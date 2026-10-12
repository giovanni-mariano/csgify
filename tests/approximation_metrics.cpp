#include "csgify/approximation.hpp"
#include "csgify/autonomy.hpp"
#include "csgify/session.hpp"

#include "approximation_internal.hpp"

#include <BRepPrimAPI_MakeBox.hxx>
#include <gp_Pnt.hxx>

#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <unistd.h>

namespace {

bool near(double actual, double expected, double tolerance = 1.0e-8) {
    return std::abs(actual - expected) <= tolerance;
}

csgify::ApproximationPolicy one_percent_policy() {
    csgify::ApproximationPolicy policy;
    policy.enabled = true;
    policy.max_global_changed_volume_fraction = 0.01;
    policy.max_body_changed_volume_fraction = 0.01;
    policy.max_action_changed_volume_fraction = 0.01;
    policy.max_global_added_volume_fraction = 0.01;
    policy.max_global_removed_volume_fraction = 0.01;
    policy.max_body_added_volume_fraction = 0.01;
    policy.max_body_removed_volume_fraction = 0.01;
    return policy;
}

} // namespace

int main(int argc, char** argv) {
    double fraction = -1.0;
    std::string error;
    assert(csgify::parse_approximation_percent("0.1%", &fraction, &error));
    assert(near(fraction, 0.001));
    assert(csgify::parse_approximation_percent(" 1% ", &fraction, &error));
    assert(near(fraction, 0.01));
    assert(csgify::parse_approximation_percent("2%", &fraction, &error));
    assert(near(fraction, 0.02));
    assert(!csgify::parse_approximation_percent("1", &fraction, &error));
    assert(!csgify::parse_approximation_percent("nan%", &fraction, &error));
    assert(!csgify::parse_approximation_percent("101%", &fraction, &error));

    const auto original = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
    const auto clipped = BRepPrimAPI_MakeBox(9.9, 10.0, 10.0).Shape();
    csgify::ApproximationMetrics clipped_metrics =
        csgify::detail::measure_shape_approximation(original, clipped);
    assert(clipped_metrics.symmetric_difference_certified);
    assert(near(clipped_metrics.before_volume, 1000.0));
    assert(near(clipped_metrics.after_volume, 990.0));
    assert(near(clipped_metrics.removed_volume, 10.0));
    assert(near(clipped_metrics.added_volume, 0.0));
    assert(near(clipped_metrics.changed_volume_fraction, 0.01));
    assert(clipped_metrics.containment_preserved);

    // Equal net volume is not equal geometry: disjoint translated boxes have
    // a 200% symmetric difference even though signed delta is zero.
    const auto translated = BRepPrimAPI_MakeBox(
        gp_Pnt(20.0, 0.0, 0.0), 10.0, 10.0, 10.0).Shape();
    csgify::ApproximationMetrics moved_metrics =
        csgify::detail::measure_shape_approximation(original, translated);
    assert(moved_metrics.symmetric_difference_certified);
    assert(near(moved_metrics.signed_volume_delta, 0.0));
    assert(near(moved_metrics.removed_volume, 1000.0));
    assert(near(moved_metrics.added_volume, 1000.0));
    assert(near(moved_metrics.changed_volume_fraction, 2.0));

    csgify::ApproximationLedger ledger;
    ledger.document_baseline_volume = 1000.0;
    csgify::ApproximationBudgetCheck check;
    assert(csgify::commit_approximation_metrics(
        ledger, one_percent_policy(), "solid:1", clipped_metrics, &check));
    assert(check.ok);
    assert(ledger.bodies.size() == 1);
    assert(near(ledger.cumulative_changed_volume, 10.0));

    // A second 1% removal cannot hide behind signed cancellation or a fresh
    // per-action budget: cumulative global/body usage becomes 2%.
    assert(!csgify::commit_approximation_metrics(
        ledger, one_percent_policy(), "solid:1", clipped_metrics, &check));
    assert(!check.ok);
    assert(check.code == "approximation_global_budget_exceeded");
    assert(near(ledger.cumulative_changed_volume, 10.0));

    assert(!csgify::check_approximation_budget(
        one_percent_policy(), ledger, "solid:2", moved_metrics).ok);

    if (argc > 1) {
        csgify::SessionPtr session = csgify::create_session();
        const csgify::DocumentId document = csgify::load_step(*session, argv[1]);
        csgify::ApproximationPolicy strict = one_percent_policy();
        strict.allow_bbox_replacement = true;
        strict.require_containment_preservation = false;
        const csgify::ApproximationCommandResult rejected =
            csgify::apply_budgeted_replacement_operation(
                *session, "solid:1", csgify::ReplacementOperation::bbox,
                strict, csgify::ReportIdMode::local);
        assert(!rejected.command.ok);
        assert(rejected.budget.code == "approximation_action_budget_exceeded");
        assert(csgify::document_revision(*session, document) == 0);
        assert(near(csgify::document_approximation_ledger(
                        *session, document).cumulative_changed_volume, 0.0));

        csgify::ApproximationPolicy permissive = strict;
        permissive.max_global_changed_volume_fraction = 0.40;
        permissive.max_body_changed_volume_fraction = 0.40;
        permissive.max_action_changed_volume_fraction = 0.40;
        permissive.max_global_added_volume_fraction = 0.40;
        permissive.max_body_added_volume_fraction = 0.40;
        const csgify::ApproximationCommandResult accepted =
            csgify::apply_budgeted_replacement_operation(
                *session, "solid:1", csgify::ReplacementOperation::bbox,
                permissive, csgify::ReportIdMode::local);
        assert(accepted.command.ok);
        assert(accepted.metrics.symmetric_difference_certified);
        assert(near(accepted.metrics.changed_volume, 250.0, 1.0e-3));
        assert(csgify::document_revision(*session, document) == 1);
        assert(near(csgify::document_approximation_ledger(
                        *session, document).cumulative_changed_volume,
                    250.0, 1.0e-3));

        assert(csgify::undo_last_mutation(*session, document).ok);
        assert(near(csgify::document_approximation_ledger(
                        *session, document).cumulative_changed_volume, 0.0));

        // Only decomposition residuals can use budgeted removal. Imported
        // solids remain an explicit modeling decision even with a 100% budget.
        csgify::ApproximationPolicy removal = permissive;
        removal.allow_sliver_removal = true;
        removal.require_topology_preservation = false;
        removal.max_global_changed_volume_fraction = 1.0;
        removal.max_body_changed_volume_fraction = 1.0;
        removal.max_action_changed_volume_fraction = 1.0;
        removal.max_global_removed_volume_fraction = 1.0;
        removal.max_body_removed_volume_fraction = 1.0;
        const csgify::ApproximationCommandResult whole_rejected =
            csgify::remove_residual_with_approximation(
                *session, "solid:1", removal, csgify::ReportIdMode::local);
        assert(!whole_rejected.command.ok);
        assert(whole_rejected.budget.code
               == "approximation_removal_requires_piece");

        const csgify::CommandResult decomposed = csgify::decompose_planar(
            *session, "solid:1", csgify::ReportIdMode::local);
        assert(decomposed.ok);
        assert(!decomposed.report.decomposed_pieces.empty());
        const std::string piece_id =
            decomposed.report.decomposed_pieces.front().id;
        const std::string source_solid_id =
            decomposed.report.decomposed_pieces.front().source_solid_id;
        const std::filesystem::path residual_context =
            std::filesystem::temp_directory_path()
            / ("csgify-residual-context-" + std::to_string(::getpid())
               + ".json");
        std::error_code remove_error;
        std::filesystem::remove(residual_context, remove_error);
        csgify::AutonomyContextOptions context_options;
        context_options.target_id = piece_id;
        assert(csgify::write_autonomy_context_json(
            *session, document, {}, context_options,
            residual_context.string()) == 0);
        std::ifstream context_input(residual_context);
        std::ostringstream context_contents;
        context_contents << context_input.rdbuf();
        const std::string context = context_contents.str();
        assert(context.find("\"kind\":\"residual_piece\"")
               != std::string::npos);
        assert(context.find("\"residual_pieces\":[{\"id\":\"")
               != std::string::npos);
        assert(context.find(piece_id) != std::string::npos);
        assert(context.find("\"owner_solid_id\":\"")
               != std::string::npos);
        assert(context.find(source_solid_id) != std::string::npos);
        std::filesystem::remove(residual_context, remove_error);
        const csgify::ApproximationCommandResult piece_removed =
            csgify::remove_residual_with_approximation(
                *session, piece_id, removal, csgify::ReportIdMode::local);
        assert(piece_removed.command.ok);
        assert(piece_removed.metrics.removed_volume > 0.0);
        assert(piece_removed.metrics.added_volume == 0.0);
        assert(csgify::document_approximation_ledger(
                   *session, document).cumulative_removed_volume > 0.0);
    }
    return 0;
}

#include "csgify/approximation.hpp"

#include "approximation_internal.hpp"
#include "occt_boolean.hpp"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Shape.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <sstream>

namespace csgify {
namespace {

double ratio(double value, double baseline) {
    return baseline > 0.0 ? value / baseline
                          : (value == 0.0 ? 0.0
                                          : std::numeric_limits<double>::infinity());
}

ApproximationLedgerEntry* find_entry(ApproximationLedger& ledger,
                                     const std::string& source_solid_id) {
    for (ApproximationLedgerEntry& entry : ledger.bodies)
        if (entry.source_solid_id == source_solid_id) return &entry;
    return nullptr;
}

const ApproximationLedgerEntry* find_entry(const ApproximationLedger& ledger,
                                           const std::string& source_solid_id) {
    for (const ApproximationLedgerEntry& entry : ledger.bodies)
        if (entry.source_solid_id == source_solid_id) return &entry;
    return nullptr;
}

bool exceeds(double value, double limit) {
    const double scale = std::max({1.0, std::abs(value), std::abs(limit)});
    return value - limit > 32.0 * std::numeric_limits<double>::epsilon() * scale;
}

ApproximationBudgetCheck reject(const std::string& code,
                                const std::string& message,
                                ApproximationBudgetCheck check) {
    check.code = code;
    check.message = message;
    return check;
}

double shape_volume(const TopoDS_Shape& shape) {
    if (shape.IsNull()) return 0.0;
    GProp_GProps props;
    BRepGProp::VolumeProperties(shape, props);
    return std::abs(props.Mass());
}

std::size_t shape_count(const TopoDS_Shape& shape, TopAbs_ShapeEnum kind) {
    std::size_t count = 0;
    for (TopExp_Explorer exp(shape, kind); exp.More(); exp.Next()) ++count;
    return count;
}

bool cut_shape(const TopoDS_Shape& left,
               const TopoDS_Shape& right,
               TopoDS_Shape* shape_out,
               std::string* error_out) {
    if (!shape_out || left.IsNull() || right.IsNull()) {
        if (error_out) *error_out = "approximation input shape is null";
        return false;
    }
    try {
        BRepAlgoAPI_Cut cut;
        detail::build_binary_boolean(cut, left, right);
        if (!cut.IsDone() || cut.HasErrors()) {
            if (error_out) *error_out = "OCCT Boolean cut did not complete";
            return false;
        }
        const TopoDS_Shape result = cut.Shape();
        if (!result.IsNull() && !BRepCheck_Analyzer(result).IsValid()) {
            if (error_out) *error_out = "OCCT Boolean difference is invalid";
            return false;
        }
        *shape_out = result;
        return true;
    } catch (const Standard_Failure& failure) {
        if (error_out) {
            const char* message = failure.what();
            *error_out = message ? message : "OCCT Boolean cut failed";
        }
        return false;
    } catch (...) {
        if (error_out) *error_out = "OCCT Boolean cut failed";
        return false;
    }
}

bool cut_volume(const TopoDS_Shape& left,
                const TopoDS_Shape& right,
                double* volume_out,
                std::string* error_out) {
    TopoDS_Shape result;
    if (!volume_out || !cut_shape(left, right, &result, error_out)) return false;
    *volume_out = shape_volume(result);
    return std::isfinite(*volume_out);
}

} // namespace

bool parse_approximation_percent(const std::string& text,
                                 double* fraction_out,
                                 std::string* error_out) {
    if (!fraction_out) {
        if (error_out) *error_out = "output pointer is null";
        return false;
    }
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    const std::size_t last = text.find_last_not_of(" \t\r\n");
    if (first == std::string::npos || text[last] != '%') {
        if (error_out) *error_out = "percentage must have an explicit '%' suffix";
        return false;
    }
    const std::string number = text.substr(first, last - first);
    if (number.empty()) {
        if (error_out) *error_out = "percentage value is empty";
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const double percent = std::strtod(number.c_str(), &end);
    while (end && *end != '\0' && std::isspace(static_cast<unsigned char>(*end)))
        ++end;
    if (errno == ERANGE || !end || *end != '\0' || !std::isfinite(percent)
        || percent < 0.0 || percent > 100.0) {
        if (error_out) *error_out = "percentage must be finite and between 0% and 100%";
        return false;
    }
    *fraction_out = percent / 100.0;
    return true;
}

ApproximationBudgetCheck check_approximation_budget(
    const ApproximationPolicy& policy,
    const ApproximationLedger& ledger,
    const std::string& source_solid_id,
    const ApproximationMetrics& metrics) {
    ApproximationBudgetCheck check;
    if (!policy.enabled) {
        return reject("approximation_disabled",
                      "Approximation requires an explicit user policy.", check);
    }
    if (!metrics.symmetric_difference_certified) {
        return reject("approximation_measurement_unknown",
                      "Symmetric-difference geometry was not certified.", check);
    }
    if (metrics.baseline_volume <= 0.0
        || ledger.document_baseline_volume <= 0.0) {
        return reject("approximation_baseline_invalid",
                      "Approximation baseline volume is unavailable.", check);
    }

    const ApproximationLedgerEntry* existing = find_entry(ledger, source_solid_id);
    const double body_baseline = existing && existing->baseline_volume > 0.0
        ? existing->baseline_volume : metrics.baseline_volume;
    const double body_changed = (existing ? existing->cumulative_changed_volume : 0.0)
        + metrics.changed_volume;
    const double body_added = (existing ? existing->cumulative_added_volume : 0.0)
        + metrics.added_volume;
    const double body_removed = (existing ? existing->cumulative_removed_volume : 0.0)
        + metrics.removed_volume;

    check.global_changed_fraction_after = ratio(
        ledger.cumulative_changed_volume + metrics.changed_volume,
        ledger.document_baseline_volume);
    check.body_changed_fraction_after = ratio(body_changed, body_baseline);
    check.global_added_fraction_after = ratio(
        ledger.cumulative_added_volume + metrics.added_volume,
        ledger.document_baseline_volume);
    check.global_removed_fraction_after = ratio(
        ledger.cumulative_removed_volume + metrics.removed_volume,
        ledger.document_baseline_volume);
    check.body_added_fraction_after = ratio(body_added, body_baseline);
    check.body_removed_fraction_after = ratio(body_removed, body_baseline);

    struct Limit { double value; double maximum; const char* code; const char* name; };
    const Limit limits[] = {
        {metrics.changed_volume_fraction, policy.max_action_changed_volume_fraction,
         "approximation_action_budget_exceeded", "action changed-volume"},
        {check.global_changed_fraction_after, policy.max_global_changed_volume_fraction,
         "approximation_global_budget_exceeded", "global changed-volume"},
        {check.body_changed_fraction_after, policy.max_body_changed_volume_fraction,
         "approximation_body_budget_exceeded", "body changed-volume"},
        {check.global_added_fraction_after, policy.max_global_added_volume_fraction,
         "approximation_global_added_budget_exceeded", "global added-volume"},
        {check.global_removed_fraction_after, policy.max_global_removed_volume_fraction,
         "approximation_global_removed_budget_exceeded", "global removed-volume"},
        {check.body_added_fraction_after, policy.max_body_added_volume_fraction,
         "approximation_body_added_budget_exceeded", "body added-volume"},
        {check.body_removed_fraction_after, policy.max_body_removed_volume_fraction,
         "approximation_body_removed_budget_exceeded", "body removed-volume"},
    };
    for (const Limit& limit : limits) {
        if (exceeds(limit.value, limit.maximum)) {
            std::ostringstream message;
            message << limit.name << " fraction " << std::setprecision(10)
                    << limit.value << " exceeds " << limit.maximum;
            return reject(limit.code, message.str(), check);
        }
    }
    check.ok = true;
    check.code = "approximation_within_budget";
    check.message = "Approximation is within every configured volume budget.";
    return check;
}

bool commit_approximation_metrics(ApproximationLedger& ledger,
                                  const ApproximationPolicy& policy,
                                  const std::string& source_solid_id,
                                  const ApproximationMetrics& metrics,
                                  ApproximationBudgetCheck* check_out) {
    const ApproximationBudgetCheck check = check_approximation_budget(
        policy, ledger, source_solid_id, metrics);
    if (check_out) *check_out = check;
    if (!check.ok) return false;
    ApproximationLedgerEntry* entry = find_entry(ledger, source_solid_id);
    if (!entry) {
        ledger.bodies.push_back({});
        entry = &ledger.bodies.back();
        entry->source_solid_id = source_solid_id;
        entry->baseline_volume = metrics.baseline_volume;
    }
    entry->cumulative_changed_volume += metrics.changed_volume;
    entry->cumulative_added_volume += metrics.added_volume;
    entry->cumulative_removed_volume += metrics.removed_volume;
    ++entry->accepted_actions;
    ledger.cumulative_changed_volume += metrics.changed_volume;
    ledger.cumulative_added_volume += metrics.added_volume;
    ledger.cumulative_removed_volume += metrics.removed_volume;
    return true;
}

namespace detail {

ApproximationMetrics measure_shape_approximation(
    const TopoDS_Shape& before,
    const TopoDS_Shape& after,
    double baseline_volume,
    double numerical_volume_tolerance,
    std::string* error_out) {
    ApproximationMetrics metrics;
    if (before.IsNull() || after.IsNull()) {
        if (error_out) *error_out = "approximation input shape is null";
        return metrics;
    }
    metrics.before_volume = shape_volume(before);
    metrics.after_volume = shape_volume(after);
    metrics.baseline_volume = baseline_volume > 0.0
        ? baseline_volume : metrics.before_volume;
    metrics.signed_volume_delta = metrics.after_volume - metrics.before_volume;
    metrics.absolute_net_volume_delta = std::abs(metrics.signed_volume_delta);
    if (!cut_volume(before, after, &metrics.removed_volume, error_out)
        || !cut_volume(after, before, &metrics.added_volume, error_out)) {
        return metrics;
    }
    const double tolerance = std::max(0.0, numerical_volume_tolerance);
    if (metrics.removed_volume <= tolerance) metrics.removed_volume = 0.0;
    if (metrics.added_volume <= tolerance) metrics.added_volume = 0.0;
    metrics.changed_volume = metrics.removed_volume + metrics.added_volume;
    metrics.changed_volume_fraction = ratio(metrics.changed_volume,
                                            metrics.baseline_volume);
    metrics.added_volume_fraction = ratio(metrics.added_volume,
                                          metrics.baseline_volume);
    metrics.removed_volume_fraction = ratio(metrics.removed_volume,
                                            metrics.baseline_volume);
    metrics.symmetric_difference_certified = true;
    metrics.containment_preserved = metrics.added_volume == 0.0;
    metrics.topology_preserved =
        shape_count(before, TopAbs_SOLID) == shape_count(after, TopAbs_SOLID)
        && shape_count(before, TopAbs_SHELL) == shape_count(after, TopAbs_SHELL);
    return metrics;
}

ApproximationMetrics measure_partition_approximation(
    const TopoDS_Shape& before,
    const std::vector<TopoDS_Shape>& after_parts,
    double baseline_volume,
    double numerical_volume_tolerance,
    std::string* error_out) {
    ApproximationMetrics metrics;
    if (before.IsNull() || after_parts.empty()) {
        if (error_out) *error_out = "approximation partition is empty";
        return metrics;
    }
    metrics.before_volume = shape_volume(before);
    metrics.baseline_volume = baseline_volume > 0.0
        ? baseline_volume : metrics.before_volume;
    TopoDS_Shape residual = before;
    for (const TopoDS_Shape& part : after_parts) {
        if (part.IsNull() || !BRepCheck_Analyzer(part).IsValid()) {
            if (error_out) *error_out = "approximation partition contains an invalid part";
            return metrics;
        }
        metrics.after_volume += shape_volume(part);
        double outside = 0.0;
        if (!cut_volume(part, before, &outside, error_out)) return metrics;
        metrics.added_volume += outside;
        TopoDS_Shape next;
        if (!cut_shape(residual, part, &next, error_out)) return metrics;
        residual = next;
    }
    metrics.removed_volume = shape_volume(residual);
    metrics.signed_volume_delta = metrics.after_volume - metrics.before_volume;
    metrics.absolute_net_volume_delta = std::abs(metrics.signed_volume_delta);
    const double tolerance = std::max(0.0, numerical_volume_tolerance);
    if (metrics.removed_volume <= tolerance) metrics.removed_volume = 0.0;
    if (metrics.added_volume <= tolerance) metrics.added_volume = 0.0;
    metrics.changed_volume = metrics.removed_volume + metrics.added_volume;
    metrics.changed_volume_fraction = ratio(metrics.changed_volume,
                                            metrics.baseline_volume);
    metrics.added_volume_fraction = ratio(metrics.added_volume,
                                          metrics.baseline_volume);
    metrics.removed_volume_fraction = ratio(metrics.removed_volume,
                                            metrics.baseline_volume);
    metrics.symmetric_difference_certified = true;
    metrics.containment_preserved = metrics.added_volume == 0.0;
    // Shattering intentionally changes the number of solids while preserving
    // their union, so topology preservation is expressed by zero symmetric
    // difference rather than equal child counts.
    metrics.topology_preserved = metrics.changed_volume == 0.0;
    return metrics;
}

} // namespace detail
} // namespace csgify

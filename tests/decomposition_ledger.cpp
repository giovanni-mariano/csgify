#include "decomposition_ledger.hpp"
#include "engine_internal.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {

int fail(const std::string& message) {
    std::cerr << "decomposition_ledger: " << message << '\n';
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return fail("expected l_shape STEP path");
    csgify::ConversionOptions options;
    options.try_planar_decomposition = false;
    csgify::CadModel model = csgify::load_step_model(argv[1], options);
    if (!model.loaded()) return fail("could not load fixture");
    csgify::ConversionReport report = csgify::analyze_model(model, options);
    csgify::detail::detect_solid_overlaps(model, options, report);
    csgify::PerformanceMetrics performance;
    csgify::detail::DecompositionProgressLedger ledger =
        csgify::detail::build_decomposition_progress_ledger(
            model, report, options, &performance);
    if (ledger.bodies().size() != 1 || ledger.state().residual_count != 1
        || ledger.state().exact_body_count != 0
        || performance.body_certificate_cache_misses != 1) {
        return fail("unexpected initial ledger state");
    }
    const csgify::detail::BodyProgressCertificate* parent =
        ledger.find("solid:1");
    if (!parent || !parent->valid || parent->exact_csg_supported) {
        return fail("parent certificate missing or incorrectly exact");
    }
    csgify::PerformanceMetrics reuse_performance;
    const csgify::detail::DecompositionProgressLedger reused =
        csgify::detail::build_decomposition_progress_ledger(
            model, report, options, &reuse_performance, &ledger);
    if (reuse_performance.body_certificate_cache_hits != 1
        || reuse_performance.body_certificate_cache_misses != 0
        || !csgify::detail::progress_states_equivalent(
            reused.state(), ledger.state())) {
        return fail("unchanged authoritative report did not reuse its body certificate");
    }

    csgify::detail::BodyProgressCertificate first;
    first.body_id = "piece:test:1";
    first.source_solid_id = "solid:1";
    first.volume = parent->volume * 0.6;
    first.face_count = 6;
    first.exact_csg_supported = true;
    first.valid = true;
    csgify::detail::BodyProgressCertificate second = first;
    second.body_id = "piece:test:2";
    second.volume = parent->volume - first.volume;
    std::vector<csgify::detail::BodyProgressCertificate> children{first, second};

    csgify::DecompositionProgressState projected;
    if (!ledger.project_replacement("solid:1", children, 0, &projected)) {
        return fail("valid replacement projection failed");
    }
    if (projected.residual_count != 0 || projected.exact_body_count != 2
        || projected.active_piece_count != 2
        || std::abs(projected.total_volume - report.total_volume) > 1.0e-9) {
        return fail("projected state is not algebraically conserved");
    }
    if (!ledger.commit_replacement("solid:1", children, 0)
        || !csgify::detail::progress_states_equivalent(
            ledger.state(), projected)) {
        return fail("committed ledger state differs from projection");
    }
    if (ledger.find("solid:1") || !ledger.find("piece:test:1")
        || !ledger.find("piece:test:2")) {
        return fail("ledger body map was not replaced atomically");
    }

    csgify::detail::BodyProgressCertificate invalid = first;
    invalid.body_id = "piece:bad";
    invalid.valid = false;
    if (ledger.project_replacement("piece:test:1", {invalid}, 0,
                                   &projected)) {
        return fail("invalid child certificate was accepted");
    }
    if (ledger.project_replacement("missing", children, 0, &projected)) {
        return fail("missing parent was accepted");
    }
    csgify::DecompositionProgressState one_overlap = ledger.state();
    csgify::DecompositionProgressState split_overlap = one_overlap;
    one_overlap.overlap_error_count = 1;
    split_overlap.overlap_error_count = 2;
    if (!csgify::detail::progress_states_equivalent(
            one_overlap, split_overlap)) {
        return fail("pre-existing overlap fan-out changed blocker state");
    }
    split_overlap.overlap_error_count = 0;
    if (csgify::detail::progress_states_equivalent(
            one_overlap, split_overlap)) {
        return fail("overlap blocker disappearance was ignored");
    }
    return 0;
}

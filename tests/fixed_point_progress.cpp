#include "csgify/fixed_point.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

int fail(const char* message) {
    std::cerr << "fixed_point_progress: " << message << '\n';
    return EXIT_FAILURE;
}

} // namespace

int main() {
    csgify::FixedPointDecompositionPolicy policy;
    policy.progress_volume_epsilon = 1.0e-6;
    policy.absolute_volume_epsilon = 1.0e-9;

    csgify::DecompositionProgressState base;
    base.unsupported_face_count = 0;
    base.residual_volume = 20.0;
    base.halfspace_violation_count = 10;
    base.residual_count = 2;
    base.active_piece_count = 3;
    const auto score = csgify::make_decomposition_progress_score(base, 100.0,
                                                                  policy);

    auto expect_better = [&](csgify::DecompositionProgressState value,
                             const char* message) {
        const auto other = csgify::make_decomposition_progress_score(
            value, 100.0, policy);
        if (!csgify::decomposition_progress_less(other, score)) {
            std::cerr << "fixed_point_progress: " << message << '\n';
            std::exit(EXIT_FAILURE);
        }
    };

    auto value = base;
    value.unsupported_face_count = 1;
    value.residual_volume = 0.0;
    if (csgify::decomposition_progress_less(
            csgify::make_decomposition_progress_score(value, 100.0, policy),
            score)) {
        return fail("unsupported regression was rewarded");
    }
    value = base; value.residual_volume = 19.0; value.active_piece_count = 100;
    expect_better(value, "residual-volume reduction did not dominate pieces");
    value = base; value.halfspace_violation_count = 9;
    expect_better(value, "complexity reduction did not count as progress");
    value = base; value.residual_count = 1;
    expect_better(value, "residual-count reduction did not count as progress");
    value = base; value.active_piece_count = 2;
    expect_better(value, "piece-count reduction did not break a tie");

    value = base; value.residual_volume += 0.00001;
    const auto jitter = csgify::make_decomposition_progress_score(value, 100.0,
                                                                   policy);
    if (jitter.residual_volume_bucket != score.residual_volume_bucket) {
        return fail("sub-bucket floating jitter changed the score");
    }

    value = base;
    value.residual_volume = std::numeric_limits<double>::quiet_NaN();
    const auto invalid = csgify::make_decomposition_progress_score(
        value, 100.0, policy);
    if (invalid.residual_volume_bucket
        != std::numeric_limits<std::uint64_t>::max()) {
        return fail("invalid volume was not saturated");
    }
    return EXIT_SUCCESS;
}

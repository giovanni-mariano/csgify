#include "engine_model.hpp"
#include "step_export.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

int fail(const char* message) {
    std::cerr << "synthetic_boolean_step: " << message << "\n";
    return EXIT_FAILURE;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        return fail("usage: synthetic_boolean_step INPUT_STEP OUTPUT_STEP");
    }

    csgify::CadModel model = csgify::load_step_model(argv[1]);
    if (!model.loaded()) {
        return fail("fixture did not load");
    }
    if (!csgify::remove_solid(model, "solid:1")) {
        return fail("fixture solid could not be suppressed");
    }

    csgify::BBox left;
    left.valid = true;
    left.xmin = 0.0; left.xmax = 1.0;
    left.ymin = 0.0; left.ymax = 1.0;
    left.zmin = 0.0; left.zmax = 1.0;
    csgify::BBox right = left;
    right.xmin = 1.0;
    right.xmax = 2.0;

    const std::string left_id = csgify::add_synthetic_box(model, left);
    const std::string right_id = csgify::add_synthetic_box(model, right);
    csgify::add_synthetic_union(model, left_id, right_id);

    if (csgify::export_step(model, argv[2]) != 0) {
        return fail("Boolean STEP export failed");
    }

    const csgify::ConversionReport report = csgify::inspect_step_file(argv[2]);
    if (!report.ok() || report.solids_seen != 1) {
        return fail("exported Boolean STEP did not re-import as one valid solid");
    }
    if (std::abs(report.total_volume - 2.0) > 1.0e-7) {
        return fail("exported Boolean STEP volume differs from exact union");
    }
    return EXIT_SUCCESS;
}

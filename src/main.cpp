#include "cad2csg/engine.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

const char* severity_name(cad2csg::DiagnosticSeverity severity) {
    switch (severity) {
    case cad2csg::DiagnosticSeverity::info:
        return "info";
    case cad2csg::DiagnosticSeverity::warning:
        return "warning";
    case cad2csg::DiagnosticSeverity::error:
        return "error";
    }
    return "unknown";
}

void usage(const char* argv0) {
    std::cerr << "usage: " << argv0 << " input.step output.i\n"
              << "       " << argv0 << " inspect input.step --report report.json\n"
              << "       " << argv0 << " simplify input.step <action> <target> --out output.i [options]\n"
              << "         actions on a solid id (e.g. solid:1):\n"
              << "           --replace-solid-bbox            --replace-solid-bbox-cylinder\n"
              << "           --replace-solid-fitted-cylinder --replace-solid-fitted-sphere\n"
              << "           --replace-solid-fitted-cone     --decompose-planar\n"
              << "           --remove-solid\n"
              << "         actions on a face id (e.g. face:1.4, or a comma list face:1.4,face:1.5):\n"
              << "           --replace-face-with-plane       --replace-face-with-cylinder\n"
              << "           --replace-face-with-sphere      --replace-face-with-cone\n"
              << "         options:\n"
              << "           --max-abs-delta value     (solid replacement, volume guard)\n"
              << "           --max-rel-delta value     (solid replacement, volume guard)\n"
              << "           --max-deviation value     (face replacement, distance guard)\n"
              << "           --report report.json\n"
              << "       " << argv0 << " auto-fit input.step --out output.i [--max-deviation X] [--report report.json]\n"
              << "       " << argv0 << " volume input.step\n"
              << "       " << argv0 << " volume original.step --compare candidate.step [--abs-tol value] [--rel-tol value]\n"
              << "       " << argv0 << " --version\n";
}

void print_summary(const cad2csg::ConversionReport& report) {
    std::cerr << "solids: " << report.solids_converted << "/"
              << report.solids_seen
              << " converted, " << report.solids_requiring_decomposition
              << " need decomposition, "
              << report.decomposition_pieces << " decomposition pieces"
              << ", faces: " << report.analytic_faces << "/"
              << report.faces_seen << " analytic"
              << ", splines: " << report.spline_faces
              << ", volume: " << report.total_volume << '\n';
}

void print_diagnostics(const cad2csg::ConversionReport& report) {
    for (const cad2csg::Diagnostic& diagnostic : report.diagnostics) {
        std::cerr << severity_name(diagnostic.severity) << " "
                  << diagnostic.code;
        if (!diagnostic.shape_label.empty()) {
            std::cerr << " " << diagnostic.shape_label;
        }
        std::cerr << ": " << diagnostic.message << '\n';
    }
}

struct SolidActionEntry {
    const char* flag;
    cad2csg::ReplacementOperation op;
};

constexpr SolidActionEntry kSolidActions[] = {
    {"--replace-solid-bbox",            cad2csg::ReplacementOperation::bbox},
    {"--replace-solid-bbox-cylinder",   cad2csg::ReplacementOperation::bbox_cylinder},
    {"--replace-solid-fitted-cylinder", cad2csg::ReplacementOperation::fitted_cylinder},
    {"--replace-solid-fitted-sphere",   cad2csg::ReplacementOperation::fitted_sphere},
    {"--replace-solid-fitted-cone",     cad2csg::ReplacementOperation::fitted_cone},
};

bool operation_from_action(const std::string& action,
                           cad2csg::ReplacementOperation* operation) {
    for (const SolidActionEntry& entry : kSolidActions) {
        if (action == entry.flag) {
            *operation = entry.op;
            return true;
        }
    }
    return false;
}

struct FaceActionEntry {
    const char* flag;
    cad2csg::SplineFitKind kind;
};

constexpr FaceActionEntry kFaceActions[] = {
    {"--replace-face-with-plane",    cad2csg::SplineFitKind::plane},
    {"--replace-face-with-sphere",   cad2csg::SplineFitKind::sphere},
    {"--replace-face-with-cylinder", cad2csg::SplineFitKind::cylinder},
    {"--replace-face-with-cone",     cad2csg::SplineFitKind::cone},
};

bool face_kind_from_action(const std::string& action,
                           cad2csg::SplineFitKind* kind) {
    for (const FaceActionEntry& entry : kFaceActions) {
        if (action == entry.flag) {
            *kind = entry.kind;
            return true;
        }
    }
    return false;
}

[[noreturn]] void fail_invalid_number(const char* name, const char* text) {
    std::cerr << "error invalid_number " << name << ": " << text << '\n';
    std::exit(EXIT_FAILURE);
}

double parse_double_arg(const char* text, const char* name) {
    try {
        std::size_t used = 0;
        const double value = std::stod(text, &used);
        if (text[used] != '\0') {
            throw std::invalid_argument(name);
        }
        return value;
    } catch (const std::exception&) {
        fail_invalid_number(name, text);
    }
}

// A small token-stream parser. Each `take_*` consumes both the flag and its
// value if the current token matches the requested flag; otherwise leaves
// the cursor alone and returns false. Subcommands loop over the scanner and
// fail on the first unrecognized token.
class OptScanner {
public:
    OptScanner(int argc, char** argv, int start)
        : argc_(argc), argv_(argv), pos_(start) {}

    bool done() const { return pos_ >= argc_; }
    const char* peek() const { return pos_ < argc_ ? argv_[pos_] : nullptr; }
    int pos() const { return pos_; }

    bool take_string(const char* flag, std::string* out) {
        if (!matches(flag)) {
            return false;
        }
        *out = argv_[pos_ + 1];
        pos_ += 2;
        return true;
    }

    bool take_double(const char* flag, double* out, bool* present = nullptr) {
        if (!matches(flag)) {
            return false;
        }
        *out = parse_double_arg(argv_[pos_ + 1], flag);
        if (present) {
            *present = true;
        }
        pos_ += 2;
        return true;
    }

private:
    bool matches(const char* flag) const {
        if (pos_ + 1 >= argc_) {
            return false;
        }
        return std::strcmp(argv_[pos_], flag) == 0;
    }

    int argc_;
    char** argv_;
    int pos_;
};

int run_volume_command(int argc, char** argv) {
    if (argc < 3) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    const std::string input = argv[2];
    std::string compare_path;
    double abs_tol = 0.0;
    double rel_tol = 0.0;

    OptScanner scan(argc, argv, 3);
    while (!scan.done()) {
        if (scan.take_string("--compare", &compare_path)) continue;
        if (scan.take_double("--abs-tol", &abs_tol)) continue;
        if (scan.take_double("--rel-tol", &rel_tol)) continue;
        std::cerr << "error unrecognized_option: " << scan.peek() << '\n';
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    cad2csg::ConversionOptions options;
    if (compare_path.empty()) {
        cad2csg::ConversionReport report = cad2csg::inspect_step_file(input, options);
        std::cout << "volume " << report.total_volume << '\n';
        std::cout << "solids " << report.solid_volumes.size() << '\n';
        for (const cad2csg::SolidVolume& solid : report.solid_volumes) {
            std::cout << solid.shape_label << " " << solid.volume << '\n';
        }
        return report.solids_seen > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    cad2csg::VolumeComparison comparison =
        cad2csg::compare_step_volumes(input, compare_path, abs_tol, rel_tol, options);
    std::cout << "original_volume " << comparison.original.total_volume << '\n';
    std::cout << "candidate_volume " << comparison.candidate.total_volume << '\n';
    std::cout << "absolute_delta " << comparison.absolute_delta << '\n';
    std::cout << "relative_delta " << comparison.relative_delta << '\n';
    std::cout << "absolute_tolerance " << comparison.absolute_tolerance << '\n';
    std::cout << "relative_tolerance " << comparison.relative_tolerance << '\n';
    std::cout << "within_tolerance "
              << (comparison.within_tolerance ? "true" : "false") << '\n';
    return comparison.within_tolerance ? EXIT_SUCCESS : EXIT_FAILURE;
}

int run_simplify_command(int argc, char** argv) {
    if (argc < 7) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    const std::string input_path = argv[2];
    const std::string action = argv[3];
    const std::string target_id = argv[4];
    if (std::strcmp(argv[5], "--out") != 0) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    const std::string output_path = argv[6];

    cad2csg::ReplacementLimits replacement_limits;
    std::string report_path;
    bool has_max_deviation = false;
    double max_deviation = 0.0;

    OptScanner scan(argc, argv, 7);
    while (!scan.done()) {
        if (scan.take_double("--max-abs-delta",
                             &replacement_limits.max_absolute_delta,
                             &replacement_limits.has_absolute_delta)) continue;
        if (scan.take_double("--max-rel-delta",
                             &replacement_limits.max_relative_delta,
                             &replacement_limits.has_relative_delta)) continue;
        if (scan.take_double("--max-deviation", &max_deviation, &has_max_deviation)) continue;
        if (scan.take_string("--report", &report_path)) continue;
        std::cerr << "error unrecognized_option: " << scan.peek() << '\n';
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    std::cerr << "cad2csg: OCCT " << cad2csg::occt_version()
              << ", Alea " << alea_version() << '\n';

    cad2csg::ConversionOptions options;
    cad2csg::CadModel model = cad2csg::load_step_model(input_path, options);
    if (!model.loaded()) {
        cad2csg::AnalysisReport report = cad2csg::analyze_model(model, options);
        print_diagnostics(report);
        return EXIT_FAILURE;
    }

    const bool has_volume_limits =
        replacement_limits.has_absolute_delta || replacement_limits.has_relative_delta;

    cad2csg::ReplacementOperation operation;
    bool action_ok = false;
    if (operation_from_action(action, &operation)) {
        if (has_volume_limits) {
            cad2csg::ReplacementCheck check;
            action_ok = cad2csg::apply_checked_replacement_operation(
                model, target_id, operation, replacement_limits, &check, options);
            if (!action_ok && !check.diagnostic.code.empty()) {
                std::cerr << severity_name(check.diagnostic.severity) << " "
                          << check.diagnostic.code;
                if (!check.diagnostic.shape_label.empty()) {
                    std::cerr << " " << check.diagnostic.shape_label;
                }
                std::cerr << ": " << check.diagnostic.message << '\n';
                return EXIT_FAILURE;
            }
        } else {
            action_ok = cad2csg::apply_replacement_operation(
                model, target_id, operation, options);
        }
    } else if (cad2csg::SplineFitKind kind = cad2csg::SplineFitKind::none;
               face_kind_from_action(action, &kind)) {
        if (has_volume_limits) {
            std::cerr << "error replacement_limit_not_applicable: volume delta limits "
                      << "apply to solid replacement commands, not face replacement\n";
            return EXIT_FAILURE;
        }
        // target_id may be a single face:N.M or a comma-separated list.
        action_ok = true;
        std::size_t cursor = 0;
        while (cursor <= target_id.size()) {
            const std::size_t comma = target_id.find(',', cursor);
            const std::size_t end = comma == std::string::npos ? target_id.size() : comma;
            const std::string id = target_id.substr(cursor, end - cursor);
            if (id.empty()) {
                std::cerr << "error invalid_face_id: empty entry in face list\n";
                return EXIT_FAILURE;
            }
            cad2csg::Diagnostic face_diag;
            const bool ok = cad2csg::replace_face_with_fit(
                model, id, kind, has_max_deviation, max_deviation,
                &face_diag, options);
            if (!ok) {
                std::cerr << severity_name(face_diag.severity) << " "
                          << face_diag.code;
                if (!face_diag.shape_label.empty()) {
                    std::cerr << " " << face_diag.shape_label;
                }
                std::cerr << ": " << face_diag.message << '\n';
                return EXIT_FAILURE;
            }
            if (comma == std::string::npos) {
                break;
            }
            cursor = comma + 1;
        }
    } else if (action == "--remove-solid") {
        if (has_volume_limits) {
            std::cerr << "error replacement_limit_not_applicable: volume delta limits "
                      << "apply to replacement commands, not remove-solid\n";
            return EXIT_FAILURE;
        }
        action_ok = cad2csg::remove_solid(model, target_id);
    } else if (action == "--decompose-planar") {
        if (has_volume_limits) {
            std::cerr << "error replacement_limit_not_applicable: volume delta limits "
                      << "apply to replacement commands, not decompose-planar\n";
            return EXIT_FAILURE;
        }
        action_ok = cad2csg::decompose_solid_by_planar_faces(model, target_id, options);
    } else {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (!action_ok) {
        cad2csg::AnalysisReport report = cad2csg::analyze_model(model, options);
        print_diagnostics(report);
        return EXIT_FAILURE;
    }

    cad2csg::ConversionResult result = cad2csg::convert_model(model, options);
    print_diagnostics(result.report);
    print_summary(result.report);

    if (!report_path.empty()
        && cad2csg::write_report_json(result.report, report_path) != 0) {
        std::cerr << "error report_write_failed: " << report_path << '\n';
        return EXIT_FAILURE;
    }

    if (!result.report.ok() || result.report.solids_converted == 0) {
        return EXIT_FAILURE;
    }

    if (cad2csg::export_mcnp(result.system.get(), output_path) != 0) {
        std::cerr << "error mcnp_export_failed: " << alea_error() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

int run_auto_fit_command(int argc, char** argv) {
    if (argc < 5) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    const std::string input_path = argv[2];
    if (std::strcmp(argv[3], "--out") != 0) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    const std::string output_path = argv[4];

    std::string report_path;
    bool has_max_deviation = false;
    double max_deviation = 0.0;

    OptScanner scan(argc, argv, 5);
    while (!scan.done()) {
        if (scan.take_double("--max-deviation", &max_deviation, &has_max_deviation)) continue;
        if (scan.take_string("--report", &report_path)) continue;
        std::cerr << "error unrecognized_option: " << scan.peek() << '\n';
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    std::cerr << "cad2csg: OCCT " << cad2csg::occt_version()
              << ", Alea " << alea_version() << '\n';

    cad2csg::ConversionOptions options;
    cad2csg::CadModel model = cad2csg::load_step_model(input_path, options);
    if (!model.loaded()) {
        cad2csg::AnalysisReport report = cad2csg::analyze_model(model, options);
        print_diagnostics(report);
        return EXIT_FAILURE;
    }

    const cad2csg::AutoFitSummary summary =
        cad2csg::auto_fit_splines(model, has_max_deviation, max_deviation, options);
    std::cerr << "auto-fit: " << summary.applied << "/" << summary.spline_faces_seen
              << " spline faces replaced, "
              << summary.skipped_over_budget << " over budget, "
              << summary.skipped_no_fit << " no candidate\n";

    cad2csg::ConversionResult result = cad2csg::convert_model(model, options);
    print_diagnostics(result.report);
    print_summary(result.report);

    if (!report_path.empty()
        && cad2csg::write_report_json(result.report, report_path) != 0) {
        std::cerr << "error report_write_failed: " << report_path << '\n';
        return EXIT_FAILURE;
    }

    if (!result.report.ok() || result.report.solids_converted == 0) {
        return EXIT_FAILURE;
    }
    if (cad2csg::export_mcnp(result.system.get(), output_path) != 0) {
        std::cerr << "error mcnp_export_failed: " << alea_error() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

int run_inspect_command(int argc, char** argv) {
    if (argc < 3) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    const std::string input_path = argv[2];
    std::string report_path;

    OptScanner scan(argc, argv, 3);
    while (!scan.done()) {
        if (scan.take_string("--report", &report_path)) continue;
        std::cerr << "error unrecognized_option: " << scan.peek() << '\n';
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (report_path.empty()) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    std::cerr << "cad2csg: OCCT " << cad2csg::occt_version()
              << ", Alea " << alea_version() << '\n';

    cad2csg::ConversionOptions options;
    cad2csg::ConversionReport report =
        cad2csg::inspect_step_file(input_path, options);
    print_diagnostics(report);
    print_summary(report);

    if (cad2csg::write_report_json(report, report_path) != 0) {
        std::cerr << "error report_write_failed: " << report_path << '\n';
        return EXIT_FAILURE;
    }
    return report.ok() ? EXIT_SUCCESS : EXIT_FAILURE;
}

int run_default_convert(int argc, char** argv) {
    if (argc != 3) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    std::cerr << "cad2csg: OCCT " << cad2csg::occt_version()
              << ", Alea " << alea_version() << '\n';

    cad2csg::ConversionOptions options;
    cad2csg::ConversionResult result =
        cad2csg::convert_step_file(argv[1], options);

    print_diagnostics(result.report);
    print_summary(result.report);

    if (!result.report.ok() || result.report.solids_converted == 0) {
        return EXIT_FAILURE;
    }

    if (cad2csg::export_mcnp(result.system.get(), argv[2]) != 0) {
        std::cerr << "error mcnp_export_failed: " << alea_error() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
        std::cout << "cad2csg: OCCT " << cad2csg::occt_version()
                  << ", Alea " << alea_version() << '\n';
        return EXIT_SUCCESS;
    }
    if (argc < 2) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    const std::string verb = argv[1];
    if (verb == "inspect") return run_inspect_command(argc, argv);
    if (verb == "volume")  return run_volume_command(argc, argv);
    if (verb == "simplify") return run_simplify_command(argc, argv);
    if (verb == "auto-fit") return run_auto_fit_command(argc, argv);

    return run_default_convert(argc, argv);
}

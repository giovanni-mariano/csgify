#include "cad2csg/engine.hpp"
#include "cad2csg/session.hpp"

#include "engine_model.hpp"
#include "lua_runner.hpp"
#include "recipe.hpp"
#include "replay.hpp"

#include <alea.h>
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
              << "       " << argv0 << " dump-assembly input.step\n"
              << "       " << argv0 << " decompose-suggest input.step solid:N [--report proposals.json]\n"
              << "       " << argv0 << " decompose-apply input.step --out output.i\n"
              << "                                  [--policy auto|conservative] [--max-depth N]\n"
              << "                                  [--allow-medium-confidence-auto] [--allow-bbox-fallback-auto]\n"
              << "                                  [--log decisions.json] [--out-step simplified.step] [--report report.json]\n"
              << "       " << argv0 << " compact decisions.json --out recipe.lua\n"
              << "       " << argv0 << " replay recipe.json --step input.step --out output.i --report replay.json [--out-step simplified.step]\n"
              << "       " << argv0 << " simplify input.step <action> <target> --out output.i [options]\n"
              << "         actions on a solid id (e.g. solid:1):\n"
              << "           --replace-solid-bbox            --replace-solid-bbox-cylinder\n"
              << "           --replace-solid-fitted-cylinder --replace-solid-fitted-sphere\n"
              << "           --replace-solid-fitted-cone     --decompose-planar\n"
              << "           --auto-decompose-bsp\n"
              << "           --apply-decomposition-candidate --candidate candidate:N\n"
              << "           --decompose-by-face --by-face face:N.M\n"
              << "           --remove-solid\n"
              << "         actions on a face id (e.g. face:1.4, or a comma list face:1.4,face:1.5):\n"
              << "           --replace-face-with-plane       --replace-face-with-cylinder\n"
              << "           --replace-face-with-sphere      --replace-face-with-cone\n"
              << "         options:\n"
              << "           --max-abs-delta value     (solid replacement, volume guard)\n"
              << "           --max-rel-delta value     (solid replacement, volume guard)\n"
              << "           --max-deviation value     (face replacement, distance guard)\n"
              << "           --max-depth value         (safe BSP auto-decomposition)\n"
              << "           --allow-bbox-fallback-auto\n"
              << "           --allow-medium-confidence-auto\n"
              << "           --report report.json\n"
              << "           --proposal-report proposals.json\n"
              << "           --log decisions.json      (append-only decision log)\n"
              << "           --out-step simplified.step (as-simplified geometry for review)\n"
              << "       " << argv0 << " auto-fit input.step --out output.i [--max-deviation X] [--report report.json]\n"
              << "       " << argv0 << " lua script.lua [args...]\n"
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

    bool take_flag(const char* flag) {
        if (pos_ >= argc_ || std::strcmp(argv_[pos_], flag) != 0) {
            return false;
        }
        ++pos_;
        return true;
    }

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
    cad2csg::SessionPtr session = cad2csg::create_session(options);
    if (compare_path.empty()) {
        const cad2csg::DocumentId document_id = cad2csg::load_step(*session, input);
        cad2csg::CommandResult analysis =
            cad2csg::analyze(*session, document_id, cad2csg::ReportIdMode::local);
        const cad2csg::ConversionReport& report = analysis.report;
        std::cout << "volume " << report.total_volume << '\n';
        std::cout << "solids " << report.solid_volumes.size() << '\n';
        for (const cad2csg::SolidVolume& solid : report.solid_volumes) {
            std::cout << solid.shape_label << " " << solid.volume << '\n';
        }
        return report.solids_seen > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    const cad2csg::DocumentId original_id = cad2csg::load_step(*session, input);
    const cad2csg::DocumentId candidate_id = cad2csg::load_step(*session, compare_path);
    cad2csg::VolumeComparison comparison =
        cad2csg::compare_document_volumes(*session,
                                          original_id,
                                          candidate_id,
                                          abs_tol,
                                          rel_tol,
                                          cad2csg::ReportIdMode::local);
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

int run_decompose_suggest_command(int argc, char** argv) {
    if (argc < 4) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    const std::string input_path = argv[2];
    const std::string target_id = argv[3];
    std::string report_path;

    OptScanner scan(argc, argv, 4);
    while (!scan.done()) {
        if (scan.take_string("--report", &report_path)) continue;
        std::cerr << "error unrecognized_option: " << scan.peek() << '\n';
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    cad2csg::ConversionOptions options;
    cad2csg::SessionPtr session = cad2csg::create_session(options);
    cad2csg::load_step(*session, input_path);
    cad2csg::DecompositionProposal proposal =
        cad2csg::propose_decomposition(*session,
                                       target_id,
                                       cad2csg::ReportIdMode::local);

    if (!report_path.empty()) {
        if (cad2csg::write_decomposition_proposal_json(proposal, report_path) != 0) {
            std::cerr << "error report_write_failed: " << report_path << '\n';
            return EXIT_FAILURE;
        }
    } else {
        std::cout << cad2csg::decomposition_proposal_json_string(proposal);
    }

    std::cerr << "decomposition candidates: " << proposal.candidates.size()
              << ", best: "
              << (proposal.best_candidate_id.empty()
                  ? std::string("-")
                  : proposal.best_candidate_id)
              << ", ok: " << (proposal.ok ? "true" : "false") << '\n';
    return proposal.ok ? EXIT_SUCCESS : EXIT_FAILURE;
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
    std::string proposal_report_path;
    std::string log_path;
    std::string step_out_path;
    std::string by_face_id;
    std::string candidate_id;
    double max_depth_value = 4.0;
    cad2csg::DecompositionPolicy decomposition_policy;
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
        if (scan.take_double("--max-depth", &max_depth_value)) continue;
        if (scan.take_string("--report", &report_path)) continue;
        if (scan.take_string("--proposal-report", &proposal_report_path)) continue;
        if (scan.take_string("--log", &log_path)) continue;
        if (scan.take_string("--out-step", &step_out_path)) continue;
        if (scan.take_string("--by-face", &by_face_id)) continue;
        if (scan.take_string("--candidate", &candidate_id)) continue;
        if (scan.take_flag("--allow-bbox-fallback-auto")) {
            decomposition_policy.allow_bbox_fallback_auto = true;
            continue;
        }
        if (scan.take_flag("--allow-medium-confidence-auto")) {
            decomposition_policy.allow_medium_confidence_auto = true;
            continue;
        }
        std::cerr << "error unrecognized_option: " << scan.peek() << '\n';
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    std::cerr << "cad2csg: OCCT " << cad2csg::occt_version()
              << ", Alea " << alea_version() << '\n';

    cad2csg::ConversionOptions options;
    cad2csg::SessionPtr session = cad2csg::create_session(options);
    if (!log_path.empty()) {
        try {
            cad2csg::attach_decision_log(*session, log_path, input_path);
        } catch (const std::exception& e) {
            std::cerr << "error decision_log_open_failed: " << e.what() << '\n';
            return EXIT_FAILURE;
        }
    }
    const cad2csg::DocumentId document_id = cad2csg::load_step(*session, input_path);

    const bool has_volume_limits =
        replacement_limits.has_absolute_delta || replacement_limits.has_relative_delta;

    cad2csg::ReplacementOperation operation;
    cad2csg::CommandResult action_result;
    if (operation_from_action(action, &operation)) {
        if (has_volume_limits) {
            cad2csg::ReplacementCommandResult replacement =
                cad2csg::apply_checked_replacement_operation(
                    *session,
                    target_id,
                    operation,
                    replacement_limits,
                    cad2csg::ReportIdMode::local);
            action_result = replacement.command;
            if (!action_result.ok && !replacement.check.diagnostic.code.empty()) {
                std::cerr << severity_name(replacement.check.diagnostic.severity) << " "
                          << replacement.check.diagnostic.code;
                if (!replacement.check.diagnostic.shape_label.empty()) {
                    std::cerr << " " << replacement.check.diagnostic.shape_label;
                }
                std::cerr << ": " << replacement.check.diagnostic.message << '\n';
                return EXIT_FAILURE;
            }
        } else {
            action_result = cad2csg::apply_replacement_operation(
                *session, target_id, operation, cad2csg::ReportIdMode::local);
        }
    } else if (cad2csg::SplineFitKind kind = cad2csg::SplineFitKind::none;
               face_kind_from_action(action, &kind)) {
        if (has_volume_limits) {
            std::cerr << "error replacement_limit_not_applicable: volume delta limits "
                      << "apply to solid replacement commands, not face replacement\n";
            return EXIT_FAILURE;
        }
        // target_id may be a single face:N.M or a comma-separated list.
        bool action_ok = true;
        std::size_t cursor = 0;
        while (cursor <= target_id.size()) {
            const std::size_t comma = target_id.find(',', cursor);
            const std::size_t end = comma == std::string::npos ? target_id.size() : comma;
            const std::string id = target_id.substr(cursor, end - cursor);
            if (id.empty()) {
                std::cerr << "error invalid_face_id: empty entry in face list\n";
                return EXIT_FAILURE;
            }
            action_result = cad2csg::replace_face_with_fit(
                *session,
                id,
                kind,
                has_max_deviation,
                max_deviation,
                cad2csg::ReportIdMode::local);
            if (!action_result.ok) {
                const cad2csg::Diagnostic face_diag =
                    action_result.report.diagnostics.empty()
                        ? cad2csg::Diagnostic{}
                        : action_result.report.diagnostics.back();
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
        if (!action_ok) {
            return EXIT_FAILURE;
        }
    } else if (action == "--remove-solid") {
        if (has_volume_limits) {
            std::cerr << "error replacement_limit_not_applicable: volume delta limits "
                      << "apply to replacement commands, not remove-solid\n";
            return EXIT_FAILURE;
        }
        action_result = cad2csg::remove_solid(
            *session, target_id, cad2csg::ReportIdMode::local);
    } else if (action == "--decompose-planar") {
        if (has_volume_limits) {
            std::cerr << "error replacement_limit_not_applicable: volume delta limits "
                      << "apply to replacement commands, not decompose-planar\n";
            return EXIT_FAILURE;
        }
        action_result = cad2csg::decompose_planar(
            *session, target_id, cad2csg::ReportIdMode::local);
    } else if (action == "--decompose-by-face") {
        if (has_volume_limits) {
            std::cerr << "error replacement_limit_not_applicable: volume delta limits "
                      << "apply to replacement commands, not decompose-by-face\n";
            return EXIT_FAILURE;
        }
        if (by_face_id.empty()) {
            std::cerr << "error missing_by_face: --decompose-by-face requires --by-face face:N.M\n";
            return EXIT_FAILURE;
        }
        action_result = cad2csg::decompose_by_face(
            *session, target_id, by_face_id, cad2csg::ReportIdMode::local);
    } else if (action == "--auto-decompose-bsp") {
        if (has_volume_limits) {
            std::cerr << "error replacement_limit_not_applicable: volume delta limits "
                      << "apply to replacement commands, not auto-decompose-bsp\n";
            return EXIT_FAILURE;
        }
        if (max_depth_value < 0.0) {
            std::cerr << "error invalid_max_depth: --max-depth must be non-negative\n";
            return EXIT_FAILURE;
        }
        action_result = cad2csg::auto_decompose_bsp(
            *session,
            target_id,
            static_cast<int>(max_depth_value),
            decomposition_policy,
            cad2csg::ReportIdMode::local);
    } else if (action == "--apply-decomposition-candidate") {
        if (has_volume_limits) {
            std::cerr << "error replacement_limit_not_applicable: volume delta limits "
                      << "apply to replacement commands, not apply-decomposition-candidate\n";
            return EXIT_FAILURE;
        }
        if (candidate_id.empty()) {
            std::cerr << "error missing_candidate: --apply-decomposition-candidate requires --candidate candidate:N\n";
            return EXIT_FAILURE;
        }
        action_result = cad2csg::apply_decomposition_candidate(
            *session,
            target_id,
            candidate_id,
            cad2csg::ReportIdMode::local);
    } else {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (!proposal_report_path.empty()) {
        const cad2csg::DecompositionProposal& proposal =
            cad2csg::last_decomposition_proposal(*session);
        if (cad2csg::write_decomposition_proposal_json(proposal,
                                                       proposal_report_path) != 0) {
            std::cerr << "error proposal_report_write_failed: "
                      << proposal_report_path << '\n';
            return EXIT_FAILURE;
        }
    }

    if (!action_result.ok) {
        print_diagnostics(action_result.report);
        return EXIT_FAILURE;
    }

    if (!step_out_path.empty()) {
        cad2csg::CommandResult step_result =
            cad2csg::export_document_step(*session,
                                          document_id,
                                          step_out_path,
                                          cad2csg::ReportIdMode::local);
        if (!step_result.ok) {
            print_diagnostics(step_result.report);
            return EXIT_FAILURE;
        }
    }

    cad2csg::CommandResult result =
        cad2csg::emit_document(*session,
                               document_id,
                               output_path,
                               cad2csg::ReportIdMode::local);
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
    cad2csg::SessionPtr session = cad2csg::create_session(options);
    const cad2csg::DocumentId document_id = cad2csg::load_step(*session, input_path);

    cad2csg::AutoFitCommandResult auto_fit =
        cad2csg::auto_fit_splines_with_summary(*session,
                                               document_id,
                                               has_max_deviation,
                                               max_deviation,
                                               cad2csg::ReportIdMode::local);
    std::cerr << "auto-fit: " << auto_fit.summary.applied << "/"
              << auto_fit.summary.spline_faces_seen
              << " spline faces replaced, "
              << auto_fit.summary.skipped_over_budget << " over budget, "
              << auto_fit.summary.skipped_no_fit << " no candidate\n";

    cad2csg::CommandResult result =
        cad2csg::emit_document(*session,
                               document_id,
                               output_path,
                               cad2csg::ReportIdMode::local);
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
    return EXIT_SUCCESS;
}

int run_inspect_command(int argc, char** argv) {
    if (argc < 3) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    const std::string input_path = argv[2];
    std::string report_path;
    std::string log_path;

    OptScanner scan(argc, argv, 3);
    while (!scan.done()) {
        if (scan.take_string("--report", &report_path)) continue;
        if (scan.take_string("--log", &log_path)) continue;
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
    cad2csg::SessionPtr session = cad2csg::create_session(options);
    if (!log_path.empty()) {
        try {
            cad2csg::attach_decision_log(*session, log_path, input_path);
        } catch (const std::exception& e) {
            std::cerr << "error decision_log_open_failed: " << e.what() << '\n';
            return EXIT_FAILURE;
        }
    }
    const cad2csg::DocumentId document_id = cad2csg::load_step(*session, input_path);
    cad2csg::CommandResult inspect =
        cad2csg::analyze(*session, document_id, cad2csg::ReportIdMode::local);
    cad2csg::ConversionReport report = inspect.report;
    print_diagnostics(report);
    print_summary(report);

    if (cad2csg::write_report_json(report, report_path) != 0) {
        std::cerr << "error report_write_failed: " << report_path << '\n';
        return EXIT_FAILURE;
    }
    return report.ok() ? EXIT_SUCCESS : EXIT_FAILURE;
}

int run_decompose_apply_command(int argc, char** argv) {
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

    std::string log_path;
    std::string step_out_path;
    std::string report_path;
    std::string policy_name = "auto";
    double max_depth_value = 4.0;
    bool has_max_depth = false;
    bool allow_medium = false;
    bool allow_bbox_fallback = false;

    OptScanner scan(argc, argv, 5);
    while (!scan.done()) {
        if (scan.take_string("--log", &log_path)) continue;
        if (scan.take_string("--out-step", &step_out_path)) continue;
        if (scan.take_string("--report", &report_path)) continue;
        if (scan.take_string("--policy", &policy_name)) continue;
        if (scan.take_double("--max-depth", &max_depth_value, &has_max_depth)) continue;
        if (scan.take_flag("--allow-medium-confidence-auto")) { allow_medium = true; continue; }
        if (scan.take_flag("--allow-bbox-fallback-auto")) { allow_bbox_fallback = true; continue; }
        std::cerr << "error unrecognized_option: " << scan.peek() << '\n';
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    cad2csg::DecompositionPolicy policy;
    if (policy_name == "auto") {
        policy.allow_high_confidence_face_auto = true;
        policy.allow_medium_confidence_auto = allow_medium;
        policy.allow_bbox_fallback_auto = allow_bbox_fallback;
    } else if (policy_name == "conservative") {
        policy.allow_high_confidence_face_auto = true;
        policy.allow_medium_confidence_auto = false;
        policy.allow_bbox_fallback_auto = false;
    } else {
        std::cerr << "error invalid_policy: expected auto|conservative, got "
                  << policy_name << '\n';
        return EXIT_FAILURE;
    }

    std::cerr << "cad2csg: OCCT " << cad2csg::occt_version()
              << ", Alea " << alea_version() << '\n';

    cad2csg::ConversionOptions options;
    cad2csg::SessionPtr session = cad2csg::create_session(options);
    if (!log_path.empty()) {
        try {
            cad2csg::attach_decision_log(*session, log_path, input_path);
        } catch (const std::exception& e) {
            std::cerr << "error decision_log_open_failed: " << e.what() << '\n';
            return EXIT_FAILURE;
        }
    }
    const cad2csg::DocumentId document_id = cad2csg::load_step(*session, input_path);

    cad2csg::CommandResult analysis =
        cad2csg::analyze(*session, document_id, cad2csg::ReportIdMode::local);

    // analysis.ok may be false here: nonconvex_solid_requires_decomposition
    // is an error-severity diagnostic and is *exactly* the condition this
    // command is meant to resolve. We snapshot the targets regardless and
    // bail only if the snapshot is also empty (i.e., nothing to do and
    // analysis already failed for other reasons).
    std::vector<std::string> targets;
    for (const cad2csg::AnalyzedSolid& s : analysis.report.solids) {
        if (s.needs_decomposition) targets.push_back(s.id);
    }

    if (targets.empty() && !analysis.ok) {
        print_diagnostics(analysis.report);
        return EXIT_FAILURE;
    }

    std::size_t attempted = 0, succeeded = 0;
    for (const std::string& id : targets) {
        ++attempted;
        cad2csg::CommandResult r =
            cad2csg::auto_decompose_bsp(*session, id,
                                        static_cast<int>(max_depth_value),
                                        policy,
                                        cad2csg::ReportIdMode::local);
        if (r.ok) ++succeeded;
    }

    std::cerr << "decompose-apply: " << succeeded << "/" << attempted
              << " non-convex solid(s) decomposed (policy=" << policy_name
              << ", max-depth=" << static_cast<int>(max_depth_value) << ")\n";

    if (!step_out_path.empty()) {
        cad2csg::CommandResult step_result =
            cad2csg::export_document_step(*session, document_id, step_out_path,
                                          cad2csg::ReportIdMode::local);
        if (!step_result.ok) {
            print_diagnostics(step_result.report);
            return EXIT_FAILURE;
        }
    }

    cad2csg::CommandResult result =
        cad2csg::emit_document(*session, document_id, output_path,
                               cad2csg::ReportIdMode::local);
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
    return EXIT_SUCCESS;
}

int run_default_convert(int argc, char** argv) {
    if (argc != 3) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    std::cerr << "cad2csg: OCCT " << cad2csg::occt_version()
              << ", Alea " << alea_version() << '\n';

    cad2csg::ConversionOptions options;
    cad2csg::SessionPtr session = cad2csg::create_session(options);
    const cad2csg::DocumentId document_id = cad2csg::load_step(*session, argv[1]);
    cad2csg::CommandResult result =
        cad2csg::emit_document(*session,
                               document_id,
                               argv[2],
                               cad2csg::ReportIdMode::local);

    print_diagnostics(result.report);
    print_summary(result.report);

    if (!result.report.ok() || result.report.solids_converted == 0) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

int run_replay_command(int argc, char** argv) {
    if (argc < 3) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    const std::string recipe_path = argv[2];
    std::string step_override;
    std::string output_path;
    std::string report_path;
    std::string step_out_path;
    OptScanner scan(argc, argv, 3);
    while (!scan.done()) {
        if (scan.take_string("--step", &step_override)) continue;
        if (scan.take_string("--out", &output_path)) continue;
        if (scan.take_string("--report", &report_path)) continue;
        if (scan.take_string("--out-step", &step_out_path)) continue;
        std::cerr << "error unrecognized_option: " << scan.peek() << '\n';
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (output_path.empty() || report_path.empty()) {
        std::cerr << "error: replay requires --out and --report\n";
        return EXIT_FAILURE;
    }
    return cad2csg::replay_recipe(recipe_path, step_override,
                                  output_path, report_path,
                                  step_out_path) == 0
        ? EXIT_SUCCESS
        : EXIT_FAILURE;
}

int run_compact_command(int argc, char** argv) {
    if (argc < 3) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    const std::string log_path = argv[2];
    std::string recipe_path;
    OptScanner scan(argc, argv, 3);
    while (!scan.done()) {
        if (scan.take_string("--out", &recipe_path)) continue;
        std::cerr << "error unrecognized_option: " << scan.peek() << '\n';
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (recipe_path.empty()) {
        std::cerr << "error: compact requires --out path\n";
        return EXIT_FAILURE;
    }
    return cad2csg::compact_log_to_recipe(log_path, recipe_path) == 0
        ? EXIT_SUCCESS
        : EXIT_FAILURE;
}

int run_dump_assembly_command(int argc, char** argv) {
    if (argc < 3) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    const std::string input_path = argv[2];

    cad2csg::ConversionOptions options;
    cad2csg::CadModel model = cad2csg::load_step_model(input_path, options);

    for (const cad2csg::Diagnostic& d : model.diagnostics) {
        std::cerr << severity_name(d.severity) << " " << d.code
                  << ": " << d.message << '\n';
    }
    if (!model.loaded()) {
        std::cerr << "error: could not load " << input_path << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "# solids (" << model.solid_metadata.size() << ")\n";
    std::cout << "# shape_id\tcomponent_entry\tname\thierarchy_path\n";
    for (const cad2csg::ShapeMetadata& m : model.solid_metadata) {
        std::cout << m.shape_id << '\t'
                  << (m.component_entry.empty() ? "-" : m.component_entry) << '\t'
                  << (m.name.empty() ? "-" : m.name) << '\t'
                  << (m.hierarchy_path.empty() ? "-" : m.hierarchy_path) << '\n';
    }

    std::cout << "\n# assembly nodes (" << model.assembly.size() << ")\n";
    std::cout << "# id\tparent\tkind\tshape_id\tlabel_entry\tpath\n";
    for (const cad2csg::AssemblyNode& n : model.assembly) {
        std::cout << n.id << '\t'
                  << (n.parent_id.empty() ? "-" : n.parent_id) << '\t'
                  << n.kind << '\t'
                  << (n.shape_id.empty() ? "-" : n.shape_id) << '\t'
                  << (n.label_entry.empty() ? "-" : n.label_entry) << '\t'
                  << (n.path.empty() ? "-" : n.path) << '\n';
    }
    return EXIT_SUCCESS;
}

int run_lua_command(int argc, char** argv) {
    if (argc < 3) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    return cad2csg::run_lua_script(argv[2], argc - 3, argv + 3) == 0
        ? EXIT_SUCCESS
        : EXIT_FAILURE;
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
    if (verb == "dump-assembly") return run_dump_assembly_command(argc, argv);
    if (verb == "decompose-suggest") return run_decompose_suggest_command(argc, argv);
    if (verb == "decompose-apply") return run_decompose_apply_command(argc, argv);
    if (verb == "compact") return run_compact_command(argc, argv);
    if (verb == "replay") return run_replay_command(argc, argv);
    if (verb == "volume")  return run_volume_command(argc, argv);
    if (verb == "simplify") return run_simplify_command(argc, argv);
    if (verb == "auto-fit") return run_auto_fit_command(argc, argv);
    if (verb == "lua") return run_lua_command(argc, argv);

    return run_default_convert(argc, argv);
}

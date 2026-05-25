#include "cad2csg/engine.hpp"
#include "cad2csg/session.hpp"

#include "decision_pack.hpp"
#include "engine_model.hpp"
#include "hole_fill.hpp"
#include "lua_runner.hpp"
#include "recipe.hpp"
#include "replay.hpp"

#include "viewer.hpp"

#include <alea.h>
#include <Graphic3d_GraphicDriverFactory.hxx>
#if CAD2CSG_HAVE_TKOPENGL
#include <OpenGl_GraphicDriverFactory.hxx>
#endif
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <set>
#include <vector>

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
              << "       " << argv0 << " decision-pack input.step --out decisions/ [--target id] [--max-packets N]\n"
              << "                                  [--hole-fill --host solid:N --near-removed solid:M]\n"
              << "       " << argv0 << " dump-assembly input.step\n"
              << "       " << argv0 << " inspect-tree input.step\n"
              << "       " << argv0 << " remove-suggest input.step [--report removals.json]\n"
              << "       " << argv0 << " surface-suggest input.step face:N.M [--report proposal.json]\n"
              << "       " << argv0 << " hole-fill-suggest input.step [--host solid:N] [--near-removed solid:M]\n"
              << "                                  [--max-radius value] [--max-volume-fraction value]\n"
              << "                                  [--max-candidates N] [--report holes.json]\n"
              << "       " << argv0 << " decompose-suggest input.step solid:N [--report proposals.json]\n"
              << "       " << argv0 << " preprocess input.step --out-step simplified.step\n"
              << "                                  [--policy auto|medium|low|conservative] [--max-depth N]\n"
              << "                                  [--target solid:N] [--max-targets N] [--max-splits N]\n"
              << "                                  [--apply-removal-candidates high|medium]\n"
              << "                                  [--allow-medium-confidence-auto] [--allow-bbox-fallback-auto]\n"
              << "                                  [--log decisions.json] [--report report.json]\n"
              << "       " << argv0 << " decompose-apply input.step --out output.i\n"
              << "                                  [--policy auto|medium|low|conservative] [--max-depth N]\n"
              << "                                  [--target solid:N] [--max-targets N] [--max-splits N]\n"
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
              << "           --fill-hole\n"
              << "           --apply-decomposition-candidate --candidate candidate:N\n"
              << "           --decompose-by-face --by-face face:N.M\n"
              << "           --remove-solid\n"
              << "         actions on a face id (e.g. face:1.4, or a comma list face:1.4,face:1.5):\n"
              << "           --replace-face-with-plane       --replace-face-with-cylinder\n"
              << "           --replace-face-with-sphere      --replace-face-with-cone\n"
              << "           --apply-surface-candidate --candidate candidate:N\n"
              << "         options:\n"
              << "           --max-abs-delta value     (solid replacement, volume guard)\n"
              << "           --max-rel-delta value     (solid replacement, volume guard)\n"
              << "           --max-deviation value     (face replacement, distance guard)\n"
              << "           --max-depth value         (safe BSP auto-decomposition)\n"
              << "           --allow-bbox-fallback-auto\n"
              << "           --allow-medium-confidence-auto\n"
              << "           --report report.json\n"
              << "           --proposal-report proposals.json\n"
              << "           --host solid:N           (fill-hole proposal context)\n"
              << "           --near-removed solid:N   (fill-hole proposal context)\n"
              << "           --max-radius value       (fill-hole proposal guard)\n"
              << "           --max-volume-fraction value (fill-hole proposal guard)\n"
              << "           --log decisions.json      (append-only decision log)\n"
              << "           --out-step simplified.step (as-simplified geometry for review)\n"
              << "       " << argv0 << " auto-fit input.step --out output.i [--max-deviation X] [--report report.json]\n"
              << "       " << argv0 << " lua script.lua [args...]\n"
              << "       " << argv0 << " volume input.step\n"
              << "       " << argv0 << " volume original.step --compare candidate.step [--abs-tol value] [--rel-tol value]\n"
              << "       " << argv0 << " viewer-check\n"
              << "       " << argv0 << " view input.step [--select doc:N/solid:M|solid:M]\n"
              << "                         [--candidate candidate:N]\n"
              << "                         [--review-removal removal:N]\n"
              << "                         [--review-hole-fill hole-fill:N [--host solid:N] [--near-removed solid:M]]\n"
              << "                         [--review-decomp candidate:N --select solid:N]\n"
              << "                         [--review-surface-replacement candidate:N --select-face face:N.M]\n"
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

std::size_t parse_size_arg(const char* text, const char* name) {
    try {
        std::size_t used = 0;
        const std::string value_text(text);
        if (value_text.empty() || value_text[0] == '-') {
            throw std::invalid_argument(name);
        }
        const unsigned long long value = std::stoull(value_text, &used, 10);
        if (used != value_text.size()
            || value > static_cast<unsigned long long>(
                std::numeric_limits<std::size_t>::max())) {
            throw std::invalid_argument(name);
        }
        return static_cast<std::size_t>(value);
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

    bool take_size(const char* flag, std::size_t* out, bool* present = nullptr) {
        if (!matches(flag)) {
            return false;
        }
        *out = parse_size_arg(argv_[pos_ + 1], flag);
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

int run_view_command(int argc, char** argv) {
#if CAD2CSG_HAVE_TKOPENGL
    if (argc < 3) {
        std::cerr << "usage: " << argv[0] << " view input.step"
                     " [--select doc:N/solid:M|solid:M]"
                     " [--candidate candidate:N]"
                     " [--review-removal removal:N]"
                     " [--review-hole-fill hole-fill:N]"
                     " [--review-decomp candidate:N]"
                     " [--review-surface-replacement candidate:N --select-face face:N.M]\n";
        return EXIT_FAILURE;
    }
    const std::string input_path = argv[2];

    std::string preselect_id;
    std::string candidate_id;
    std::string review_removal_id;
    std::string review_hole_fill_id;
    std::string review_decomp_id;
    std::string review_surface_id;
    std::string select_face_id;
    cad2csg::HoleFillOptions hole_fill_options;

    for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "--select") == 0 && i + 1 < argc) {
            preselect_id = argv[++i];
        } else if (std::strcmp(argv[i], "--candidate") == 0 && i + 1 < argc) {
            candidate_id = argv[++i];
        } else if (std::strcmp(argv[i], "--review-removal") == 0 && i + 1 < argc) {
            review_removal_id = argv[++i];
        } else if (std::strcmp(argv[i], "--review-hole-fill") == 0 && i + 1 < argc) {
            review_hole_fill_id = argv[++i];
        } else if (std::strcmp(argv[i], "--review-decomp") == 0 && i + 1 < argc) {
            review_decomp_id = argv[++i];
            candidate_id = review_decomp_id;
        } else if (std::strcmp(argv[i], "--review-surface-replacement") == 0
                   && i + 1 < argc) {
            review_surface_id = argv[++i];
        } else if (std::strcmp(argv[i], "--select-face") == 0 && i + 1 < argc) {
            select_face_id = argv[++i];
        } else if (std::strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            hole_fill_options.host_solid_id = argv[++i];
        } else if (std::strcmp(argv[i], "--near-removed") == 0 && i + 1 < argc) {
            hole_fill_options.near_removed_solid_id = argv[++i];
        } else if (std::strcmp(argv[i], "--max-radius") == 0 && i + 1 < argc) {
            hole_fill_options.max_radius =
                parse_double_arg(argv[++i], "--max-radius");
            hole_fill_options.has_max_radius = true;
        } else if (std::strcmp(argv[i], "--max-volume-fraction") == 0 && i + 1 < argc) {
            hole_fill_options.max_volume_fraction =
                parse_double_arg(argv[++i], "--max-volume-fraction");
            hole_fill_options.has_max_volume_fraction = true;
        } else if (std::strcmp(argv[i], "--max-candidates") == 0 && i + 1 < argc) {
            hole_fill_options.max_candidates =
                parse_size_arg(argv[++i], "--max-candidates");
        } else {
            std::cerr << "error unrecognized_option: " << argv[i] << '\n';
            return EXIT_FAILURE;
        }
    }

    const int review_count = (review_removal_id.empty() ? 0 : 1)
        + (review_hole_fill_id.empty() ? 0 : 1)
        + (review_decomp_id.empty() ? 0 : 1)
        + (review_surface_id.empty() ? 0 : 1);
    if (review_count > 1) {
        std::cerr << "error view_review_conflict: use only one --review-* option\n";
        return EXIT_FAILURE;
    }

    cad2csg::ConversionOptions options;
    cad2csg::SessionPtr session = cad2csg::create_session(options);
    const cad2csg::DocumentId doc_id = cad2csg::load_step(*session, input_path);
    cad2csg::analyze(*session, doc_id, cad2csg::ReportIdMode::scoped);

    const cad2csg::DecompositionProposal* proposal = nullptr;
    cad2csg::DecompositionProposal owned_proposal;
    cad2csg::ViewerReviewContext review;

    if (!review_removal_id.empty()) {
        cad2csg::RemovalProposal removals =
            cad2csg::suggest_removals(*session,
                                      doc_id,
                                      cad2csg::ReportIdMode::scoped);
        const cad2csg::RemovalCandidate* selected = nullptr;
        for (const cad2csg::RemovalCandidate& c : removals.candidates) {
            if (c.id == review_removal_id) {
                selected = &c;
                break;
            }
        }
        if (!selected) {
            std::cerr << "error review_removal_not_found: "
                      << review_removal_id << '\n';
            return EXIT_FAILURE;
        }
        preselect_id = selected->solid_id;
        review.kind = "removal";
        review.review_id = selected->id;
        review.action = "remove_solid";
        review.target_id = selected->solid_id;
    }

    if (!review_hole_fill_id.empty()) {
        cad2csg::HoleFillProposal holes =
            cad2csg::propose_hole_fills(*session,
                                        doc_id,
                                        hole_fill_options,
                                        cad2csg::ReportIdMode::scoped);
        const cad2csg::HoleFillCandidate* selected = nullptr;
        for (const cad2csg::HoleFillCandidate& c : holes.candidates) {
            if (c.id == review_hole_fill_id) {
                selected = &c;
                break;
            }
        }
        if (!selected) {
            std::cerr << "error review_hole_fill_not_found: "
                      << review_hole_fill_id << '\n';
            return EXIT_FAILURE;
        }
        preselect_id = selected->host_solid_id;
        review.kind = "hole_fill";
        review.review_id = selected->id;
        review.action = "fill_hole";
        review.target_id = doc_id;
        review.candidate_id = selected->id;
        review.hole_fill = *selected;
        review.has_hole_fill = true;
    }

    if (!review_surface_id.empty()) {
        if (select_face_id.empty()) {
            std::cerr << "error review_surface_requires_select_face: "
                      << "--review-surface-replacement needs --select-face face:N.M\n";
            return EXIT_FAILURE;
        }
        cad2csg::SurfaceReplacementProposal surface =
            cad2csg::propose_surface_replacement(*session,
                                                 select_face_id,
                                                 cad2csg::ReportIdMode::scoped);
        const cad2csg::SurfaceReplacementCandidate* selected = nullptr;
        for (const cad2csg::SurfaceReplacementCandidate& c : surface.candidates) {
            if (c.id == review_surface_id) {
                selected = &c;
                break;
            }
        }
        if (!selected) {
            std::cerr << "error review_surface_not_found: "
                      << review_surface_id << '\n';
            return EXIT_FAILURE;
        }
        preselect_id = selected->owner_target_id;
        review.kind = "surface_replacement";
        review.review_id = selected->id;
        review.action = "apply_surface_replacement_candidate";
        review.target_id = surface.face_id;
        review.candidate_id = selected->id;
        review.surface_replacement = *selected;
        review.has_surface_replacement = true;
    }

    if (!review_decomp_id.empty() && preselect_id.empty()) {
        std::cerr << "error review_decomp_requires_select: "
                  << "--review-decomp needs --select solid:N\n";
        return EXIT_FAILURE;
    }

    if (!preselect_id.empty() && !candidate_id.empty()) {
        owned_proposal = cad2csg::propose_decomposition(
            *session, preselect_id, cad2csg::ReportIdMode::scoped);
        proposal = &owned_proposal;
        const cad2csg::DecompositionCandidate* selected = nullptr;
        for (const cad2csg::DecompositionCandidate& c : owned_proposal.candidates) {
            if (c.id == candidate_id) {
                selected = &c;
                break;
            }
        }
        if (!review_decomp_id.empty() && !selected) {
            std::cerr << "error review_decomp_not_found: "
                      << review_decomp_id << '\n';
            return EXIT_FAILURE;
        }
        if (!review_decomp_id.empty() && selected) {
            review.kind = "decomposition";
            review.review_id = selected->id;
            review.action = "apply_decomposition_candidate";
            review.target_id = selected->target_id;
            review.candidate_id = selected->id;
        }
    }

    return cad2csg::run_viewer_command(
        *session, input_path, preselect_id, proposal, candidate_id, review);
#else
    (void)argc; (void)argv;
    std::cerr << "error viewer_not_built: cad2csg was built without TKOpenGl support.\n";
    return EXIT_FAILURE;
#endif
}

int run_viewer_check_command() {
    std::cout << "occt_version " << cad2csg::occt_version() << '\n';

#if CAD2CSG_HAVE_TKOPENGL
    if (Graphic3d_GraphicDriverFactory::DefaultDriverFactory().IsNull()) {
        Graphic3d_GraphicDriverFactory::RegisterFactory(
            new OpenGl_GraphicDriverFactory(),
            true);
    }
#endif

    const bool has_factory =
        !Graphic3d_GraphicDriverFactory::DefaultDriverFactory().IsNull();
    std::cout << "tkopengl_linked "
#if CAD2CSG_HAVE_TKOPENGL
              << "true"
#else
              << "false"
#endif
              << '\n';
    std::cout << "graphic_driver_factory "
              << (has_factory ? "available" : "missing") << '\n';

    const char* display = std::getenv("DISPLAY");
    const char* wayland = std::getenv("WAYLAND_DISPLAY");
    std::cout << "display "
              << (display && display[0] ? display : "-") << '\n';
    std::cout << "wayland_display "
              << (wayland && wayland[0] ? wayland : "-") << '\n';

    if (!has_factory) {
        std::cout << "status unavailable\n";
        std::cout << "reason OCCT has no registered Graphic3d driver factory. "
                  << "This usually means the installed OCCT was built without "
                  << "the TKOpenGl visualization driver.\n";
        return EXIT_FAILURE;
    }

    if ((!display || !display[0]) && (!wayland || !wayland[0])) {
        std::cout << "status probably_unavailable\n";
        std::cout << "reason no DISPLAY or WAYLAND_DISPLAY is set for the OCCT viewer.\n";
        return EXIT_FAILURE;
    }

    std::cout << "status available\n";
    return EXIT_SUCCESS;
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

int run_decision_pack_command(int argc, char** argv) {
    if (argc < 5) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    const std::string input_path = argv[2];
    cad2csg::DecisionPackOptions pack_options;
    cad2csg::HoleFillOptions hole_fill_options;
    bool use_hole_fill_proposal = false;

    OptScanner scan(argc, argv, 3);
    while (!scan.done()) {
        if (scan.take_string("--out", &pack_options.output_dir)) continue;
        if (scan.take_string("--target", &pack_options.target_id)) continue;
        if (scan.take_size("--max-packets", &pack_options.max_packets)) continue;
        if (scan.take_flag("--hole-fill")) {
            use_hole_fill_proposal = true;
            continue;
        }
        if (scan.take_string("--host", &hole_fill_options.host_solid_id)) continue;
        if (scan.take_string("--near-removed",
                             &hole_fill_options.near_removed_solid_id)) continue;
        if (scan.take_double("--max-radius",
                             &hole_fill_options.max_radius,
                             &hole_fill_options.has_max_radius)) continue;
        if (scan.take_double("--max-volume-fraction",
                             &hole_fill_options.max_volume_fraction,
                             &hole_fill_options.has_max_volume_fraction)) continue;
        if (scan.take_size("--max-candidates",
                           &hole_fill_options.max_candidates)) continue;
        std::cerr << "error unrecognized_option: " << scan.peek() << '\n';
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (pack_options.output_dir.empty()) {
        std::cerr << "error missing_out: decision-pack requires --out directory\n";
        return EXIT_FAILURE;
    }

    cad2csg::ConversionOptions options;
    cad2csg::CadModel model = cad2csg::load_step_model(input_path, options);
    const cad2csg::DocumentId document_id = "doc:1";
    cad2csg::ConversionReport decision_report;
    if (use_hole_fill_proposal) {
        cad2csg::HoleFillProposal proposal =
            cad2csg::propose_hole_fill_candidates(model,
                                                  hole_fill_options,
                                                  options);
        if (!proposal.ok) {
            std::cerr << "error hole_fill_proposal_failed\n";
            return EXIT_FAILURE;
        }
        decision_report.document_id = document_id;
        decision_report.source_path = input_path;
        decision_report.diagnostics = proposal.diagnostics;
        decision_report.decision_requests = proposal.decision_requests;
    } else {
        decision_report = cad2csg::analyze_model(model, options);
    }

    cad2csg::DecisionPackResult result =
        cad2csg::write_decision_packs(decision_report, model, pack_options);
    if (!result.ok) {
        std::cerr << "error decision_pack_failed: " << result.error << '\n';
        return EXIT_FAILURE;
    }

    std::cerr << "decision packets: " << result.packets_written
              << " written to " << pack_options.output_dir << '\n';
    return EXIT_SUCCESS;
}

int run_remove_suggest_command(int argc, char** argv) {
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

    cad2csg::ConversionOptions options;
    cad2csg::SessionPtr session = cad2csg::create_session(options);
    const cad2csg::DocumentId document_id = cad2csg::load_step(*session, input_path);
    cad2csg::RemovalProposal proposal =
        cad2csg::suggest_removals(*session,
                                  document_id,
                                  cad2csg::ReportIdMode::local);

    if (!report_path.empty()) {
        if (cad2csg::write_removal_proposal_json(proposal, report_path) != 0) {
            std::cerr << "error report_write_failed: " << report_path << '\n';
            return EXIT_FAILURE;
        }
    } else {
        std::cout << cad2csg::removal_proposal_json_string(proposal);
    }

    std::cerr << "removal candidates: " << proposal.candidates.size()
              << ", high-confidence: " << proposal.high_confidence_count
              << ", high-confidence-volume-fraction: "
              << proposal.high_confidence_volume_fraction
              << ", ok: " << (proposal.ok ? "true" : "false") << '\n';
    return proposal.ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

int run_hole_fill_suggest_command(int argc, char** argv) {
    if (argc < 3) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    const std::string input_path = argv[2];
    std::string report_path;
    cad2csg::HoleFillOptions hole_options;

    OptScanner scan(argc, argv, 3);
    while (!scan.done()) {
        if (scan.take_string("--report", &report_path)) continue;
        if (scan.take_string("--host", &hole_options.host_solid_id)) continue;
        if (scan.take_string("--near-removed",
                             &hole_options.near_removed_solid_id)) continue;
        if (scan.take_double("--max-radius",
                             &hole_options.max_radius,
                             &hole_options.has_max_radius)) continue;
        if (scan.take_double("--max-volume-fraction",
                             &hole_options.max_volume_fraction,
                             &hole_options.has_max_volume_fraction)) continue;
        if (scan.take_size("--max-candidates",
                           &hole_options.max_candidates)) continue;
        std::cerr << "error unrecognized_option: " << scan.peek() << '\n';
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    cad2csg::ConversionOptions options;
    cad2csg::SessionPtr session = cad2csg::create_session(options);
    const cad2csg::DocumentId document_id = cad2csg::load_step(*session, input_path);
    cad2csg::HoleFillProposal proposal =
        cad2csg::propose_hole_fills(*session,
                                    document_id,
                                    hole_options,
                                    cad2csg::ReportIdMode::local);

    if (!report_path.empty()) {
        if (cad2csg::write_hole_fill_proposal_json(proposal, report_path) != 0) {
            std::cerr << "error report_write_failed: " << report_path << '\n';
            return EXIT_FAILURE;
        }
    } else {
        std::cout << cad2csg::hole_fill_proposal_json_string(proposal);
    }

    std::cerr << "hole-fill candidates: " << proposal.candidates.size()
              << ", decisions: " << proposal.decision_requests.size()
              << ", ok: " << (proposal.ok ? "true" : "false") << '\n';
    return proposal.ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

int run_surface_suggest_command(int argc, char** argv) {
    if (argc < 4) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    const std::string input_path = argv[2];
    const std::string face_id = argv[3];
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
    cad2csg::SurfaceReplacementProposal proposal =
        cad2csg::propose_surface_replacement(*session,
                                             face_id,
                                             cad2csg::ReportIdMode::local);

    if (!report_path.empty()) {
        if (cad2csg::write_surface_replacement_proposal_json(proposal,
                                                             report_path) != 0) {
            std::cerr << "error report_write_failed: " << report_path << '\n';
            return EXIT_FAILURE;
        }
    } else {
        std::cout << cad2csg::surface_replacement_proposal_json_string(proposal);
    }

    std::cerr << "surface candidates: " << proposal.candidates.size()
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
    cad2csg::HoleFillOptions hole_fill_options;
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
        if (scan.take_string("--host", &hole_fill_options.host_solid_id)) continue;
        if (scan.take_string("--near-removed",
                             &hole_fill_options.near_removed_solid_id)) continue;
        if (scan.take_double("--max-radius",
                             &hole_fill_options.max_radius,
                             &hole_fill_options.has_max_radius)) continue;
        if (scan.take_double("--max-volume-fraction",
                             &hole_fill_options.max_volume_fraction,
                             &hole_fill_options.has_max_volume_fraction)) continue;
        if (scan.take_size("--max-candidates",
                           &hole_fill_options.max_candidates)) continue;
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
    } else if (action == "--fill-hole") {
        if (has_volume_limits) {
            std::cerr << "error replacement_limit_not_applicable: volume delta limits "
                      << "apply to replacement commands, not fill-hole\n";
            return EXIT_FAILURE;
        }
        if (hole_fill_options.host_solid_id.empty()
            && hole_fill_options.near_removed_solid_id.empty()) {
            std::cerr << "error missing_hole_fill_context: --fill-hole requires "
                      << "--host solid:N or --near-removed solid:N\n";
            return EXIT_FAILURE;
        }
        cad2csg::HoleFillProposal proposal =
            cad2csg::propose_hole_fills(*session,
                                        document_id,
                                        hole_fill_options,
                                        cad2csg::ReportIdMode::local);
        if (!proposal.ok) {
            std::cerr << "error hole_fill_proposal_failed\n";
            return EXIT_FAILURE;
        }
        action_result = cad2csg::fill_hole(
            *session,
            document_id,
            target_id,
            cad2csg::ReportIdMode::local);
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
    } else if (action == "--apply-surface-candidate") {
        if (has_volume_limits) {
            std::cerr << "error replacement_limit_not_applicable: volume delta limits "
                      << "apply to replacement commands, not apply-surface-candidate\n";
            return EXIT_FAILURE;
        }
        if (candidate_id.empty()) {
            std::cerr << "error missing_candidate: --apply-surface-candidate requires --candidate candidate:N\n";
            return EXIT_FAILURE;
        }
        action_result = cad2csg::apply_surface_replacement_candidate(
            *session,
            target_id,
            candidate_id,
            cad2csg::ReportIdMode::local);
    } else {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (!proposal_report_path.empty()) {
        int proposal_rc = 0;
        if (action == "--fill-hole") {
            proposal_rc = cad2csg::write_hole_fill_proposal_json(
                cad2csg::last_hole_fill_proposal(*session),
                proposal_report_path);
        } else if (action == "--apply-surface-candidate") {
            proposal_rc = cad2csg::write_surface_replacement_proposal_json(
                cad2csg::last_surface_replacement_proposal(*session),
                proposal_report_path);
        } else {
            const cad2csg::DecompositionProposal& proposal =
                cad2csg::last_decomposition_proposal(*session);
            proposal_rc = cad2csg::write_decomposition_proposal_json(
                proposal,
                proposal_report_path);
        }
        if (proposal_rc != 0) {
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
    std::size_t max_depth_value = 4;
    bool has_max_depth = false;
    bool allow_medium = false;
    bool allow_bbox_fallback = false;
    std::string apply_removal_confidence;
    std::size_t max_splits_value = 16;
    std::size_t max_targets_value = 0;
    std::vector<std::string> selected_targets;

    OptScanner scan(argc, argv, 5);
    while (!scan.done()) {
        std::string target_id;
        if (scan.take_string("--log", &log_path)) continue;
        if (scan.take_string("--out-step", &step_out_path)) continue;
        if (scan.take_string("--report", &report_path)) continue;
        if (scan.take_string("--policy", &policy_name)) continue;
        if (scan.take_string("--apply-removal-candidates",
                             &apply_removal_confidence)) continue;
        if (scan.take_size("--max-depth", &max_depth_value, &has_max_depth)) continue;
        if (scan.take_size("--max-splits", &max_splits_value)) continue;
        if (scan.take_size("--max-targets", &max_targets_value)) continue;
        if (scan.take_string("--target", &target_id)) {
            selected_targets.push_back(target_id);
            continue;
        }
        if (scan.take_flag("--allow-medium-confidence-auto")) { allow_medium = true; continue; }
        if (scan.take_flag("--allow-bbox-fallback-auto")) { allow_bbox_fallback = true; continue; }
        std::cerr << "error unrecognized_option: " << scan.peek() << '\n';
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    cad2csg::DecompositionPolicy policy;
    // Confidence ladder:
    //   conservative / auto -> accept only "high"-confidence candidates
    //   medium              -> add "medium"
    //   low                 -> add "low"  (lets the BSP recurse through
    //                                       cuts whose pieces still need
    //                                       further decomposition)
    // --allow-medium-confidence-auto and --allow-bbox-fallback-auto layer
    // on top of any policy for finer control.
    if (policy_name == "auto" || policy_name == "conservative") {
        policy.allow_high_confidence_face_auto = true;
        policy.allow_medium_confidence_auto = allow_medium;
        policy.allow_bbox_fallback_auto = allow_bbox_fallback;
    } else if (policy_name == "medium") {
        policy.allow_high_confidence_face_auto = true;
        policy.allow_medium_confidence_auto = true;
        policy.allow_bbox_fallback_auto = allow_bbox_fallback;
    } else if (policy_name == "low") {
        policy.allow_high_confidence_face_auto = true;
        policy.allow_medium_confidence_auto = true;
        policy.allow_bbox_fallback_auto = true;
    } else {
        std::cerr << "error invalid_policy: expected auto|medium|low|"
                     "conservative, got " << policy_name << '\n';
        return EXIT_FAILURE;
    }
    policy.max_splits_per_target = max_splits_value;
    if (!apply_removal_confidence.empty()
        && apply_removal_confidence != "high"
        && apply_removal_confidence != "medium") {
        std::cerr << "error invalid_removal_policy: expected high|medium, got "
                  << apply_removal_confidence << '\n';
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

    if (!apply_removal_confidence.empty()) {
        cad2csg::RemovalProposal removals =
            cad2csg::suggest_removals(*session,
                                      document_id,
                                      cad2csg::ReportIdMode::local);
        std::size_t removed = 0;
        for (const cad2csg::RemovalCandidate& candidate : removals.candidates) {
            const bool allowed = candidate.confidence == "high"
                || (apply_removal_confidence == "medium"
                    && candidate.confidence == "medium");
            if (!allowed || candidate.suggested_action == "manual") {
                continue;
            }
            cad2csg::CommandResult r =
                cad2csg::remove_solid(*session,
                                      candidate.solid_id,
                                      cad2csg::ReportIdMode::local);
            if (r.ok) {
                ++removed;
            }
        }
        std::cerr << "preprocess: removed " << removed
                  << " component(s) from removal candidates"
                  << " (policy=" << apply_removal_confidence << ")\n";
    }

    cad2csg::CommandResult analysis =
        cad2csg::analyze(*session, document_id, cad2csg::ReportIdMode::local);

    // analysis.ok may be false here: nonconvex_solid_requires_decomposition
    // is an error-severity diagnostic and is *exactly* the condition this
    // command is meant to resolve. We snapshot the targets regardless and
    // bail only if the snapshot is also empty (i.e., nothing to do and
    // analysis already failed for other reasons).
    std::vector<std::string> targets = selected_targets;
    if (targets.empty()) {
        for (const cad2csg::AnalyzedSolid& s : analysis.report.solids) {
            if (s.needs_decomposition) targets.push_back(s.id);
        }
    }
    const std::size_t available_targets = targets.size();
    if (max_targets_value > 0 && targets.size() > max_targets_value) {
        targets.resize(max_targets_value);
    }

    if (targets.empty() && !analysis.ok) {
        print_diagnostics(analysis.report);
        return EXIT_FAILURE;
    }

    std::size_t attempted = 0;
    std::size_t succeeded = 0;
    std::size_t needs_decision = 0;
    std::size_t other_failure = 0;
    for (const std::string& id : targets) {
        ++attempted;
        cad2csg::CommandResult r =
            cad2csg::auto_decompose_bsp(*session, id,
                                        static_cast<int>(max_depth_value),
                                        policy,
                                        cad2csg::ReportIdMode::local,
                                        /*defer_analysis=*/true);
        if (r.ok) {
            ++succeeded;
        } else {
            const bool stuck_on_confidence = std::any_of(
                r.report.diagnostics.begin(), r.report.diagnostics.end(),
                [](const cad2csg::Diagnostic& d) {
                    return d.code == "auto_decompose_bsp_needs_decision";
                });
            if (stuck_on_confidence) ++needs_decision;
            else ++other_failure;
        }
    }

    std::cerr << "decompose-apply: " << succeeded << "/" << attempted
              << " selected non-convex solid(s) decomposed"
              << " (selected=" << targets.size()
              << ", available=" << available_targets
              << ", policy=" << policy_name
              << ", max-depth=" << max_depth_value << ")\n";
    if (needs_decision > 0) {
        std::cerr << "  " << needs_decision
                  << " stopped on low-confidence candidates."
                     " Re-run with --policy medium or --policy low to allow"
                     " the BSP to recurse through them.\n";
    }
    if (other_failure > 0) {
        std::cerr << "  " << other_failure
                  << " failed for other reasons; see report diagnostics.\n";
    }

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

int run_preprocess_command(int argc, char** argv) {
    if (argc < 3) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    const std::string input_path = argv[2];

    std::string log_path;
    std::string step_out_path;
    std::string report_path;
    std::string policy_name = "auto";
    std::size_t max_depth_value = 4;
    bool has_max_depth = false;
    bool allow_medium = false;
    bool allow_bbox_fallback = false;
    std::string apply_removal_confidence;
    std::size_t max_splits_value = 16;
    std::size_t max_targets_value = 0;
    std::vector<std::string> selected_targets;

    OptScanner scan(argc, argv, 3);
    while (!scan.done()) {
        std::string target_id;
        if (scan.take_string("--log", &log_path)) continue;
        if (scan.take_string("--out-step", &step_out_path)) continue;
        if (scan.take_string("--report", &report_path)) continue;
        if (scan.take_string("--policy", &policy_name)) continue;
        if (scan.take_string("--apply-removal-candidates",
                             &apply_removal_confidence)) continue;
        if (scan.take_size("--max-depth", &max_depth_value, &has_max_depth)) continue;
        if (scan.take_size("--max-splits", &max_splits_value)) continue;
        if (scan.take_size("--max-targets", &max_targets_value)) continue;
        if (scan.take_string("--target", &target_id)) {
            selected_targets.push_back(target_id);
            continue;
        }
        if (scan.take_flag("--allow-medium-confidence-auto")) { allow_medium = true; continue; }
        if (scan.take_flag("--allow-bbox-fallback-auto")) { allow_bbox_fallback = true; continue; }
        std::cerr << "error unrecognized_option: " << scan.peek() << '\n';
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (step_out_path.empty()) {
        std::cerr << "error preprocess_requires_out_step: preprocess requires --out-step\n";
        return EXIT_FAILURE;
    }

    cad2csg::DecompositionPolicy policy;
    if (policy_name == "auto" || policy_name == "conservative") {
        policy.allow_high_confidence_face_auto = true;
        policy.allow_medium_confidence_auto = allow_medium;
        policy.allow_bbox_fallback_auto = allow_bbox_fallback;
    } else if (policy_name == "medium") {
        policy.allow_high_confidence_face_auto = true;
        policy.allow_medium_confidence_auto = true;
        policy.allow_bbox_fallback_auto = allow_bbox_fallback;
    } else if (policy_name == "low") {
        policy.allow_high_confidence_face_auto = true;
        policy.allow_medium_confidence_auto = true;
        policy.allow_bbox_fallback_auto = true;
    } else {
        std::cerr << "error invalid_policy: expected auto|medium|low|"
                     "conservative, got " << policy_name << '\n';
        return EXIT_FAILURE;
    }
    policy.max_splits_per_target = max_splits_value;
    if (!apply_removal_confidence.empty()
        && apply_removal_confidence != "high"
        && apply_removal_confidence != "medium") {
        std::cerr << "error invalid_removal_policy: expected high|medium, got "
                  << apply_removal_confidence << '\n';
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

    if (!apply_removal_confidence.empty()) {
        cad2csg::RemovalProposal removals =
            cad2csg::suggest_removals(*session,
                                      document_id,
                                      cad2csg::ReportIdMode::local);
        std::size_t removed = 0;
        for (const cad2csg::RemovalCandidate& candidate : removals.candidates) {
            const bool allowed = candidate.confidence == "high"
                || (apply_removal_confidence == "medium"
                    && candidate.confidence == "medium");
            if (!allowed || candidate.suggested_action == "manual") {
                continue;
            }
            cad2csg::CommandResult r =
                cad2csg::remove_solid(*session,
                                      candidate.solid_id,
                                      cad2csg::ReportIdMode::local);
            if (r.ok) {
                ++removed;
            }
        }
        std::cerr << "preprocess: removed " << removed
                  << " component(s) from removal candidates"
                  << " (policy=" << apply_removal_confidence << ")\n";
    }

    cad2csg::CommandResult analysis =
        cad2csg::analyze(*session, document_id, cad2csg::ReportIdMode::local);
    std::vector<std::string> targets = selected_targets;
    if (targets.empty()) {
        for (const cad2csg::AnalyzedSolid& s : analysis.report.solids) {
            if (s.needs_decomposition) targets.push_back(s.id);
        }
    }
    const std::size_t available_targets = targets.size();
    if (max_targets_value > 0 && targets.size() > max_targets_value) {
        targets.resize(max_targets_value);
    }

    if (targets.empty() && !analysis.ok) {
        print_diagnostics(analysis.report);
        return EXIT_FAILURE;
    }

    std::size_t attempted = 0;
    std::size_t succeeded = 0;
    std::size_t needs_decision = 0;
    std::size_t other_failure = 0;
    for (const std::string& id : targets) {
        ++attempted;
        cad2csg::CommandResult r =
            cad2csg::auto_decompose_bsp(*session, id,
                                        static_cast<int>(max_depth_value),
                                        policy,
                                        cad2csg::ReportIdMode::local,
                                        /*defer_analysis=*/true);
        if (r.ok) {
            ++succeeded;
        } else {
            const bool stuck_on_confidence = std::any_of(
                r.report.diagnostics.begin(), r.report.diagnostics.end(),
                [](const cad2csg::Diagnostic& d) {
                    return d.code == "auto_decompose_bsp_needs_decision";
                });
            if (stuck_on_confidence) ++needs_decision;
            else ++other_failure;
        }
    }

    std::cerr << "preprocess: " << succeeded << "/" << attempted
              << " selected non-convex solid(s) decomposed"
              << " (selected=" << targets.size()
              << ", available=" << available_targets
              << ", policy=" << policy_name
              << ", max-depth=" << max_depth_value
              << ", max-splits=" << max_splits_value << ")\n";
    if (needs_decision > 0) {
        std::cerr << "  " << needs_decision
                  << " stopped on low-confidence candidates.\n";
    }
    if (other_failure > 0) {
        std::cerr << "  " << other_failure
                  << " failed for other reasons; see report diagnostics.\n";
    }

    cad2csg::CommandResult step_result =
        cad2csg::export_document_step(*session, document_id, step_out_path,
                                      cad2csg::ReportIdMode::local);

    if (!report_path.empty()
        && cad2csg::write_report_json(step_result.report, report_path) != 0) {
        std::cerr << "error report_write_failed: " << report_path << '\n';
        return EXIT_FAILURE;
    }

    return step_result.ok ? EXIT_SUCCESS : EXIT_FAILURE;
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

std::string last_id_component(const std::string& id) {
    const std::size_t slash = id.rfind('/');
    if (slash == std::string::npos) {
        return id;
    }
    return id.substr(slash + 1);
}

std::string solid_status_label(const cad2csg::AnalyzedSolid& solid) {
    if (solid.exact_csg_supported) {
        return "ready";
    }
    if (solid.needs_spline_replacement) {
        return "spline";
    }
    if (solid.needs_decomposition) {
        return "decompose";
    }
    if (solid.needs_user_decision) {
        return "decision";
    }
    return "review";
}

std::string compact_name(const std::string& primary,
                         const std::string& fallback,
                         const std::string& id) {
    if (!primary.empty()) {
        return primary;
    }
    if (!fallback.empty()) {
        return fallback;
    }
    return last_id_component(id);
}

void print_solid_tree_line(const cad2csg::AnalyzedSolid& solid,
                           const std::string& prefix,
                           bool last) {
    std::cout << prefix << (last ? "`- " : "|- ")
              << solid.id << " [" << solid_status_label(solid) << "]";
    const std::string name = compact_name(solid.name, solid.label, solid.id);
    if (!name.empty()) {
        std::cout << "  " << name;
    }
    std::cout << "  volume=" << solid.volume;
    if (solid.volume_fraction > 0.0) {
        std::cout << " fraction=" << solid.volume_fraction;
    }
    if (!solid.hierarchy_path.empty()) {
        std::cout << "  path=\"" << solid.hierarchy_path << "\"";
    }
    std::cout << '\n';
}

void print_assembly_node_tree(
    const cad2csg::AssemblyNode& node,
    const std::unordered_map<std::string, std::vector<const cad2csg::AssemblyNode*>>& children,
    const std::unordered_map<std::string, const cad2csg::AnalyzedSolid*>& solid_by_id,
    std::set<std::string>* printed_solids,
    const std::string& prefix,
    bool last) {
    std::cout << prefix << (last ? "`- " : "|- ")
              << node.id << " [" << (node.kind.empty() ? "node" : node.kind) << "]";
    const std::string name = compact_name(node.name, node.label_entry, node.id);
    if (!name.empty()) {
        std::cout << "  " << name;
    }
    if (!node.shape_id.empty()) {
        std::cout << "  shape=" << node.shape_id;
    }
    std::cout << '\n';

    const std::string child_prefix = prefix + (last ? "   " : "|  ");

    auto solid_it = solid_by_id.find(node.shape_id);
    const bool has_solid = solid_it != solid_by_id.end();
    auto child_it = children.find(node.id);
    const bool has_children = child_it != children.end() && !child_it->second.empty();

    if (has_solid) {
        printed_solids->insert(solid_it->second->id);
        print_solid_tree_line(*solid_it->second, child_prefix, !has_children);
    }

    if (!has_children) {
        return;
    }
    const std::vector<const cad2csg::AssemblyNode*>& node_children = child_it->second;
    for (std::size_t i = 0; i < node_children.size(); ++i) {
        print_assembly_node_tree(*node_children[i],
                                 children,
                                 solid_by_id,
                                 printed_solids,
                                 child_prefix,
                                 i + 1 == node_children.size());
    }
}

int run_inspect_tree_command(int argc, char** argv) {
    if (argc < 3) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    const std::string input_path = argv[2];
    for (int i = 3; i < argc; ++i) {
        std::cerr << "error unrecognized_option: " << argv[i] << '\n';
        return EXIT_FAILURE;
    }

    cad2csg::ConversionOptions options;
    cad2csg::SessionPtr session = cad2csg::create_session(options);
    const cad2csg::DocumentId doc_id = cad2csg::load_step(*session, input_path);
    cad2csg::CommandResult result =
        cad2csg::analyze(*session, doc_id, cad2csg::ReportIdMode::scoped);
    const cad2csg::ConversionReport& report = result.report;

    for (const cad2csg::Diagnostic& d : report.diagnostics) {
        std::cerr << severity_name(d.severity) << " " << d.code
                  << ": " << d.message << '\n';
    }

    std::unordered_map<std::string, const cad2csg::AnalyzedSolid*> solid_by_id;
    for (const cad2csg::AnalyzedSolid& solid : report.solids) {
        solid_by_id[solid.id] = &solid;
    }

    std::unordered_map<std::string, std::vector<const cad2csg::AssemblyNode*>> children;
    std::vector<const cad2csg::AssemblyNode*> roots;
    for (const cad2csg::AssemblyNode& node : report.assembly) {
        children[node.parent_id].push_back(&node);
    }
    auto root_it = children.find("");
    if (root_it != children.end()) {
        roots = root_it->second;
    }

    std::cout << "# cad2csg tree\n";
    std::cout << "# source " << input_path << '\n';
    std::cout << "# document " << report.document_id << '\n';
    std::cout << "# solids " << report.solids.size()
              << " assembly_nodes " << report.assembly.size() << '\n';
    std::cout << "# statuses: ready, decompose, spline, decision, review\n";

    std::set<std::string> printed_solids;
    if (!roots.empty()) {
        for (std::size_t i = 0; i < roots.size(); ++i) {
            print_assembly_node_tree(*roots[i],
                                     children,
                                     solid_by_id,
                                     &printed_solids,
                                     "",
                                     i + 1 == roots.size());
        }
    }

    std::vector<const cad2csg::AnalyzedSolid*> unlisted;
    for (const cad2csg::AnalyzedSolid& solid : report.solids) {
        if (!printed_solids.count(solid.id)) {
            unlisted.push_back(&solid);
        }
    }

    if (!unlisted.empty()) {
        if (!roots.empty()) {
            std::cout << "`- unlisted-solids\n";
        }
        const std::string prefix = roots.empty() ? "" : "   ";
        for (std::size_t i = 0; i < unlisted.size(); ++i) {
            print_solid_tree_line(*unlisted[i], prefix, i + 1 == unlisted.size());
        }
    }

    return report.solids_seen > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
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
    if (verb == "decision-pack") return run_decision_pack_command(argc, argv);
    if (verb == "dump-assembly") return run_dump_assembly_command(argc, argv);
    if (verb == "inspect-tree") return run_inspect_tree_command(argc, argv);
    if (verb == "remove-suggest") return run_remove_suggest_command(argc, argv);
    if (verb == "surface-suggest") return run_surface_suggest_command(argc, argv);
    if (verb == "hole-fill-suggest") return run_hole_fill_suggest_command(argc, argv);
    if (verb == "decompose-suggest") return run_decompose_suggest_command(argc, argv);
    if (verb == "preprocess") return run_preprocess_command(argc, argv);
    if (verb == "decompose-apply") return run_decompose_apply_command(argc, argv);
    if (verb == "compact") return run_compact_command(argc, argv);
    if (verb == "replay") return run_replay_command(argc, argv);
    if (verb == "volume")  return run_volume_command(argc, argv);
    if (verb == "simplify") return run_simplify_command(argc, argv);
    if (verb == "auto-fit") return run_auto_fit_command(argc, argv);
    if (verb == "lua") return run_lua_command(argc, argv);
    if (verb == "viewer-check") return run_viewer_check_command();
    if (verb == "view") return run_view_command(argc, argv);

    return run_default_convert(argc, argv);
}

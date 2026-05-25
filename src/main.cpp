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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
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

// Per-verb help table. Drives both the global usage listing and the
// per-verb --help output. Keeping the help as data (vs. a long branchy
// usage() function) means new verbs only need one entry to be wired up,
// and the help text is also visible from `cad2csg <verb> --help`.
struct VerbHelp {
    const char* name;
    const char* synopsis;     // multi-line: each line shown after "usage:"
    const char* description;  // one-line summary; may be empty
    const char* options;      // multi-line option block; may be empty
    const char* mutation;     // mutation contract (empty for read-only verbs)
};

constexpr const char* kReadOnlyMutation = "Read-only: no model changes.";
constexpr const char* kPropMutation = "Read-only: proposal generation only.";

constexpr VerbHelp kVerbHelp[] = {
    {"inspect",
     "inspect input.step [--report report.json|-] [--log decisions.json]\n"
     "                            [--units mm|cm|m|in|...] [--scale X]\n"
     "                            [--normalize-units mm|cm|m|in|...]",
     "Analyze a STEP file; print a human summary or write JSON.",
     "  --report PATH         write JSON report to PATH (`-` for stdout)\n"
     "  --log PATH            open an append-only decision log alongside the run\n"
     "  --units NAME          assert the source unit (no geometry rescale)\n"
     "  --scale X             assert the source-unit-to-mm factor (no rescale)\n"
     "  --normalize-units NM  actually rescale geometry to NM (mm|cm|m|in|...)",
     kReadOnlyMutation},
    {"explain",
     "explain <shape-id> input.step [--report report.json|-]",
     "Explain why a target solid/piece/face is not emit-ready.",
     "  <shape-id>            solid:N, piece:N, or face:N.M (doc:1/ prefix ok)\n"
     "  --report PATH         write the underlying JSON analysis report",
     kReadOnlyMutation},
    {"decision-pack",
     "decision-pack input.step --out decisions/ [--target id] [--max-packets N]\n"
     "                                  [--hole-fill --host solid:N --near-removed solid:M]",
     "Bundle decision packets for a human or agent reviewer.",
     "", kReadOnlyMutation},
    {"dump-assembly",
     "dump-assembly input.step",
     "Print the XCAF assembly tree as text.", "", kReadOnlyMutation},
    {"inspect-tree",
     "inspect-tree input.step",
     "Print the per-solid status overlaid on the assembly tree.", "",
     kReadOnlyMutation},
    {"remove-suggest",
     "remove-suggest input.step [--report removals.json|-]",
     "Propose solids that are safe to remove.", "", kPropMutation},
    {"surface-suggest",
     "surface-suggest input.step face:N.M [--report proposal.json|-]",
     "Propose analytic-surface replacements for a spline face.", "",
     kPropMutation},
    {"hole-fill-suggest",
     "hole-fill-suggest input.step [--host solid:N] [--near-removed solid:M]\n"
     "                                  [--max-radius value] [--max-volume-fraction value]\n"
     "                                  [--max-candidates N] [--report holes.json|-]",
     "Propose synthetic plug solids for cylindrical holes.", "",
     kPropMutation},
    {"decompose-suggest",
     "decompose-suggest input.step solid:N [--report proposals.json|-]",
     "Propose decompositions for a non-convex solid.", "", kPropMutation},
    {"preprocess",
     "preprocess input.step --out-step simplified.step\n"
     "                                  [--policy auto|medium|low|conservative] [--max-depth N]\n"
     "                                  [--target solid:N] [--max-targets N] [--max-splits N]\n"
     "                                  [--apply-removal-candidates high|medium]\n"
     "                                  [--safe-all] [--dry-run]\n"
     "                                  [--allow-medium-confidence-auto] [--allow-bbox-fallback-auto]\n"
     "                                  [--log decisions.json] [--report report.json|-]",
     "Apply safe high-confidence operations and emit a simplified STEP.",
     "  --safe-all              compose every safe pass (auto-fit + BSP + high-confidence removals)\n"
     "  --dry-run               print the plan without mutating; --out-step optional\n"
     "  --apply-removal-candidates LEVEL  one of: high | medium (default: none)",
     "Mutation contract: may suppress solids selected for removal, may auto-fit\n"
     "  spline faces in place (when --safe-all), and may decompose non-convex\n"
     "  targets. Decomposed solid:N is suppressed and produces piece:1..K\n"
     "  (scoped doc:1/piece:N). Surviving solid:N IDs do NOT shift -- TopExp\n"
     "  order of surviving solids is stable."},
    {"decompose-apply",
     "decompose-apply input.step --out output.i\n"
     "                                  [--policy auto|medium|low|conservative] [--max-depth N]\n"
     "                                  [--target solid:N] [--max-targets N] [--max-splits N]\n"
     "                                  [--allow-medium-confidence-auto] [--allow-bbox-fallback-auto]\n"
     "                                  [--log decisions.json] [--out-step simplified.step] [--report report.json|-]",
     "Run safe BSP decomposition then emit MCNP.",
     "",
     "Mutation contract: decomposed solid:N is suppressed and produces\n"
     "  piece:1..K. Surviving solid:N IDs do NOT shift."},
    {"compact",
     "compact decisions.json --out recipe.lua",
     "Compact a decision log into a deterministic Lua recipe.", "",
     kReadOnlyMutation},
    {"replay",
     "replay recipe.json --step input.step --out output.i --report replay.json [--out-step simplified.step]",
     "Replay a recipe against a STEP file and emit MCNP.",
     "",
     "Mutation contract: the recipe's ops are applied in order. Each op's\n"
     "  contract is the same as its direct CLI form."},
    {"simplify",
     "simplify input.step <action> <target> --out output.i [options]",
     "Apply a single solid/face action and emit MCNP.",
     "  Solid actions (target solid:N):\n"
     "    --replace-solid-bbox            --replace-solid-bbox-cylinder\n"
     "    --replace-solid-fitted-cylinder --replace-solid-fitted-sphere\n"
     "    --replace-solid-fitted-cone     --decompose-planar\n"
     "    --auto-decompose-bsp\n"
     "    --fill-hole\n"
     "    --apply-decomposition-candidate --candidate candidate:N\n"
     "    --decompose-by-face --by-face face:N.M\n"
     "    --remove-solid\n"
     "  Face actions (target face:N.M, comma list allowed):\n"
     "    --replace-face-with-plane       --replace-face-with-cylinder\n"
     "    --replace-face-with-sphere      --replace-face-with-cone\n"
     "    --apply-surface-candidate --candidate candidate:N\n"
     "  Options:\n"
     "    --max-abs-delta value          (solid replacement volume guard)\n"
     "    --max-rel-delta value          (solid replacement volume guard)\n"
     "    --max-deviation value          (face replacement distance guard)\n"
     "    --max-depth value              (safe BSP auto-decomposition)\n"
     "    --allow-bbox-fallback-auto\n"
     "    --allow-medium-confidence-auto\n"
     "    --report PATH                  (`-` for stdout)\n"
     "    --proposal-report PATH         (`-` for stdout)\n"
     "    --host solid:N                 (fill-hole proposal context)\n"
     "    --near-removed solid:N         (fill-hole proposal context)\n"
     "    --max-radius value             (fill-hole proposal guard)\n"
     "    --max-volume-fraction value    (fill-hole proposal guard)\n"
     "    --log decisions.json           (append-only decision log)\n"
     "    --out-step simplified.step     (as-simplified geometry for review)",
     "Mutation contract:\n"
     "  --replace-solid-*    suppresses solid:N; emits a synthetic body with\n"
     "                       the same solid:N id. Surviving solid:N unchanged.\n"
     "  --remove-solid       suppresses solid:N. Surviving solid:N unchanged.\n"
     "  --decompose-*        suppresses solid:N; produces piece:1..K.\n"
     "                       Surviving solid:N unchanged.\n"
     "  --fill-hole          creates a synthetic plug (synthetic:N). Host\n"
     "                       solid:N id is unchanged. Surviving IDs unchanged.\n"
     "  --replace-face-*     replaces face geometry in place; solid:N id is\n"
     "                       unchanged. Surviving IDs unchanged."},
    {"auto-fit",
     "auto-fit input.step --out output.i [--max-deviation X] [--report report.json|-]",
     "Replace every spline face whose best fit meets --max-deviation.",
     "",
     "Mutation contract: replaces spline face geometry in place. Solid:N IDs\n"
     "  are not affected; only face contents change."},
    {"lua",
     "lua script.lua [args...]",
     "Run a Lua driver script against the cad2csg session API.", "",
     "Mutation contract: depends on the script. Each op called from Lua\n"
     "  follows the contract of its CLI equivalent."},
    {"volume",
     "volume input.step\n"
     "       volume original.step --compare candidate.step [--abs-tol value] [--rel-tol value]",
     "Report total volume; optionally compare two STEP files.", "",
     kReadOnlyMutation},
    {"propose",
     "propose <kind> <target?> input.step [--report report.json|-]\n"
     "  where <kind> is one of: decompose | surface | removal | hole-fill\n"
     "  <target> is solid:N for decompose, face:N.M for surface, omitted otherwise",
     "Generate candidate proposals (unified front door over the *-suggest verbs).",
     "  --report PATH                  write JSON proposal (`-` for stdout)\n"
     "  --host solid:N                 (hole-fill kind only)\n"
     "  --near-removed solid:N         (hole-fill kind only)\n"
     "  --max-radius value             (hole-fill kind only)\n"
     "  --max-volume-fraction value    (hole-fill kind only)\n"
     "  --max-candidates N             (hole-fill kind only)",
     kPropMutation},
    {"apply",
     "apply <kind> <target> input.step --out out.i [--candidate candidate:N] [options]\n"
     "  where <kind> is one of: decompose | surface | removal | hole-fill\n"
     "  --candidate required for decompose / surface / hole-fill",
     "Apply a candidate proposal (unified front door over simplify --apply-*-candidate).",
     "  --candidate candidate:N        the candidate id from `propose`\n"
     "  --log decisions.json           append-only decision log\n"
     "  --report PATH                  (`-` for stdout)",
     "Mutation contract: same as the simplify --apply-*-candidate it forwards to."},
    {"viewer-check",
     "viewer-check",
     "Probe whether the OCCT viewer can run in this environment.", "",
     kReadOnlyMutation},
    {"view",
     "view input.step [--select doc:N/solid:M|solid:M]\n"
     "                         [--candidate candidate:N]\n"
     "                         [--review-removal removal:N]\n"
     "                         [--review-hole-fill hole-fill:N [--host solid:N] [--near-removed solid:M]]\n"
     "                         [--review-decomp candidate:N --select solid:N]\n"
     "                         [--review-surface-replacement candidate:N --select-face face:N.M]",
     "Open the OCCT viewer for visual review.", "", kReadOnlyMutation},
    {"help",
     "help [verb]",
     "Show global usage, or the help for a specific verb.", "", ""},
    {"--version",
     "--version",
     "Print the cad2csg version, OCCT version, and Alea version.", "", ""},
};

const VerbHelp* lookup_verb_help(const std::string& name) {
    for (const VerbHelp& v : kVerbHelp) {
        if (name == v.name) return &v;
    }
    return nullptr;
}

void print_synopsis_line(std::ostream& out, const char* argv0,
                         const VerbHelp& v) {
    // Synopsis may be multi-line. The first line gets the "usage:" prefix;
    // continuation lines align under it.
    const std::string syn(v.synopsis);
    const std::size_t lead = std::strlen(argv0) + std::strlen("usage: ") + 1;
    std::size_t start = 0;
    bool first = true;
    while (start <= syn.size()) {
        const std::size_t nl = syn.find('\n', start);
        const std::string line = syn.substr(start,
                                            nl == std::string::npos
                                                ? syn.size() - start
                                                : nl - start);
        if (first) {
            out << "usage: " << argv0 << " " << line << "\n";
            first = false;
        } else {
            out << std::string(lead, ' ') << line << "\n";
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
}

void print_verb_help(std::ostream& out, const char* argv0, const VerbHelp& v) {
    print_synopsis_line(out, argv0, v);
    if (v.description && v.description[0]) {
        out << "\n" << v.description << "\n";
    }
    if (v.options && v.options[0]) {
        out << "\n" << v.options << "\n";
    }
    if (v.mutation && v.mutation[0]) {
        out << "\nMutation contract: " << v.mutation << "\n";
    }
}

void print_global_usage(std::ostream& out, const char* argv0) {
    out << "usage: " << argv0 << " input.step output.i      "
        << "(default: emit MCNP from STEP)\n";
    out << "       " << argv0 << " <verb> ...\n";
    out << "       " << argv0 << " help [verb]              "
        << "(per-verb help; also: <verb> --help)\n";
    out << "\nVerbs:\n";
    for (const VerbHelp& v : kVerbHelp) {
        // Skip --version (mentioned in synopsis) and the help meta-verb.
        if (std::strcmp(v.name, "--version") == 0) continue;
        if (std::strcmp(v.name, "help") == 0) continue;
        out << "  " << v.name;
        const std::size_t pad = std::strlen(v.name) >= 20
                                    ? 1
                                    : 20 - std::strlen(v.name);
        out << std::string(pad, ' ');
        out << (v.description ? v.description : "") << "\n";
    }
    out << "\n  --version              print versions and exit\n";
    out << "\nFor details on a verb: " << argv0 << " <verb> --help\n";
}

void usage(const char* argv0) {
    print_global_usage(std::cerr, argv0);
}

// Returns true if argv contains a top-level --help or -h token. Used by
// every run_*_command so that `<verb> --help` prints that verb's help
// on stdout and exits 0 instead of falling through to argument parsing
// and erroring on argc.
bool wants_help(int argc, char** argv) {
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0
            || std::strcmp(argv[i], "-h") == 0) {
            return true;
        }
    }
    return false;
}

int print_verb_help_and_exit(const char* argv0, const std::string& verb) {
    const VerbHelp* v = lookup_verb_help(verb);
    if (!v) {
        std::cerr << "internal_error: no help wired for verb '" << verb
                  << "'\n";
        return EXIT_FAILURE;
    }
    print_verb_help(std::cout, argv0, *v);
    return EXIT_SUCCESS;
}

// Path-like heuristic. Used by main() to decide whether a non-verb
// argv[1] looks like a STEP path (in which case we run the default
// convert), or whether it is a typo'd verb that we should reject with
// "unknown command" + a did-you-mean suggestion.
bool is_step_path_like(const char* arg) {
    if (!arg || !arg[0]) return false;
    const std::size_t n = std::strlen(arg);
    auto ends_with = [&](const char* suf) {
        const std::size_t sn = std::strlen(suf);
        if (n < sn) return false;
        for (std::size_t i = 0; i < sn; ++i) {
            const char a = std::tolower(static_cast<unsigned char>(arg[n - sn + i]));
            const char b = std::tolower(static_cast<unsigned char>(suf[i]));
            if (a != b) return false;
        }
        return true;
    };
    if (ends_with(".step") || ends_with(".stp")) return true;
    if (std::strchr(arg, '/') != nullptr) return true;
    struct stat st;
    return ::stat(arg, &st) == 0;
}

int edit_distance(const std::string& a, const std::string& b) {
    const std::size_t na = a.size();
    const std::size_t nb = b.size();
    if (na == 0) return static_cast<int>(nb);
    if (nb == 0) return static_cast<int>(na);
    std::vector<int> prev(nb + 1);
    std::vector<int> cur(nb + 1);
    for (std::size_t j = 0; j <= nb; ++j) prev[j] = static_cast<int>(j);
    for (std::size_t i = 1; i <= na; ++i) {
        cur[0] = static_cast<int>(i);
        for (std::size_t j = 1; j <= nb; ++j) {
            const int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
        }
        prev.swap(cur);
    }
    return prev[nb];
}

const char* suggest_verb(const std::string& typo) {
    const int threshold = typo.size() <= 4 ? 1 : static_cast<int>(typo.size() / 3 + 1);
    int best = threshold + 1;
    const char* best_name = nullptr;
    for (const VerbHelp& v : kVerbHelp) {
        if (v.name[0] == '-') continue;
        const int d = edit_distance(typo, v.name);
        if (d < best) {
            best = d;
            best_name = v.name;
        }
    }
    return best_name;
}

// `-` is the conventional stdout sentinel; every verb that takes a
// --report path routes through these helpers so the convention is
// uniform.
bool is_stdout_path(const std::string& path) { return path == "-"; }

int write_report_or_stdout(const cad2csg::ConversionReport& report,
                           const std::string& path) {
    if (is_stdout_path(path)) {
        std::cout << cad2csg::report_json_string(report);
        return 0;
    }
    return cad2csg::write_report_json(report, path);
}

int write_decomp_proposal_or_stdout(const cad2csg::DecompositionProposal& p,
                                    const std::string& path) {
    if (is_stdout_path(path)) {
        std::cout << cad2csg::decomposition_proposal_json_string(p);
        return 0;
    }
    return cad2csg::write_decomposition_proposal_json(p, path);
}

int write_removal_proposal_or_stdout(const cad2csg::RemovalProposal& p,
                                     const std::string& path) {
    if (is_stdout_path(path)) {
        std::cout << cad2csg::removal_proposal_json_string(p);
        return 0;
    }
    return cad2csg::write_removal_proposal_json(p, path);
}

int write_hole_fill_proposal_or_stdout(const cad2csg::HoleFillProposal& p,
                                       const std::string& path) {
    if (is_stdout_path(path)) {
        std::cout << cad2csg::hole_fill_proposal_json_string(p);
        return 0;
    }
    return cad2csg::write_hole_fill_proposal_json(p, path);
}

int write_surface_proposal_or_stdout(
    const cad2csg::SurfaceReplacementProposal& p,
    const std::string& path) {
    if (is_stdout_path(path)) {
        std::cout << cad2csg::surface_replacement_proposal_json_string(p);
        return 0;
    }
    return cad2csg::write_surface_replacement_proposal_json(p, path);
}

// Compact human summary for `inspect` without --report. The JSON report
// is canonical and exposes every field; this is the at-a-glance view
// for a user who just typed `cad2csg inspect file.step`.
std::string solid_status_label(const cad2csg::AnalyzedSolid& solid);

void print_human_inspect(std::ostream& out,
                         const cad2csg::ConversionReport& report) {
    std::size_t ready = 0;
    for (const cad2csg::AnalyzedSolid& s : report.solids) {
        if (s.exact_csg_supported) ++ready;
    }
    out << "cad2csg inspect";
    if (!report.source_path.empty()) {
        out << " " << report.source_path;
    }
    out << "\n";
    out << "  solids:  " << ready << "/" << report.solids_seen
        << " emit-ready";
    if (report.solids_requiring_decomposition > 0) {
        out << ", " << report.solids_requiring_decomposition
            << " need decomposition";
    }
    out << "\n";
    out << "  faces:   " << report.analytic_faces << "/" << report.faces_seen
        << " analytic, " << report.spline_faces << " spline\n";
    out << "  volume:  " << report.total_volume;
    if (!report.length_unit.empty()) {
        out << " " << report.length_unit << "^3";
    }
    out << "\n";
    out << "  units:   "
        << (report.length_unit.empty() ? "unknown (assumed mm)" : report.length_unit);
    if (!report.length_unit.empty()
        && report.length_unit != "mm"
        && report.length_unit_to_mm > 0.0) {
        out << " (x" << report.length_unit_to_mm << " to mm)";
    }
    out << "\n";
    if (!report.solids.empty()) {
        out << "\nPer-solid status:\n";
        for (const cad2csg::AnalyzedSolid& s : report.solids) {
            out << "  " << s.id << "  " << solid_status_label(s)
                << "  vol=" << s.volume;
            if (!s.name.empty()) out << "  " << s.name;
            out << "\n";
        }
    }
    if (!report.diagnostics.empty()) {
        out << "\nDiagnostics:\n";
        for (const cad2csg::Diagnostic& d : report.diagnostics) {
            out << "  " << severity_name(d.severity) << " " << d.code;
            if (!d.shape_label.empty()) out << " " << d.shape_label;
            out << ": " << d.message << "\n";
        }
    }
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
              << ", volume: " << report.total_volume;
    if (!report.length_unit.empty()) {
        std::cerr << " " << report.length_unit << "^3";
    }
    std::cerr << '\n';
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
    if (wants_help(argc, argv)) return print_verb_help_and_exit(argv[0], "volume");
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
    if (wants_help(argc, argv)) return print_verb_help_and_exit(argv[0], "view");
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
    if (wants_help(argc, argv)) return print_verb_help_and_exit(argv[0], "decompose-suggest");
    std::cerr << "info deprecated_verb: `decompose-suggest` will be removed; "
              << "use `propose decompose <solid:N> <input.step>` instead.\n";
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
        if (write_decomp_proposal_or_stdout(proposal, report_path) != 0) {
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
    if (wants_help(argc, argv)) return print_verb_help_and_exit(argv[0], "decision-pack");
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
    if (wants_help(argc, argv)) return print_verb_help_and_exit(argv[0], "remove-suggest");
    std::cerr << "info deprecated_verb: `remove-suggest` will be removed; "
              << "use `propose removal <input.step>` instead.\n";
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
        if (write_removal_proposal_or_stdout(proposal, report_path) != 0) {
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
    if (wants_help(argc, argv)) return print_verb_help_and_exit(argv[0], "hole-fill-suggest");
    std::cerr << "info deprecated_verb: `hole-fill-suggest` will be removed; "
              << "use `propose hole-fill <input.step>` instead.\n";
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
        if (write_hole_fill_proposal_or_stdout(proposal, report_path) != 0) {
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
    if (wants_help(argc, argv)) return print_verb_help_and_exit(argv[0], "surface-suggest");
    std::cerr << "info deprecated_verb: `surface-suggest` will be removed; "
              << "use `propose surface <face:N.M> <input.step>` instead.\n";
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
        if (write_surface_proposal_or_stdout(proposal, report_path) != 0) {
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
    if (wants_help(argc, argv)) return print_verb_help_and_exit(argv[0], "simplify");
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
                if (!report_path.empty()
                    && write_report_or_stdout(action_result.report,
                                              report_path) != 0) {
                    std::cerr << "error report_write_failed: "
                              << report_path << '\n';
                }
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
            proposal_rc = write_hole_fill_proposal_or_stdout(
                cad2csg::last_hole_fill_proposal(*session),
                proposal_report_path);
        } else if (action == "--apply-surface-candidate") {
            proposal_rc = write_surface_proposal_or_stdout(
                cad2csg::last_surface_replacement_proposal(*session),
                proposal_report_path);
        } else {
            proposal_rc = write_decomp_proposal_or_stdout(
                cad2csg::last_decomposition_proposal(*session),
                proposal_report_path);
        }
        if (proposal_rc != 0) {
            std::cerr << "error proposal_report_write_failed: "
                      << proposal_report_path << '\n';
            return EXIT_FAILURE;
        }
    }

    if (!action_result.ok) {
        // The requested action did not apply. Write the post-action
        // report if --report was given so the caller can see the
        // failure diagnostics. Exit code is EXIT_FAILURE.
        print_diagnostics(action_result.report);
        if (!report_path.empty()
            && write_report_or_stdout(action_result.report, report_path) != 0) {
            std::cerr << "error report_write_failed: " << report_path << '\n';
        }
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
        && write_report_or_stdout(result.report, report_path) != 0) {
        std::cerr << "error report_write_failed: " << report_path << '\n';
        return EXIT_FAILURE;
    }

    if (!result.report.ok() || result.report.solids_converted == 0) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

int run_auto_fit_command(int argc, char** argv) {
    if (wants_help(argc, argv)) return print_verb_help_and_exit(argv[0], "auto-fit");
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
        && write_report_or_stdout(result.report, report_path) != 0) {
        std::cerr << "error report_write_failed: " << report_path << '\n';
        return EXIT_FAILURE;
    }

    if (!result.report.ok() || result.report.solids_converted == 0) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

int run_inspect_command(int argc, char** argv) {
    if (wants_help(argc, argv)) return print_verb_help_and_exit(argv[0], "inspect");
    if (argc < 3) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    const std::string input_path = argv[2];
    std::string report_path;
    std::string log_path;
    std::string units_override;
    std::string normalize_to;
    double scale_override = 0.0;

    OptScanner scan(argc, argv, 3);
    while (!scan.done()) {
        if (scan.take_string("--report", &report_path)) continue;
        if (scan.take_string("--log", &log_path)) continue;
        if (scan.take_string("--units", &units_override)) continue;
        if (scan.take_string("--normalize-units", &normalize_to)) continue;
        if (scan.take_double("--scale", &scale_override)) continue;
        std::cerr << "error unrecognized_option: " << scan.peek() << '\n';
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    std::cerr << "cad2csg: OCCT " << cad2csg::occt_version()
              << ", Alea " << alea_version() << '\n';

    cad2csg::ConversionOptions options;
    options.unit_override = units_override;
    options.normalize_to_unit = normalize_to;
    options.unit_scale_to_mm_override = scale_override;
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

    // No --report: print a human-readable summary to stdout. With --report
    // (file or `-`), write JSON there and keep a one-line summary on stderr.
    if (report_path.empty()) {
        print_human_inspect(std::cout, report);
    } else {
        print_summary(report);
        if (write_report_or_stdout(report, report_path) != 0) {
            std::cerr << "error report_write_failed: " << report_path << '\n';
            return EXIT_FAILURE;
        }
    }
    return report.ok() ? EXIT_SUCCESS : EXIT_FAILURE;
}

// Human-readable face description used by `explain` to surface why a
// face is blocking emission. Anchored on the AnalyzedFace fields the
// analyzer already populates -- explain is a *read-only view* over
// existing analysis, not new geometry logic.
const char* face_blocker_reason(const cad2csg::AnalyzedFace& f) {
    if (f.exact_csg_supported) return "supported";
    if (f.needs_spline_replacement) return "spline (needs analytic fit)";
    if (f.needs_surface_replacement) return "surface needs replacement";
    if (f.needs_emitter) return "no emitter wired for this surface";
    return "unsupported";
}

const char* face_suggested_action(const cad2csg::AnalyzedFace& f) {
    if (f.exact_csg_supported) return "none";
    if (f.needs_spline_replacement || f.needs_surface_replacement) {
        return "replace-face-with-* (try `surface-suggest` to score candidates)";
    }
    return "decompose-by-face or manual review";
}

int run_explain_command(int argc, char** argv) {
    if (wants_help(argc, argv)) return print_verb_help_and_exit(argv[0], "explain");
    if (argc < 4) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    const std::string target_id = argv[2];
    const std::string input_path = argv[3];
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
    cad2csg::CommandResult analysis =
        cad2csg::analyze(*session, "doc:1", cad2csg::ReportIdMode::local);
    const cad2csg::ConversionReport& report = analysis.report;

    // Locate the target. Accept solid:N, piece:N, and face:N.M (with or
    // without doc:1/ prefix). The analyzer's report carries one row per
    // active solid/piece, plus one row per face per solid.
    const cad2csg::AnalyzedSolid* solid_hit = nullptr;
    const cad2csg::AnalyzedDecomposedPiece* piece_hit = nullptr;
    const cad2csg::AnalyzedFace* face_hit = nullptr;

    auto strip_doc_prefix = [](std::string s) {
        const std::string prefix = "doc:1/";
        if (s.rfind(prefix, 0) == 0) s.erase(0, prefix.size());
        return s;
    };
    const std::string id = strip_doc_prefix(target_id);

    for (const cad2csg::AnalyzedSolid& s : report.solids) {
        if (s.id == id) { solid_hit = &s; break; }
    }
    if (!solid_hit) {
        for (const cad2csg::AnalyzedDecomposedPiece& p : report.decomposed_pieces) {
            if (p.id == id) { piece_hit = &p; break; }
        }
    }
    if (!solid_hit && !piece_hit) {
        for (const cad2csg::AnalyzedFace& f : report.faces) {
            if (f.id == id) { face_hit = &f; break; }
        }
    }

    if (!solid_hit && !piece_hit && !face_hit) {
        std::cerr << "error explain_target_not_found: '" << target_id
                  << "' is not a solid, piece, or face in this STEP\n";
        return EXIT_FAILURE;
    }

    // Print a human-readable explanation. Exit code reflects whether
    // the target is already emit-ready: 0 if ready, 1 if it still
    // needs a decision/decompose/replace.
    bool target_ready = false;

    if (solid_hit) {
        target_ready = solid_hit->exact_csg_supported;
        std::cout << "explain " << target_id << "\n";
        std::cout << "  kind:    solid\n";
        std::cout << "  status:  " << solid_status_label(*solid_hit) << "\n";
        std::cout << "  volume:  " << solid_hit->volume;
        if (!report.length_unit.empty()) std::cout << " " << report.length_unit << "^3";
        std::cout << "\n";
        if (!solid_hit->name.empty()) {
            std::cout << "  name:    " << solid_hit->name << "\n";
        }
        if (target_ready) {
            std::cout << "  ready:   yes; emit will produce exact CSG.\n";
        } else {
            std::cout << "  ready:   no\n";
            if (solid_hit->needs_decomposition) {
                std::cout << "  blocker: non-convex (needs decomposition)\n";
            }
            if (solid_hit->needs_spline_replacement) {
                std::cout << "  blocker: spline face(s) need analytic fit\n";
            }
            if (solid_hit->needs_surface_replacement) {
                std::cout << "  blocker: face surface(s) need replacement\n";
            }
            if (solid_hit->needs_user_decision) {
                std::cout << "  blocker: user decision required\n";
            }
        }

        // Per-face details for blocking faces.
        std::vector<const cad2csg::AnalyzedFace*> blocking_faces;
        for (const cad2csg::AnalyzedFace& f : report.faces) {
            if (f.solid_id == solid_hit->id && !f.exact_csg_supported) {
                blocking_faces.push_back(&f);
            }
        }
        if (!blocking_faces.empty()) {
            std::cout << "\nBlocking faces:\n";
            for (const cad2csg::AnalyzedFace* f : blocking_faces) {
                std::cout << "  " << f->id << "  type=" << f->surface_type
                          << "  " << face_blocker_reason(*f) << "\n";
                std::cout << "    action: " << face_suggested_action(*f) << "\n";
            }
        }

        // Decomposition proposals (if non-convex).
        if (solid_hit->needs_decomposition) {
            cad2csg::DecompositionProposal proposal =
                cad2csg::propose_decomposition(*session, solid_hit->id,
                                                cad2csg::ReportIdMode::local);
            std::cout << "\nDecomposition candidates: " << proposal.candidates.size();
            if (!proposal.best_candidate_id.empty()) {
                std::cout << " (best: " << proposal.best_candidate_id << ")";
            }
            std::cout << "\n";
            const std::size_t limit = std::min<std::size_t>(
                3, proposal.candidates.size());
            for (std::size_t i = 0; i < limit; ++i) {
                const cad2csg::DecompositionCandidate& c = proposal.candidates[i];
                std::cout << "  " << c.id << "  " << c.tool_kind
                          << "  confidence=" << c.confidence
                          << "  pieces=" << c.piece_count
                          << "  rel_delta=" << c.relative_delta << "\n";
            }
            if (proposal.candidates.size() > limit) {
                std::cout << "  ... (" << (proposal.candidates.size() - limit)
                          << " more; run `decompose-suggest` for the full list)\n";
            }
        }
    } else if (piece_hit) {
        target_ready = piece_hit->exact_csg_supported;
        std::cout << "explain " << target_id << "\n";
        std::cout << "  kind:    piece (from decomposition)\n";
        std::cout << "  source:  " << piece_hit->source_solid_id << "\n";
        std::cout << "  volume:  " << piece_hit->volume;
        if (!report.length_unit.empty()) std::cout << " " << report.length_unit << "^3";
        std::cout << "\n";
        std::cout << "  ready:   " << (target_ready ? "yes" : "no") << "\n";
    } else if (face_hit) {
        target_ready = face_hit->exact_csg_supported;
        std::cout << "explain " << target_id << "\n";
        std::cout << "  kind:    face\n";
        std::cout << "  type:    " << face_hit->surface_type << "\n";
        std::cout << "  solid:   " << face_hit->solid_id << "\n";
        std::cout << "  area:    " << face_hit->area << "\n";
        std::cout << "  ready:   " << (target_ready ? "yes" : "no") << "\n";
        if (!target_ready) {
            std::cout << "  blocker: " << face_blocker_reason(*face_hit) << "\n";
            std::cout << "  action:  " << face_suggested_action(*face_hit) << "\n";
            // Surface-replacement candidates for non-ready faces.
            cad2csg::SurfaceReplacementProposal proposal =
                cad2csg::propose_surface_replacement(*session, face_hit->id,
                                                       cad2csg::ReportIdMode::local);
            if (!proposal.candidates.empty()) {
                std::cout << "\nSurface-replacement candidates: "
                          << proposal.candidates.size();
                if (!proposal.best_candidate_id.empty()) {
                    std::cout << " (best: " << proposal.best_candidate_id << ")";
                }
                std::cout << "\n";
                const std::size_t limit = std::min<std::size_t>(
                    3, proposal.candidates.size());
                for (std::size_t i = 0; i < limit; ++i) {
                    const cad2csg::SurfaceReplacementCandidate& c =
                        proposal.candidates[i];
                    std::cout << "  " << c.id << "  " << c.kind
                              << "  confidence=" << c.confidence
                              << "  max_dev=" << c.max_deviation << "\n";
                }
            }
        }
    }

    if (!report_path.empty()) {
        if (write_report_or_stdout(report, report_path) != 0) {
            std::cerr << "error report_write_failed: " << report_path << '\n';
            return EXIT_FAILURE;
        }
    }

    return target_ready ? EXIT_SUCCESS : EXIT_FAILURE;
}

int run_decompose_apply_command(int argc, char** argv) {
    if (wants_help(argc, argv)) return print_verb_help_and_exit(argv[0], "decompose-apply");
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
        && write_report_or_stdout(result.report, report_path) != 0) {
        std::cerr << "error report_write_failed: " << report_path << '\n';
        return EXIT_FAILURE;
    }

    if (!result.report.ok() || result.report.solids_converted == 0) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

int run_preprocess_command(int argc, char** argv) {
    if (wants_help(argc, argv)) return print_verb_help_and_exit(argv[0], "preprocess");
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
    bool safe_all = false;
    bool dry_run = false;

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
        if (scan.take_flag("--safe-all")) { safe_all = true; continue; }
        if (scan.take_flag("--dry-run")) { dry_run = true; continue; }
        std::cerr << "error unrecognized_option: " << scan.peek() << '\n';
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (step_out_path.empty() && !dry_run) {
        std::cerr << "error preprocess_requires_out_step: preprocess requires --out-step (omit only with --dry-run)\n";
        return EXIT_FAILURE;
    }
    // --safe-all implies the safe removal policy unless one is given
    // explicitly. Hole fills stay opt-in per
    // HOLE_FILL_IMPLEMENTATION_PLAN.md (fills are modeling decisions,
    // not safe ops).
    if (safe_all && apply_removal_confidence.empty()) {
        apply_removal_confidence = "high";
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

    // --safe-all extends preprocess with a leading auto-fit pass over
    // spline faces that meet --max-deviation. This is the only place
    // we touch face geometry; the rest of preprocess still operates
    // on whole solids. Skipped in --dry-run mode -- we just count.
    std::size_t auto_fit_applied = 0;
    std::size_t auto_fit_seen = 0;
    if (safe_all && !dry_run) {
        cad2csg::AutoFitCommandResult af =
            cad2csg::auto_fit_splines_with_summary(
                *session, document_id,
                /*has_max_deviation=*/false, /*max_deviation=*/0.0,
                cad2csg::ReportIdMode::local);
        auto_fit_applied = af.summary.applied;
        auto_fit_seen = af.summary.spline_faces_seen;
        std::cerr << "preprocess: auto-fit replaced " << auto_fit_applied
                  << "/" << auto_fit_seen << " spline faces"
                  << " (over-budget=" << af.summary.skipped_over_budget
                  << ", no-fit=" << af.summary.skipped_no_fit << ")\n";
    } else if (safe_all && dry_run) {
        cad2csg::CommandResult analyse =
            cad2csg::analyze(*session, document_id, cad2csg::ReportIdMode::local);
        for (const cad2csg::AnalyzedFace& f : analyse.report.faces) {
            if (f.needs_spline_replacement) ++auto_fit_seen;
        }
        std::cerr << "preprocess [dry-run]: would auto-fit up to "
                  << auto_fit_seen << " spline faces\n";
    }

    std::size_t removed = 0;
    std::size_t removal_candidates_total = 0;
    if (!apply_removal_confidence.empty()) {
        cad2csg::RemovalProposal removals =
            cad2csg::suggest_removals(*session,
                                      document_id,
                                      cad2csg::ReportIdMode::local);
        for (const cad2csg::RemovalCandidate& candidate : removals.candidates) {
            const bool allowed = candidate.confidence == "high"
                || (apply_removal_confidence == "medium"
                    && candidate.confidence == "medium");
            if (!allowed || candidate.suggested_action == "manual") {
                continue;
            }
            ++removal_candidates_total;
            if (dry_run) continue;
            cad2csg::CommandResult r =
                cad2csg::remove_solid(*session,
                                      candidate.solid_id,
                                      cad2csg::ReportIdMode::local);
            if (r.ok) {
                ++removed;
            }
        }
        if (dry_run) {
            std::cerr << "preprocess [dry-run]: would remove "
                      << removal_candidates_total
                      << " component(s) (policy=" << apply_removal_confidence
                      << ")\n";
        } else {
            std::cerr << "preprocess: removed " << removed
                      << " component(s) from removal candidates"
                      << " (policy=" << apply_removal_confidence << ")\n";
        }
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
    if (dry_run) {
        std::cerr << "preprocess [dry-run]: would attempt BSP decomposition on "
                  << targets.size() << " non-convex target(s)"
                  << " (policy=" << policy_name
                  << ", max-depth=" << max_depth_value << ")\n";
    } else {
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
    }

    if (!dry_run) {
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
    }

    // --dry-run never mutates beyond what we already counted; emit the
    // closing summary (if --safe-all) and exit.
    if (dry_run) {
        if (safe_all) {
            std::cerr << "preprocess [safe-all] summary: would auto-fit "
                      << auto_fit_seen << " spline face(s), remove "
                      << removal_candidates_total << " component(s), "
                      << "decompose " << targets.size()
                      << " non-convex target(s)\n";
        }
        return EXIT_SUCCESS;
    }

    // Run export BEFORE the final analyze so performance counters
    // (export_step_ms) accumulate before we serialize the report.
    cad2csg::CommandResult step_result =
        cad2csg::export_document_step(*session, document_id, step_out_path,
                                      cad2csg::ReportIdMode::local);

    // Re-analyze once so the closing summary and the on-disk report
    // both reflect the final post-mutation state; this also picks up
    // the export_step_ms accumulated by the call above.
    cad2csg::CommandResult final_analysis =
        cad2csg::analyze(*session, document_id, cad2csg::ReportIdMode::local);

    // Closing "what's left for you" summary. After all safe ops, count
    // solids that still need attention. This is the user-facing answer
    // to "what does --safe-all leave behind?".
    if (safe_all) {
        std::size_t still_needs = 0;
        std::size_t still_decisions = 0;
        for (const cad2csg::AnalyzedSolid& s : final_analysis.report.solids) {
            if (s.exact_csg_supported) continue;
            ++still_needs;
            if (s.needs_user_decision) ++still_decisions;
        }
        std::cerr << "preprocess [safe-all] summary: "
                  << "auto-fit=" << auto_fit_applied
                  << ", removed=" << removed
                  << ", decomposed=" << succeeded
                  << "; remaining=" << still_needs
                  << " solid(s) need attention (" << still_decisions
                  << " require user decisions)\n";
    }

    if (!report_path.empty()
        && write_report_or_stdout(final_analysis.report, report_path) != 0) {
        std::cerr << "error report_write_failed: " << report_path << '\n';
        return EXIT_FAILURE;
    }

    return step_result.ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

// `propose` and `apply` are the noun-first front door for the
// proposal/apply idiom (WS6 of UX_AND_FEATURES_IMPLEMENTATION_PLAN.md).
// They dispatch by <kind> to the existing session calls (the engine
// surfaces are unchanged) so old recipes/scripts continue to work.

int run_propose_command(int argc, char** argv) {
    if (wants_help(argc, argv)) return print_verb_help_and_exit(argv[0], "propose");
    if (argc < 4) {
        std::cerr << "error propose_missing_args: "
                  << "usage: propose <kind> <target?> input.step [options]\n";
        return EXIT_FAILURE;
    }
    const std::string kind = argv[2];

    // Parse positional args by kind. decompose/surface require a target
    // (solid:N or face:N.M); removal and hole-fill take none.
    std::string target_id;
    std::string input_path;
    int opts_start = 0;
    if (kind == "decompose" || kind == "surface") {
        if (argc < 5) {
            std::cerr << "error propose_missing_args: "
                      << "propose " << kind << " needs <target> input.step\n";
            return EXIT_FAILURE;
        }
        target_id = argv[3];
        input_path = argv[4];
        opts_start = 5;
    } else if (kind == "removal" || kind == "hole-fill") {
        input_path = argv[3];
        opts_start = 4;
    } else {
        std::cerr << "error propose_unknown_kind: '" << kind
                  << "' (expected decompose|surface|removal|hole-fill)\n";
        return EXIT_FAILURE;
    }

    std::string report_path;
    cad2csg::HoleFillOptions hole_options;
    OptScanner scan(argc, argv, opts_start);
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
        return EXIT_FAILURE;
    }

    cad2csg::ConversionOptions options;
    cad2csg::SessionPtr session = cad2csg::create_session(options);
    const cad2csg::DocumentId document_id =
        cad2csg::load_step(*session, input_path);

    if (kind == "decompose") {
        cad2csg::DecompositionProposal p =
            cad2csg::propose_decomposition(*session, target_id,
                                            cad2csg::ReportIdMode::local);
        if (!report_path.empty()) {
            if (write_decomp_proposal_or_stdout(p, report_path) != 0) {
                std::cerr << "error report_write_failed: " << report_path << '\n';
                return EXIT_FAILURE;
            }
        } else {
            std::cout << cad2csg::decomposition_proposal_json_string(p);
        }
        std::cerr << "propose decompose: " << p.candidates.size()
                  << " candidate(s), best: "
                  << (p.best_candidate_id.empty() ? "-" : p.best_candidate_id)
                  << ", ok: " << (p.ok ? "true" : "false") << '\n';
        return p.ok ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (kind == "surface") {
        cad2csg::SurfaceReplacementProposal p =
            cad2csg::propose_surface_replacement(*session, target_id,
                                                  cad2csg::ReportIdMode::local);
        if (!report_path.empty()) {
            if (write_surface_proposal_or_stdout(p, report_path) != 0) {
                std::cerr << "error report_write_failed: " << report_path << '\n';
                return EXIT_FAILURE;
            }
        } else {
            std::cout << cad2csg::surface_replacement_proposal_json_string(p);
        }
        std::cerr << "propose surface: " << p.candidates.size()
                  << " candidate(s), best: "
                  << (p.best_candidate_id.empty() ? "-" : p.best_candidate_id)
                  << ", ok: " << (p.ok ? "true" : "false") << '\n';
        return p.ok ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (kind == "removal") {
        cad2csg::RemovalProposal p =
            cad2csg::suggest_removals(*session, document_id,
                                       cad2csg::ReportIdMode::local);
        if (!report_path.empty()) {
            if (write_removal_proposal_or_stdout(p, report_path) != 0) {
                std::cerr << "error report_write_failed: " << report_path << '\n';
                return EXIT_FAILURE;
            }
        } else {
            std::cout << cad2csg::removal_proposal_json_string(p);
        }
        std::cerr << "propose removal: " << p.candidates.size()
                  << " candidate(s), high-confidence: " << p.high_confidence_count
                  << ", ok: " << (p.ok ? "true" : "false") << '\n';
        return p.ok ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    // kind == "hole-fill"
    cad2csg::HoleFillProposal p =
        cad2csg::propose_hole_fills(*session, document_id, hole_options,
                                     cad2csg::ReportIdMode::local);
    if (!report_path.empty()) {
        if (write_hole_fill_proposal_or_stdout(p, report_path) != 0) {
            std::cerr << "error report_write_failed: " << report_path << '\n';
            return EXIT_FAILURE;
        }
    } else {
        std::cout << cad2csg::hole_fill_proposal_json_string(p);
    }
    std::cerr << "propose hole-fill: " << p.candidates.size()
              << " candidate(s), ok: " << (p.ok ? "true" : "false") << '\n';
    return p.ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

int run_apply_command(int argc, char** argv) {
    if (wants_help(argc, argv)) return print_verb_help_and_exit(argv[0], "apply");
    if (argc < 5) {
        std::cerr << "error apply_missing_args: "
                  << "usage: apply <kind> <target> input.step --out out.i [options]\n";
        return EXIT_FAILURE;
    }
    const std::string kind = argv[2];
    const std::string target_id = argv[3];
    const std::string input_path = argv[4];

    std::string output_path;
    std::string report_path;
    std::string log_path;
    std::string candidate_id;
    OptScanner scan(argc, argv, 5);
    while (!scan.done()) {
        if (scan.take_string("--out", &output_path)) continue;
        if (scan.take_string("--report", &report_path)) continue;
        if (scan.take_string("--log", &log_path)) continue;
        if (scan.take_string("--candidate", &candidate_id)) continue;
        std::cerr << "error unrecognized_option: " << scan.peek() << '\n';
        return EXIT_FAILURE;
    }
    if (output_path.empty()) {
        std::cerr << "error apply_requires_out: apply requires --out output.i\n";
        return EXIT_FAILURE;
    }
    const bool needs_candidate =
        (kind == "decompose" || kind == "surface" || kind == "hole-fill");
    if (needs_candidate && candidate_id.empty()) {
        std::cerr << "error apply_requires_candidate: "
                  << "apply " << kind << " needs --candidate candidate:N\n";
        return EXIT_FAILURE;
    }

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
    const cad2csg::DocumentId document_id =
        cad2csg::load_step(*session, input_path);

    cad2csg::CommandResult action_result;
    if (kind == "decompose") {
        action_result = cad2csg::apply_decomposition_candidate(
            *session, target_id, candidate_id, cad2csg::ReportIdMode::local);
    } else if (kind == "surface") {
        action_result = cad2csg::apply_surface_replacement_candidate(
            *session, target_id, candidate_id, cad2csg::ReportIdMode::local);
    } else if (kind == "removal") {
        action_result = cad2csg::remove_solid(
            *session, target_id, cad2csg::ReportIdMode::local);
    } else if (kind == "hole-fill") {
        action_result = cad2csg::fill_hole(
            *session, document_id, candidate_id, cad2csg::ReportIdMode::local);
    } else {
        std::cerr << "error apply_unknown_kind: '" << kind
                  << "' (expected decompose|surface|removal|hole-fill)\n";
        return EXIT_FAILURE;
    }

    if (!action_result.ok) {
        // Action did not apply; write the post-action report when
        // --report was given so the caller sees the failure detail.
        print_diagnostics(action_result.report);
        if (!report_path.empty()
            && write_report_or_stdout(action_result.report, report_path) != 0) {
            std::cerr << "error report_write_failed: " << report_path << '\n';
        }
        return EXIT_FAILURE;
    }

    cad2csg::CommandResult emit_result =
        cad2csg::emit_document(*session, document_id, output_path,
                                cad2csg::ReportIdMode::local);
    print_diagnostics(emit_result.report);
    print_summary(emit_result.report);

    if (!report_path.empty()
        && write_report_or_stdout(emit_result.report, report_path) != 0) {
        std::cerr << "error report_write_failed: " << report_path << '\n';
        return EXIT_FAILURE;
    }
    if (!emit_result.report.ok() || emit_result.report.solids_converted == 0) {
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
    if (wants_help(argc, argv)) return print_verb_help_and_exit(argv[0], "replay");
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
    if (wants_help(argc, argv)) return print_verb_help_and_exit(argv[0], "compact");
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
    if (wants_help(argc, argv)) return print_verb_help_and_exit(argv[0], "dump-assembly");
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
    if (wants_help(argc, argv)) return print_verb_help_and_exit(argv[0], "inspect-tree");
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
    if (argc >= 3
        && (std::strcmp(argv[2], "--help") == 0
            || std::strcmp(argv[2], "-h") == 0)) {
        return print_verb_help_and_exit(argv[0], "lua");
    }
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

    // help / --help / -h: print global usage on stdout (exit 0). With a
    // second arg, treat it as a verb name and print that verb's help.
    if (verb == "help" || verb == "--help" || verb == "-h") {
        if (argc >= 3) {
            const VerbHelp* v = lookup_verb_help(argv[2]);
            if (!v) {
                std::cerr << "error unknown_verb: " << argv[2] << '\n';
                return EXIT_FAILURE;
            }
            print_verb_help(std::cout, argv[0], *v);
            return EXIT_SUCCESS;
        }
        print_global_usage(std::cout, argv[0]);
        return EXIT_SUCCESS;
    }

    if (verb == "inspect") return run_inspect_command(argc, argv);
    if (verb == "explain") return run_explain_command(argc, argv);
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
    if (verb == "propose") return run_propose_command(argc, argv);
    if (verb == "apply") return run_apply_command(argc, argv);
    if (verb == "lua") return run_lua_command(argc, argv);
    if (verb == "viewer-check") return run_viewer_check_command();
    if (verb == "view") return run_view_command(argc, argv);

    // Fall-through: distinguish typo'd verbs from the bare two-arg
    // convert form (`input.step output.i`). Treat argv[1] as a path
    // only if it looks like one (.step/.stp suffix, contains /, or
    // exists on disk); otherwise report the typo with a did-you-mean.
    if (!is_step_path_like(argv[1])) {
        std::cerr << "error unknown_command: '" << argv[1] << "'";
        const char* suggestion = suggest_verb(argv[1]);
        if (suggestion) {
            std::cerr << " (did you mean '" << suggestion << "'?)";
        }
        std::cerr << "\n";
        std::cerr << "Run `" << argv[0] << " help` to list verbs.\n";
        return 2;
    }

    return run_default_convert(argc, argv);
}

#include "csgify/engine.hpp"
#include "csgify/alea_bridge.hpp"
#include "csgify/alea_void_bridge.hpp"
#include "csgify/autonomy.hpp"
#include "csgify/project_store.hpp"
#include "csgify/project_job_progress.hpp"
#include "csgify/session.hpp"

#include "decision_pack.hpp"
#include "engine_internal.hpp"
#include "engine_model.hpp"
#include "fnv.hpp"
#include "hole_fill.hpp"
#include "lua_runner.hpp"
#include "operation_registry.hpp"
#include "recipe.hpp"
#include "replay.hpp"

#include "viewer.hpp"
#include "viewer_controller.hpp"  // ViewerReviewContext + headless replay (ungated)
#if CSGIFY_HAVE_QT6_VIEWER
#include "viewer_qt.hpp"
#endif

#include <alea.h>
#include <Graphic3d_GraphicDriverFactory.hxx>
#if CSGIFY_HAVE_TKOPENGL
#include <OpenGl_GraphicDriverFactory.hxx>
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <sys/stat.h>
#include <unordered_map>
#include <set>
#include <vector>

namespace {

const char* severity_name(csgify::DiagnosticSeverity severity) {
    switch (severity) {
    case csgify::DiagnosticSeverity::info:
        return "info";
    case csgify::DiagnosticSeverity::warning:
        return "warning";
    case csgify::DiagnosticSeverity::error:
        return "error";
    }
    return "unknown";
}

// Per-verb help table. Drives both the global usage listing and the
// per-verb --help output. Keeping the help as data (vs. a long branchy
// usage() function) means new verbs only need one entry to be wired up,
// and the help text is also visible from `csgify <verb> --help`.
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
     "                            [--progress-json events.jsonl|-]\n"
     "                            [--project project-directory]\n"
     "                            [--units mm|cm|m|in|...] [--scale X]\n"
     "                            [--normalize-units mm|cm|m|in|...]\n"
     "                            [--target solid:N ...] [--no-overlap-check]",
     "Analyze a STEP file; print a human summary or write JSON.",
     "  --report PATH         write JSON report to PATH (`-` for stdout)\n"
     "  --log PATH            open an append-only decision log alongside the run\n"
     "  --project DIR         persist semantic import state and report artifact\n"
     "  --progress-json PATH  stream general analysis progress as JSON Lines\n"
     "  --units NAME          assert the source unit (no geometry rescale)\n"
     "  --scale X             assert the source-unit-to-mm factor (no rescale)\n"
     "  --normalize-units NM  actually rescale geometry to NM (mm|cm|m|in|...)\n"
     "  --no-overlap-check    skip the pairwise solid-overlap analysis\n"
     "                          (default: on; flags interpenetrating solids,\n"
     "                          which MCNP cannot tolerate)\n"
     "  --allow-overlaps      downgrade overlap diagnostics from error to\n"
     "                          warning; emit will proceed despite overlaps\n"
     "                          (use only when you understand the consequences)",
     kReadOnlyMutation},
    {"handoff",
     "handoff input.step --out handoff.json|- [--project project-directory]\n"
     "                            [--report report.json|-]",
     "Build and validate the versioned csgify-to-libalea handoff package.",
     "  --out PATH            write the deterministic handoff manifest\n"
     "  --project DIR         publish the manifest as an immutable project artifact\n"
     "  --report PATH         optionally write the conversion JSON report\n"
     "Input geometry must already satisfy the exact Alea/MCNP cell requirements.",
     kReadOnlyMutation},
    {"voids",
     "voids input.step --out coverage.json|- [--out-mcnp geometry.i]\n"
     "                         [--project project-directory] [--margin F]\n"
     "                         [--domain-id ID] [--bounds xmin,ymin,zmin,xmax,ymax,zmax]\n"
     "                         [--max-depth N] [--min-size X] [--workers N]\n"
     "                         [--scratch-bytes N] [--samples N]\n"
     "                         [--no-merge] [--no-graveyard] [--review]",
     "Generate modeled-domain voids with libalea and certify ownership.",
     "  --out PATH            write the versioned coverage certificate\n"
     "  --out-mcnp PATH       write material, void, and graveyard geometry\n"
     "  --project DIR         checkpoint immutable geometry/certificate artifacts\n"
     "  --margin F            domain diagonal margin fraction (default 0.05)\n"
     "  --domain-id ID        stable modeled-domain identity\n"
     "  --bounds CSV          explicit finite modeled-domain bounds\n"
     "  --max-depth N         libalea adaptive-octree depth (default 8)\n"
     "  --min-size X          minimum octree box size (default libalea behavior)\n"
     "  --workers N           bounded classification workers (0 = runtime maximum)\n"
     "  --scratch-bytes N     parallel scratch budget (default 512 MiB)\n"
     "  --samples N           complete-ownership lattice per axis (default 7)\n"
     "  --no-merge            retain libalea's unmerged regional void cells\n"
     "  --no-graveyard        omit the transport graveyard cells\n"
     "  --review              open the typed Qt coverage panel after generation/reuse\n"
     "Input geometry must already satisfy exact Alea/MCNP cell requirements.\n"
     "A certificate with overlap, gap, undefined-fill, or unresolved evidence blocks success.",
     "Mutating: adds analytical void/graveyard cells to the in-memory Alea model; source STEP is unchanged."},
    {"coverage",
     "coverage status coverage.json\n"
     "coverage review coverage.json\n"
     "coverage queue coverage.json --out queue.json [--source final.step]\n"
     "                                      [--resume previous-queue.json]",
     "Inspect a validated Phase 8 ownership certificate without reading raw JSON.",
     "  status                 print concise domain, ownership, resource, and blocker evidence\n"
     "  review                 open the searchable Qt 6 coverage decision panel\n"
     "  queue                  persist blocking findings in the standard autonomy queue\n"
     "  --source PATH          associate corrective items with the source STEP\n"
     "  --resume PATH          verify resolved/remaining items against a replacement certificate\n"
     "Unsafe findings remain blocking; this command cannot authorize overlap or weaken MCNP ownership.",
     kReadOnlyMutation},
    {"project",
     "project init DIR\n"
     "project status DIR [--limit N]\n"
     "project events DIR --job JOB_ID [--after SEQUENCE] [--limit N]\n"
     "project cancel DIR --job JOB_ID\n"
     "project review [DIR] [--source model.step] [--coverage coverage.json]\n"
     "                   [--queue queue.json]",
     "Inspect or open the resumable large-model project workspace.",
     "  init                   create/migrate a project and conservative project-policy.json\n"
     "  status                 query SQLite read-only; do not load CAD geometry\n"
     "  events                 stream persisted job events as JSON Lines\n"
     "  cancel                 request cancellation at the next safe boundary\n"
     "  review                 open the Qt 6 project workspace\n"
     "  --limit N              maximum rows printed per paginated category\n"
     "  --source PATH          STEP loaded only on explicit geometry review\n"
     "  --coverage PATH        attach a Phase 8 ownership certificate\n"
     "  --queue PATH           attach a durable autonomy queue\n"
     "  --smoke-test           construct, validate attachments, then close (CI)",
     "Status and events are read-only and never initialize a project. Review may\n"
     "persist explicitly approved workspace operations. Cancel changes only the\n"
     "durable job request; geometry remains unchanged until its worker reaches a\n"
     "certified transaction boundary."},
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
     "                                  [--prismatic-max-pieces N] [--prismatic-max-splitter-calls N]\n"
     "                                  [--legacy-global-trials] [--no-parallel-plane-batches]\n"
     "                                  [--max-parallel-families N] [--max-planes-per-batch N]\n"
     "                                  [--max-local-boolean-calls N] [--verify-incremental-state]\n"
     "                                  [--apply-removal-candidates high|medium]\n"
     "                                  [--safe-all] [--max-deviation value] [--dry-run]\n"
     "                                  [--allow-medium-confidence-auto] [--allow-bbox-fallback-auto]\n"
     "                                  [--approx-volume P%]\n"
     "                                  [--allow-overlaps]\n"
     "                                  [--log decisions.json] [--report report.json|-]",
     "Apply safe high-confidence operations and emit a simplified STEP.",
     "  --safe-all              compose bounded auto-fit and conservative BSP\n"
     "  --max-deviation VALUE   enable spline auto-fit with this absolute residual budget\n"
     "  --dry-run               print the plan without mutating; --out-step optional\n"
     "  --apply-removal-candidates LEVEL  one of: high | medium (default: none)\n"
     "  --allow-overlaps        downgrade overlap diagnostics to warning and\n"
     "                            let --safe-all proceed despite pre-existing\n"
     "                            interpenetrations (default: abort if any pair\n"
     "                            is detected)",
     "Mutation contract: may suppress solids selected for removal, may auto-fit\n"
     "  spline faces in place (when --safe-all), and may decompose non-convex\n"
     "  targets. Decomposed solid:N is suppressed and produces piece:1..K\n"
     "  (scoped doc:1/piece:N). Surviving solid:N IDs do NOT shift -- TopExp\n"
     "  order of surviving solids is stable."},
    {"decompose-apply",
     "decompose-apply input.step --out output.i\n"
     "                                  [--fixed-point|--single-pass] [--max-actions N]\n"
     "                                  [--max-active-pieces N] [--max-candidate-evaluations N]\n"
     "                                  [--max-candidates-per-target N]\n"
     "                                  [--max-splitter-calls N] [--max-cumulative-volume-delta X]\n"
     "                                  [--max-action-volume-delta X]\n"
     "                                  [--legacy-global-trials] [--no-parallel-plane-batches]\n"
     "                                  [--max-parallel-families N] [--max-planes-per-batch N]\n"
     "                                  [--max-local-boolean-calls N] [--max-stagnant-target-visits N]\n"
     "                                  [--verify-incremental-state]\n"
     "                                  [--policy auto|medium|low|conservative] [--max-depth N]\n"
     "                                  [--target solid:N] [--max-targets N] [--max-splits N]\n"
     "                                  [--prismatic-max-pieces N] [--prismatic-max-splitter-calls N]\n"
     "                                  [--allow-medium-confidence-auto] [--allow-bbox-fallback-auto]\n"
     "                                  [--log decisions.json] [--progress-json events.jsonl]\n"
     "                                  [--out-step simplified.step] [--report report.json|-]",
     "Run bounded decomposition then emit MCNP.",
     "  --fixed-point           globally improving coordinator (default)\n"
     "  --single-pass           compatibility: legacy per-target decomposition pass\n"
     "  --out-step PATH         STEP output (default: <out-stem>_decomposed.step)\n"
     "  --progress-json PATH    stream bounded fixed-point events as JSON Lines\n"
     "  --no-partial-commit     roll back fixed-point progress unless the result is exact\n"
     "  --legacy-global-trials  qualification oracle: copy and recertify the full model per trial\n"
     "  --no-parallel-plane-batches disable grouped face-plane OCCT candidates\n"
     "  --max-parallel-families N bound grouped support families per target (default: 8)\n"
     "  --max-planes-per-batch N bound planes in one grouped splitter call (default: 64)\n"
     "  --max-local-boolean-calls N bound exact local partition proof calls (default: 2048)\n"
     "  --max-stagnant-target-visits N skip unchanged failed targets after N visits (default: 2)\n"
     "  --max-candidates-per-target N bound OCCT proposals/certificates before yielding (default: 64)\n"
     "  --verify-incremental-state globally reconcile after every accepted local action\n"
     "  --approx-volume P%      resolve small overlaps by deterministic single ownership;\n"
     "                          charge removed volume to this immutable budget\n"
     "  --allow-shatter-fallback enable bounded shatter as a final fixed-point strategy",
     "Mutation contract: decomposed solid:N is suppressed and produces\n"
     "  piece:1..K. Surviving solid:N IDs do NOT shift."},
    {"shatter",
     "shatter input.step --out output.i [--all | --target solid:N ...]\n"
     "                                  [--max-depth N] [--allow-shatter-volume-drift]\n"
     "                                  [--log decisions.json] [--out-step simplified.step] [--report report.json|-]",
     "Exhaustively subdivide non-convex solids (no confidence gate) then emit MCNP.",
     "  --all                   shatter every solid that fails convex validation\n"
     "  --target solid:N        shatter only the named solid(s) (repeatable)\n"
     "  --max-depth N           recursion bound (default 8)\n"
     "  --approx-volume P%      measured cumulative symmetric-difference budget\n"
     "  --allow-containment-change permit measured outward numerical drift\n"
     "  --allow-topology-change permit topology-changing approximation\n"
     "  --allow-shatter-volume-drift  deprecated legacy unmeasured override\n\n"
     "Mutation contract: shattered solid:N is suppressed and produces\n"
     "  piece:1..K. Surviving solid:N IDs do NOT shift."},
    {"compact",
     "compact decisions.json --out recipe.lua [--preserve-step-exports]",
     "Compact a decision log into a deterministic Lua recipe.",
     "  --preserve-step-exports retain the latest recorded STEP export and path",
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
     "    --auto-decompose-bsp            --shatter\n"
     "    --fill-hole\n"
     "    --apply-decomposition-candidate --candidate candidate:N\n"
     "    --decompose-by-face --by-face face:N.M\n"
     "    --resolve-overlap --owner solid:M\n"
     "    --remove-solid\n"
     "  Face actions (target face:N.M, comma list allowed):\n"
     "    --replace-face-with-plane       --replace-face-with-cylinder\n"
     "    --replace-face-with-sphere      --replace-face-with-cone\n"
     "    --apply-surface-candidate --candidate candidate:N\n"
     "  Options:\n"
     "    --max-abs-delta value          (solid replacement volume guard)\n"
     "    --max-rel-delta value          (solid replacement volume guard)\n"
     "    --approx-volume P%             symmetric-difference volume budget\n"
     "    --approx-body-volume P%        cumulative per-source override\n"
     "    --approx-action-volume P%      per-action override\n"
     "    --approx-added-volume P%       added-volume override\n"
     "    --approx-removed-volume P%     removed-volume override\n"
     "    --approx-preset NAME           conservative(0.1), moderate(1), permissive(2) percent\n"
     "    --protect shape-id             forbid approximation of target (repeatable)\n"
     "    --protect-material ID          forbid approximation of material (repeatable)\n"
     "    --allow-containment-change     permit measured added volume\n"
     "    --allow-topology-change        permit solid/shell count changes\n"
     "    --owner shape-id               component retaining overlap ownership\n"
     "    --mcnp-domain solid:N          non-emitting modeled-domain container\n"
     "    --require-domain-coverage      block unless domain coverage certifies\n"
     "    --max-deviation value          (face replacement distance guard)\n"
     "    --max-depth value              (safe BSP / shatter recursion bound)\n"
     "    --allow-bbox-fallback-auto\n"
     "    --allow-medium-confidence-auto\n"
     "    --allow-shatter-volume-drift   (let shatter proceed past tolerance)\n"
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
     "auto-fit input.step --out output.i --max-deviation X [--report report.json|-]",
     "Replace every spline face whose best fit meets --max-deviation.",
     "",
     "Mutation contract: replaces spline face geometry in place. Solid:N IDs\n"
     "  are not affected; only face contents change."},
    {"lua",
     "lua script.lua [args...]",
     "Run a Lua driver script against the csgify session API.", "",
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
     "  --out-step simplified.step     as-applied geometry for further passes\n"
     "  --report PATH                  (`-` for stdout)",
     "Same as the simplify --apply-*-candidate operation it forwards to."},
    {"autonomy",
     "autonomy input.step (--actions actions.json|--actions-json JSON) [--mode explain|propose|auto]\n"
     "                         [--policy-file project-policy.json]\n"
     "                         [--queue-report queue.json|-] [--project project-directory]\n"
     "                         [--log decisions.json]\n"
     "                         [--out output.i] [--out-step reviewed.step] [policy options]",
     "Evaluate structured LLM actions through deterministic geometry policy gates.",
     "  --actions PATH                    csgify.autonomy-actions JSON request\n"
     "  --actions-json JSON               inline request; avoids temporary files\n"
     "  --resume PATH                     restore applied state and continue a saved queue\n"
     "  --mode explain|propose|auto       default: propose\n"
     "  --policy-file PATH                shared versioned preparation/authority policy\n"
     "  --units NAME                      assert source units without rescaling\n"
     "  --scale X                         assert source scale to millimetres\n"
     "  --normalize-units NAME            explicitly rescale imported geometry\n"
     "  --no-heal                         disable import-time ShapeFix preparation\n"
     "  --max-relative-delta X            auto-apply volume budget\n"
     "  --allow-certified-intermediate X auto-apply bounded progress splits up to delta X\n"
     "  --max-intermediate-pieces N       per-split piece limit (default: 64)\n"
     "  --max-intermediate-residuals N    residual-child limit (default: 4)\n"
     "  --min-piece-fraction X            reject autonomous slivers\n"
     "  --allow-removal FRACTION          opt in to tiny high-confidence removals\n"
     "  --allow-contained-medium ID:PHASE:DENSITY\n"
     "                                    authorize an exact configured fluid material\n"
     "  --allow-approximate-surface DEV   opt in with maximum deviation\n"
     "  --approx-volume P%                immutable symmetric-difference budget\n"
     "  --approx-shatter                  permit measured numerical shatter loss\n"
     "  --approx-remove-residuals         permit measured piece suppression\n"
     "  --approx-resolve-overlaps         permit measured single-owner overlap repair\n"
     "  --allow-containment-change        permit measured added volume\n"
     "  --allow-topology-change           permit topology-changing approximation\n"
     "  --max-auto-actions N              queue mutation budget (default: 32)\n"
     "  --queue-report PATH|-             required audit/evidence output (default: -)\n"
     "  --project DIR                     discover policy; reserve/recover requests\n"
     "  --log PATH                        replayable mutation log with actor/policy audit\n"
     "  --review                          open the persistent review queue in the Qt viewer\n"
     "Exit 0: completed; 2: human review required; 1: denied or failed.",
     "Only auto-approved actions execute, each through the atomic "
     "Session API. Human-review and denied items never mutate geometry."},
    {"context",
     "context input.step --out context.json|- [--target solid:N|face:N.M]\n"
     "                       [--history decisions.json] [--policy-file project-policy.json]\n"
     "                       [--project project-directory] [policy options]",
     "Build a compact, versioned reasoning context for an external LLM.",
     "  --out PATH|-                       csgify.llm-context JSON\n"
     "  --target ID                        include fresh candidates for one target\n"
     "  --history PATH                     include a bounded decision-history tail\n"
     "  --candidate-offset N               start each bounded candidate page at N\n"
     "  --max-candidates N                 candidates per family (default 32)\n"
     "  --solid-offset N                   start the bounded solid page at N\n"
     "  --max-solids N                     solids per page (default 32)\n"
     "  --occurrence-offset N              start the same-definition occurrence page at N\n"
     "  --max-occurrences N                occurrences per page (default 32)\n"
     "  --overlap-offset N                 start the bounded local-overlap page at N\n"
     "  --max-local-overlaps N             local overlaps per page (default 32)\n"
     "  --max-faces-per-solid N            face IDs per solid (default 32)\n"
     "  --max-materials N                  material definitions in context (default 32)\n"
     "  --max-interfaces N                 selected-owner interfaces (default 32)\n"
     "  --max-context-bytes N              hard JSON size ceiling (default 1048576)\n"
     "  --policy-file PATH                 use shared preparation/authority policy\n"
     "  --project DIR                      discover DIR/project-policy.json\n"
     "  --max-relative-delta X             advertise autonomous volume budget\n"
     "  --allow-certified-intermediate X  advertise bounded progress-split policy\n"
     "  --allow-removal FRACTION           advertise removal policy\n"
     "  --allow-contained-medium ID:PHASE:DENSITY\n"
     "                                     advertise an authorized fluid material\n"
     "  --allow-approximate-surface DEV    advertise surface deviation policy\n"
     "  --approx-volume P%                 advertise immutable approximation budget\n"
     "  --approx-shatter                   advertise budgeted shatter permission\n"
     "  --approx-remove-residuals          advertise residual-removal permission\n"
     "  --approx-resolve-overlaps          advertise overlap-ownership permission\n"
     "  --allow-containment-change         advertise containment permission\n"
     "  --allow-topology-change            advertise topology permission",
     kReadOnlyMutation},
    {"queue",
     "queue status queue.json [--out queue.json|-]\n"
     "queue reject queue.json ITEM_ID --rationale TEXT [--out queue.json] [--log decisions.json]",
     "Inspect or persist a human rejection in an autonomy queue.",
     "  status                  validate and print the durable queue\n"
     "  reject                  mark one unresolved item rejected\n"
     "  --rationale TEXT        required rejection rationale\n"
     "  --log PATH              append a non-mutating review audit record",
     "Reject changes queue state and optionally appends an audit record; it never mutates CAD."},
    {"viewer-check",
     "viewer-check",
     "Probe whether the OCCT viewer can run in this environment.", "",
     kReadOnlyMutation},
    {"view",
     "view input.step [--select doc:N/solid:M|solid:M]\n"
     "                         [--candidate candidate:N|best]\n"
     "                         [--review-removal removal:N]\n"
     "                         [--review-removals  (Qt component checklist and revision-bound batch confirmation)]\n"
     "                         [--review-hole-fill hole-fill:N [--host solid:N] [--near-removed solid:M]]\n"
     "                         [--review-decomp candidate:N|best --select solid:N]\n"
     "                         [--review-surface-replacement candidate:N --select-face face:N.M]\n"
     "                         [--log decisions.json] [--out output.i]\n"
     "                         [--viewer-backend auto|qt|x11] [--keys A,Tab,A,Q]",
     "Open the OCCT viewer for visual review and live editing.",
     "  --log decisions.json           record accepted decisions (replayable)\n"
     "  --out output.i                 emit the MCNP deck on quit\n"
     "  --viewer-backend auto|qt|x11   choose UI backend (default: auto; Qt preferred)\n"
     "  --keys A,Tab,A,Q               headless: replay keys, no window/display\n"
     "                                 (A=accept active, X=reject, Tab/BackTab=cycle,\n"
     "                                  R:solid:N=remove, Q=quit+commit)",
     "Qt mutations use explicit controls and confirmation; Ctrl+Enter accepts the\n"
     "current non-removal proposal. Headless --keys A/R drives the session in place.\n"
     "With --log each decision is recorded; compact it "
     "into a recipe to replay headless."},
    {"help",
     "help [verb]",
     "Show global usage, or the help for a specific verb.", "", ""},
    {"--version",
     "--version",
     "Print the csgify version, OCCT version, and Alea version.", "", ""},
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

int write_report_or_stdout(const csgify::ConversionReport& report,
                           const std::string& path) {
    if (is_stdout_path(path)) {
        std::cout << csgify::report_json_string(report);
        return 0;
    }
    return csgify::write_report_json(report, path);
}

int write_decomp_proposal_or_stdout(const csgify::DecompositionProposal& p,
                                    const std::string& path) {
    if (is_stdout_path(path)) {
        std::cout << csgify::decomposition_proposal_json_string(p);
        return 0;
    }
    return csgify::write_decomposition_proposal_json(p, path);
}

int write_removal_proposal_or_stdout(const csgify::RemovalProposal& p,
                                     const std::string& path) {
    if (is_stdout_path(path)) {
        std::cout << csgify::removal_proposal_json_string(p);
        return 0;
    }
    return csgify::write_removal_proposal_json(p, path);
}

int write_hole_fill_proposal_or_stdout(const csgify::HoleFillProposal& p,
                                       const std::string& path) {
    if (is_stdout_path(path)) {
        std::cout << csgify::hole_fill_proposal_json_string(p);
        return 0;
    }
    return csgify::write_hole_fill_proposal_json(p, path);
}

int write_surface_proposal_or_stdout(
    const csgify::SurfaceReplacementProposal& p,
    const std::string& path) {
    if (is_stdout_path(path)) {
        std::cout << csgify::surface_replacement_proposal_json_string(p);
        return 0;
    }
    return csgify::write_surface_replacement_proposal_json(p, path);
}

// Compact human summary for `inspect` without --report. The JSON report
// is canonical and exposes every field; this is the at-a-glance view
// for a user who just typed `csgify inspect file.step`.
std::string solid_status_label(const csgify::AnalyzedSolid& solid);

void print_human_inspect(std::ostream& out,
                         const csgify::ConversionReport& report) {
    std::size_t ready = 0;
    for (const csgify::AnalyzedSolid& s : report.solids) {
        if (s.exact_csg_supported) ++ready;
    }
    out << "csgify inspect";
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
    if (!report.overlaps.empty()) {
        out << "  overlaps: " << report.overlaps.size()
            << " interpenetrating pair(s) -- MCNP cells must be disjoint\n";
    }
    if (!report.solids.empty()) {
        out << "\nPer-solid status:\n";
        for (const csgify::AnalyzedSolid& s : report.solids) {
            out << "  " << s.id << "  " << solid_status_label(s)
                << "  vol=" << s.volume;
            if (!s.name.empty()) out << "  " << s.name;
            out << "\n";
        }
    }
    if (!report.diagnostics.empty()) {
        out << "\nDiagnostics:\n";
        for (const csgify::Diagnostic& d : report.diagnostics) {
            out << "  " << severity_name(d.severity) << " " << d.code;
            if (!d.shape_label.empty()) out << " " << d.shape_label;
            out << ": " << d.message << "\n";
        }
    }
}

void print_summary(const csgify::ConversionReport& report) {
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

void print_diagnostics(const csgify::ConversionReport& report) {
    for (const csgify::Diagnostic& diagnostic : report.diagnostics) {
        std::cerr << severity_name(diagnostic.severity) << " "
                  << diagnostic.code;
        if (!diagnostic.shape_label.empty()) {
            std::cerr << " " << diagnostic.shape_label;
        }
        std::cerr << ": " << diagnostic.message << '\n';
    }
}

bool operation_from_action(const std::string& action,
                           csgify::ReplacementOperation* operation) {
    const csgify::OperationDescriptor* descriptor =
        csgify::find_replacement_by_cli_flag(action);
    if (!descriptor) return false;
    *operation = descriptor->replacement_operation;
    return true;
}

struct FaceActionEntry {
    const char* flag;
    csgify::SplineFitKind kind;
};

constexpr FaceActionEntry kFaceActions[] = {
    {"--replace-face-with-plane",    csgify::SplineFitKind::plane},
    {"--replace-face-with-sphere",   csgify::SplineFitKind::sphere},
    {"--replace-face-with-cylinder", csgify::SplineFitKind::cylinder},
    {"--replace-face-with-cone",     csgify::SplineFitKind::cone},
};

bool face_kind_from_action(const std::string& action,
                           csgify::SplineFitKind* kind) {
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

bool set_uniform_approximation_budget(const std::string& text,
                                      csgify::ApproximationPolicy* policy,
                                      std::string* error_out) {
    double fraction = 0.0;
    if (!csgify::parse_approximation_percent(text, &fraction, error_out))
        return false;
    policy->enabled = true;
    policy->max_global_changed_volume_fraction = fraction;
    policy->max_body_changed_volume_fraction = fraction;
    policy->max_action_changed_volume_fraction = fraction;
    policy->max_global_added_volume_fraction = fraction;
    policy->max_global_removed_volume_fraction = fraction;
    policy->max_body_added_volume_fraction = fraction;
    policy->max_body_removed_volume_fraction = fraction;
    return true;
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
    csgify::ConversionOptions options;
    csgify::SessionPtr session = csgify::create_session(options);
    if (compare_path.empty()) {
        const csgify::DocumentId document_id = csgify::load_step(*session, input);
        csgify::CommandResult analysis =
            csgify::analyze(*session, document_id, csgify::ReportIdMode::local);
        const csgify::ConversionReport& report = analysis.report;
        std::cout << "volume " << report.total_volume << '\n';
        std::cout << "solids " << report.solid_volumes.size() << '\n';
        for (const csgify::SolidVolume& solid : report.solid_volumes) {
            std::cout << solid.shape_label << " " << solid.volume << '\n';
        }
        return report.solids_seen > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    const csgify::DocumentId original_id = csgify::load_step(*session, input);
    const csgify::DocumentId candidate_id = csgify::load_step(*session, compare_path);
    csgify::VolumeComparison comparison =
        csgify::compare_document_volumes(*session,
                                          original_id,
                                          candidate_id,
                                          abs_tol,
                                          rel_tol,
                                          csgify::ReportIdMode::local);
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

int run_queue_command(int argc, char** argv) {
    if (wants_help(argc, argv)) return print_verb_help_and_exit(argv[0], "queue");
    if (argc < 4) {
        std::cerr << "error queue_usage: expected status|reject queue.json\n";
        return EXIT_FAILURE;
    }
    const std::string command = argv[2];
    const std::string queue_path = argv[3];
    std::string item_id;
    int option_start = 4;
    if (command == "reject") {
        if (argc < 5) {
            std::cerr << "error queue_item_required\n";
            return EXIT_FAILURE;
        }
        item_id = argv[4];
        option_start = 5;
    } else if (command != "status") {
        std::cerr << "error invalid_queue_command: " << command << '\n';
        return EXIT_FAILURE;
    }
    std::string output_path = command == "reject" ? queue_path : "-";
    std::string rationale;
    std::string log_path;
    for (int i = option_start; i < argc; ++i) {
        if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc)
            output_path = argv[++i];
        else if (std::strcmp(argv[i], "--rationale") == 0 && i + 1 < argc)
            rationale = argv[++i];
        else if (std::strcmp(argv[i], "--log") == 0 && i + 1 < argc)
            log_path = argv[++i];
        else {
            std::cerr << "error unrecognized_option: " << argv[i] << '\n';
            return EXIT_FAILURE;
        }
    }
    csgify::AutonomyQueue queue;
    try {
        queue = csgify::read_autonomy_queue_json(queue_path);
    } catch (const std::exception& e) {
        std::cerr << "error queue_invalid: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
    if (command == "reject") {
        if (rationale.empty()) {
            std::cerr << "error queue_rejection_rationale_required\n";
            return EXIT_FAILURE;
        }
        csgify::AutonomyQueueItem* selected = nullptr;
        for (csgify::AutonomyQueueItem& item : queue.items)
            if (item.proposal.id == item_id) { selected = &item; break; }
        if (!selected || !csgify::reject_autonomy_queue_item(
                queue, item_id, rationale)) {
            std::cerr << "error queue_item_not_rejectable: " << item_id << '\n';
            return EXIT_FAILURE;
        }
        if (!log_path.empty()) {
            try {
                csgify::SessionPtr session = csgify::create_session();
                csgify::attach_decision_log(
                    *session, log_path, queue.source_path);
                csgify::load_step(*session, queue.source_path);
                csgify::DecisionAuditContext audit;
                audit.actor = "human";
                audit.proposal_actor = selected->proposal.actor;
                audit.proposal_id = selected->proposal.id;
                audit.rationale = rationale;
                audit.policy_rule = selected->policy_rule;
                audit.approval_mode = "manual";
                const csgify::OperationDescriptor* descriptor =
                    csgify::find_operation(selected->proposal.op);
                csgify::record_decision_review(
                    *session, selected->proposal.target_id,
                    selected->proposal.id, "rejected", audit,
                    descriptor ? descriptor->category : std::string(),
                    selected->proposal.op, selected->proposal.candidate_id,
                    selected->evaluated_revision);
            } catch (const std::exception& e) {
                std::cerr << "error queue_review_audit_failed: "
                          << e.what() << '\n';
                return EXIT_FAILURE;
            }
        }
    }
    if (csgify::write_autonomy_queue_json(queue, output_path) != 0) {
        std::cerr << "error queue_write_failed: " << output_path << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

int run_context_command(int argc, char** argv) {
    if (wants_help(argc, argv)) return print_verb_help_and_exit(argv[0], "context");
    if (argc < 3) {
        std::cerr << "error context_input_required\n";
        return EXIT_FAILURE;
    }
    const std::string input_path = argv[2];
    std::string output_path;
    std::string policy_path;
    std::string project_path;
    csgify::AutonomyContextOptions context_options;
    csgify::AutonomyPolicy policy;
    for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            output_path = argv[++i];
        } else if (std::strcmp(argv[i], "--policy-file") == 0
                   && i + 1 < argc) {
            policy_path = argv[++i];
        } else if (std::strcmp(argv[i], "--project") == 0
                   && i + 1 < argc) {
            project_path = argv[++i];
        } else if (std::strcmp(argv[i], "--target") == 0 && i + 1 < argc) {
            context_options.target_id = argv[++i];
        } else if (std::strcmp(argv[i], "--history") == 0 && i + 1 < argc) {
            context_options.history_path = argv[++i];
        } else if (std::strcmp(argv[i], "--candidate-offset") == 0
                   && i + 1 < argc) {
            context_options.candidate_offset =
                parse_size_arg(argv[++i], "--candidate-offset");
        } else if (std::strcmp(argv[i], "--max-candidates") == 0
                   && i + 1 < argc) {
            context_options.max_candidates =
                parse_size_arg(argv[++i], "--max-candidates");
        } else if (std::strcmp(argv[i], "--solid-offset") == 0
                   && i + 1 < argc) {
            context_options.solid_offset =
                parse_size_arg(argv[++i], "--solid-offset");
        } else if (std::strcmp(argv[i], "--max-solids") == 0
                   && i + 1 < argc) {
            context_options.max_solids =
                parse_size_arg(argv[++i], "--max-solids");
        } else if (std::strcmp(argv[i], "--occurrence-offset") == 0
                   && i + 1 < argc) {
            context_options.occurrence_offset =
                parse_size_arg(argv[++i], "--occurrence-offset");
        } else if (std::strcmp(argv[i], "--max-occurrences") == 0
                   && i + 1 < argc) {
            context_options.max_occurrences =
                parse_size_arg(argv[++i], "--max-occurrences");
        } else if (std::strcmp(argv[i], "--overlap-offset") == 0
                   && i + 1 < argc) {
            context_options.overlap_offset =
                parse_size_arg(argv[++i], "--overlap-offset");
        } else if (std::strcmp(argv[i], "--max-local-overlaps") == 0
                   && i + 1 < argc) {
            context_options.max_local_overlaps =
                parse_size_arg(argv[++i], "--max-local-overlaps");
        } else if (std::strcmp(argv[i], "--max-faces-per-solid") == 0
                   && i + 1 < argc) {
            context_options.max_faces_per_solid =
                parse_size_arg(argv[++i], "--max-faces-per-solid");
        } else if (std::strcmp(argv[i], "--max-materials") == 0
                   && i + 1 < argc) {
            context_options.max_materials =
                parse_size_arg(argv[++i], "--max-materials");
        } else if (std::strcmp(argv[i], "--max-interfaces") == 0
                   && i + 1 < argc) {
            context_options.max_interfaces =
                parse_size_arg(argv[++i], "--max-interfaces");
        } else if (std::strcmp(argv[i], "--max-context-bytes") == 0
                   && i + 1 < argc) {
            context_options.max_serialized_bytes =
                parse_size_arg(argv[++i], "--max-context-bytes");
        } else if (std::strcmp(argv[i], "--max-relative-delta") == 0
                   && i + 1 < argc) {
            policy.max_relative_volume_delta =
                parse_double_arg(argv[++i], "--max-relative-delta");
        } else if (std::strcmp(argv[i], "--allow-certified-intermediate") == 0
                   && i + 1 < argc) {
            policy.allow_certified_intermediate_decomposition = true;
            policy.max_intermediate_relative_volume_delta =
                parse_double_arg(argv[++i], "--allow-certified-intermediate");
        } else if (std::strcmp(argv[i], "--allow-removal") == 0
                   && i + 1 < argc) {
            policy.allow_removal = true;
            policy.max_removal_volume_fraction =
                parse_double_arg(argv[++i], "--allow-removal");
        } else if (std::strcmp(argv[i], "--allow-contained-medium") == 0
                   && i + 1 < argc) {
            const std::string specification = argv[++i];
            const std::size_t density_separator = specification.rfind(':');
            const std::size_t phase_separator = density_separator == std::string::npos
                ? std::string::npos
                : specification.rfind(':', density_separator - 1);
            if (phase_separator == std::string::npos
                || density_separator == std::string::npos
                || phase_separator == 0
                || density_separator <= phase_separator + 1) {
                std::cerr << "error invalid_contained_medium_material: expected ID:PHASE:DENSITY\n";
                return EXIT_FAILURE;
            }
            csgify::MaterialDefinition material;
            material.id = specification.substr(0, phase_separator);
            const std::string phase = specification.substr(
                phase_separator + 1,
                density_separator - phase_separator - 1);
            material.phase = phase == "liquid" ? csgify::MaterialPhase::liquid
                : phase == "gas" ? csgify::MaterialPhase::gas
                                  : csgify::MaterialPhase::unknown;
            material.density = parse_double_arg(
                specification.substr(density_separator + 1).c_str(),
                "--allow-contained-medium density");
            if (material.id.empty()
                || material.phase == csgify::MaterialPhase::unknown
                || !std::isfinite(material.density)) {
                std::cerr << "error invalid_contained_medium_material: phase must be liquid or gas and density finite\n";
                return EXIT_FAILURE;
            }
            policy.allow_contained_medium_generation = true;
            policy.contained_medium_materials.push_back(std::move(material));
        } else if (std::strcmp(argv[i], "--allow-approximate-surface") == 0
                   && i + 1 < argc) {
            policy.allow_approximate_surface_replacement = true;
            policy.max_surface_deviation =
                parse_double_arg(argv[++i], "--allow-approximate-surface");
            policy.approximation.allow_surface_replacement = true;
            policy.approximation.max_surface_deviation =
                policy.max_surface_deviation;
        } else if (std::strcmp(argv[i], "--approx-volume") == 0
                   && i + 1 < argc) {
            std::string error;
            if (!set_uniform_approximation_budget(
                    argv[++i], &policy.approximation, &error)) {
                std::cerr << "error invalid_approximation_percent: "
                          << error << '\n';
                return EXIT_FAILURE;
            }
        } else if (std::strcmp(argv[i], "--approx-shatter") == 0) {
            policy.approximation.allow_numerical_shatter_loss = true;
        } else if (std::strcmp(argv[i], "--approx-remove-residuals") == 0) {
            policy.approximation.allow_sliver_removal = true;
        } else if (std::strcmp(argv[i], "--approx-resolve-overlaps") == 0) {
            policy.approximation.allow_overlap_resolution = true;
        } else if (std::strcmp(argv[i], "--allow-containment-change") == 0) {
            policy.approximation.require_containment_preservation = false;
        } else if (std::strcmp(argv[i], "--allow-topology-change") == 0) {
            policy.approximation.require_topology_preservation = false;
        } else {
            std::cerr << "error unrecognized_option: " << argv[i] << '\n';
            return EXIT_FAILURE;
        }
    }
    if (output_path.empty()) {
        std::cerr << "error context_output_required: use --out context.json|-\n";
        return EXIT_FAILURE;
    }
    if (policy_path.empty() && !project_path.empty()) {
        const std::filesystem::path discovered =
            std::filesystem::path(project_path) / "project-policy.json";
        std::error_code error;
        if (!std::filesystem::is_regular_file(discovered, error)) {
            std::cerr << "error context_project_policy_missing: "
                      << discovered.string()
                      << "; run `csgify project init " << project_path << "`\n";
            return EXIT_FAILURE;
        }
        policy_path = discovered.string();
    }
    csgify::AutonomyPreparationPolicy preparation;
    if (!policy_path.empty()) {
        const auto is_inline_policy_option = [](const char* option) {
            return std::strcmp(option, "--max-relative-delta") == 0
                || std::strcmp(option, "--allow-certified-intermediate") == 0
                || std::strcmp(option, "--allow-removal") == 0
                || std::strcmp(option, "--allow-contained-medium") == 0
                || std::strcmp(option, "--allow-approximate-surface") == 0
                || std::strcmp(option, "--approx-volume") == 0
                || std::strcmp(option, "--approx-shatter") == 0
                || std::strcmp(option, "--approx-remove-residuals") == 0
                || std::strcmp(option, "--approx-resolve-overlaps") == 0
                || std::strcmp(option, "--allow-containment-change") == 0
                || std::strcmp(option, "--allow-topology-change") == 0;
        };
        for (int i = 3; i < argc; ++i) {
            if (is_inline_policy_option(argv[i])) {
                std::cerr << "error context_policy_conflict: --policy-file cannot be combined with inline policy options\n";
                return EXIT_FAILURE;
            }
        }
        try {
            const csgify::AutonomyProjectPolicy project_policy =
                csgify::read_autonomy_project_policy_json(policy_path);
            context_options.project_id = project_policy.project_id;
            context_options.intended_analysis =
                project_policy.intended_analysis;
            preparation = project_policy.preparation;
            context_options.preparation = preparation;
            policy = project_policy.authority;
        } catch (const std::exception& e) {
            std::cerr << "error context_policy_load_failed: " << e.what()
                      << '\n';
            return EXIT_FAILURE;
        }
    }
    csgify::ConversionOptions context_conversion_options;
    context_conversion_options.heal_shape = preparation.heal_shape;
    context_conversion_options.unit_override = preparation.unit_override;
    context_conversion_options.unit_scale_to_mm_override =
        preparation.unit_scale_to_mm_override;
    context_conversion_options.normalize_to_unit =
        preparation.normalize_to_unit;
    context_conversion_options.approximation = policy.approximation;
    csgify::SessionPtr session =
        csgify::create_session(context_conversion_options);
    csgify::DocumentId document;
    try {
        document = csgify::load_step(*session, input_path);
    } catch (const std::exception& e) {
        std::cerr << "error context_load_failed: "
                  << e.what() << '\n';
        return EXIT_FAILURE;
    }
    if (csgify::write_autonomy_context_json(
            *session, document, policy, context_options, output_path) != 0) {
        std::cerr << "error context_write_failed: " << output_path << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

int run_autonomy_command(int argc, char** argv) {
    if (wants_help(argc, argv)) return print_verb_help_and_exit(argv[0], "autonomy");
    if (argc < 3) {
        std::cerr << "error autonomy_input_required\n";
        return EXIT_FAILURE;
    }
    const std::string input_path = argv[2];
    std::string actions_path;
    std::string actions_json;
    std::string resume_path;
    std::string queue_report_path = "-";
    std::string log_path;
    std::string output_path;
    std::string step_output_path;
    std::string project_path;
    std::string policy_path;
    csgify::AutonomyPolicy policy;
    csgify::AutonomyPreparationPolicy preparation;
    bool open_review = false;
    bool mode_was_set = false;
    bool preparation_was_set = false;
    for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "--actions") == 0 && i + 1 < argc) {
            actions_path = argv[++i];
        } else if (std::strcmp(argv[i], "--actions-json") == 0 && i + 1 < argc) {
            actions_json = argv[++i];
        } else if (std::strcmp(argv[i], "--resume") == 0 && i + 1 < argc) {
            resume_path = argv[++i];
        } else if (std::strcmp(argv[i], "--queue-report") == 0 && i + 1 < argc) {
            queue_report_path = argv[++i];
        } else if (std::strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            log_path = argv[++i];
        } else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            output_path = argv[++i];
        } else if (std::strcmp(argv[i], "--out-step") == 0 && i + 1 < argc) {
            step_output_path = argv[++i];
        } else if (std::strcmp(argv[i], "--project") == 0 && i + 1 < argc) {
            project_path = argv[++i];
        } else if (std::strcmp(argv[i], "--policy-file") == 0
                   && i + 1 < argc) {
            policy_path = argv[++i];
        } else if (std::strcmp(argv[i], "--units") == 0 && i + 1 < argc) {
            preparation.unit_override = argv[++i];
            preparation_was_set = true;
        } else if (std::strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            preparation.unit_scale_to_mm_override =
                parse_double_arg(argv[++i], "--scale");
            preparation_was_set = true;
        } else if (std::strcmp(argv[i], "--normalize-units") == 0
                   && i + 1 < argc) {
            preparation.normalize_to_unit = argv[++i];
            preparation_was_set = true;
        } else if (std::strcmp(argv[i], "--no-heal") == 0) {
            preparation.heal_shape = false;
            preparation_was_set = true;
        } else if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            const std::string mode = argv[++i];
            mode_was_set = true;
            if (mode == "explain") policy.mode = csgify::AutonomyMode::explain;
            else if (mode == "propose") policy.mode = csgify::AutonomyMode::propose;
            else if (mode == "auto") policy.mode = csgify::AutonomyMode::automatic;
            else {
                std::cerr << "error invalid_autonomy_mode: " << mode << '\n';
                return EXIT_FAILURE;
            }
        } else if (std::strcmp(argv[i], "--max-relative-delta") == 0
                   && i + 1 < argc) {
            policy.max_relative_volume_delta =
                parse_double_arg(argv[++i], "--max-relative-delta");
        } else if (std::strcmp(argv[i], "--allow-certified-intermediate") == 0
                   && i + 1 < argc) {
            policy.allow_certified_intermediate_decomposition = true;
            policy.max_intermediate_relative_volume_delta =
                parse_double_arg(argv[++i], "--allow-certified-intermediate");
        } else if (std::strcmp(argv[i], "--max-intermediate-pieces") == 0
                   && i + 1 < argc) {
            policy.max_intermediate_pieces =
                parse_size_arg(argv[++i], "--max-intermediate-pieces");
        } else if (std::strcmp(argv[i], "--max-intermediate-residuals") == 0
                   && i + 1 < argc) {
            policy.max_intermediate_residual_pieces =
                parse_size_arg(argv[++i], "--max-intermediate-residuals");
        } else if (std::strcmp(argv[i], "--min-piece-fraction") == 0
                   && i + 1 < argc) {
            policy.min_piece_volume_fraction =
                parse_double_arg(argv[++i], "--min-piece-fraction");
        } else if (std::strcmp(argv[i], "--allow-removal") == 0
                   && i + 1 < argc) {
            policy.allow_removal = true;
            policy.max_removal_volume_fraction =
                parse_double_arg(argv[++i], "--allow-removal");
        } else if (std::strcmp(argv[i], "--allow-contained-medium") == 0
                   && i + 1 < argc) {
            const std::string specification = argv[++i];
            const std::size_t density_separator = specification.rfind(':');
            const std::size_t phase_separator = density_separator == std::string::npos
                ? std::string::npos
                : specification.rfind(':', density_separator - 1);
            if (phase_separator == std::string::npos
                || density_separator == std::string::npos
                || phase_separator == 0
                || density_separator <= phase_separator + 1) {
                std::cerr << "error invalid_contained_medium_material: expected ID:PHASE:DENSITY\n";
                return EXIT_FAILURE;
            }
            csgify::MaterialDefinition material;
            material.id = specification.substr(0, phase_separator);
            const std::string phase = specification.substr(
                phase_separator + 1,
                density_separator - phase_separator - 1);
            material.phase = phase == "liquid" ? csgify::MaterialPhase::liquid
                : phase == "gas" ? csgify::MaterialPhase::gas
                                  : csgify::MaterialPhase::unknown;
            material.density = parse_double_arg(
                specification.substr(density_separator + 1).c_str(),
                "--allow-contained-medium density");
            if (material.id.empty()
                || material.phase == csgify::MaterialPhase::unknown
                || !std::isfinite(material.density)) {
                std::cerr << "error invalid_contained_medium_material: phase must be liquid or gas and density finite\n";
                return EXIT_FAILURE;
            }
            policy.allow_contained_medium_generation = true;
            policy.contained_medium_materials.push_back(std::move(material));
        } else if (std::strcmp(argv[i], "--allow-approximate-surface") == 0
                   && i + 1 < argc) {
            policy.allow_approximate_surface_replacement = true;
            policy.max_surface_deviation =
                parse_double_arg(argv[++i], "--allow-approximate-surface");
            policy.approximation.allow_surface_replacement = true;
            policy.approximation.max_surface_deviation =
                policy.max_surface_deviation;
        } else if (std::strcmp(argv[i], "--max-auto-actions") == 0
                   && i + 1 < argc) {
            policy.max_auto_actions =
                parse_size_arg(argv[++i], "--max-auto-actions");
        } else if (std::strcmp(argv[i], "--approx-volume") == 0
                   && i + 1 < argc) {
            std::string error;
            if (!set_uniform_approximation_budget(
                    argv[++i], &policy.approximation, &error)) {
                std::cerr << "error invalid_approximation_percent: "
                          << error << '\n';
                return EXIT_FAILURE;
            }
        } else if (std::strcmp(argv[i], "--approx-shatter") == 0) {
            policy.approximation.allow_numerical_shatter_loss = true;
        } else if (std::strcmp(argv[i], "--approx-remove-residuals") == 0) {
            policy.approximation.allow_sliver_removal = true;
        } else if (std::strcmp(argv[i], "--approx-resolve-overlaps") == 0) {
            policy.approximation.allow_overlap_resolution = true;
        } else if (std::strcmp(argv[i], "--allow-containment-change") == 0) {
            policy.approximation.require_containment_preservation = false;
        } else if (std::strcmp(argv[i], "--allow-topology-change") == 0) {
            policy.approximation.require_topology_preservation = false;
        } else if (std::strcmp(argv[i], "--review") == 0) {
            open_review = true;
        } else {
            std::cerr << "error unrecognized_option: " << argv[i] << '\n';
            return EXIT_FAILURE;
        }
    }
    if (!project_path.empty()) {
        const std::filesystem::path project_root(project_path);
        const std::filesystem::path database = project_root / "project.sqlite";
        const std::filesystem::path discovered =
            project_root / "project-policy.json";
        std::error_code error;
        if (!std::filesystem::is_regular_file(database, error)
            || !std::filesystem::is_regular_file(discovered, error)) {
            std::cerr << "error autonomy_project_not_initialized: run `csgify project init "
                      << project_path << "`\n";
            return EXIT_FAILURE;
        }
        if (policy_path.empty() && resume_path.empty())
            policy_path = discovered.string();
    }
    if (!policy_path.empty()) {
        const auto is_inline_authority_option = [](const char* option) {
            return std::strcmp(option, "--max-relative-delta") == 0
                || std::strcmp(option, "--allow-certified-intermediate") == 0
                || std::strcmp(option, "--max-intermediate-pieces") == 0
                || std::strcmp(option, "--max-intermediate-residuals") == 0
                || std::strcmp(option, "--min-piece-fraction") == 0
                || std::strcmp(option, "--allow-removal") == 0
                || std::strcmp(option, "--allow-contained-medium") == 0
                || std::strcmp(option, "--allow-approximate-surface") == 0
                || std::strcmp(option, "--max-auto-actions") == 0
                || std::strcmp(option, "--approx-volume") == 0
                || std::strcmp(option, "--approx-shatter") == 0
                || std::strcmp(option, "--approx-remove-residuals") == 0
                || std::strcmp(option, "--approx-resolve-overlaps") == 0
                || std::strcmp(option, "--allow-containment-change") == 0
                || std::strcmp(option, "--allow-topology-change") == 0;
        };
        for (int i = 3; i < argc; ++i) {
            if (is_inline_authority_option(argv[i])
                || std::strcmp(argv[i], "--units") == 0
                || std::strcmp(argv[i], "--scale") == 0
                || std::strcmp(argv[i], "--normalize-units") == 0
                || std::strcmp(argv[i], "--no-heal") == 0) {
                std::cerr << "error autonomy_policy_conflict: --policy-file cannot be combined with inline preparation or policy options\n";
                return EXIT_FAILURE;
            }
        }
        if (!resume_path.empty()) {
            std::cerr << "error autonomy_policy_conflict: a resumed queue already contains its immutable policy\n";
            return EXIT_FAILURE;
        }
        const csgify::AutonomyMode requested_mode = policy.mode;
        try {
            const csgify::AutonomyProjectPolicy project_policy =
                csgify::read_autonomy_project_policy_json(policy_path);
            preparation = project_policy.preparation;
            policy = project_policy.authority;
            if (mode_was_set) policy.mode = requested_mode;
        } catch (const std::exception& e) {
            std::cerr << "error autonomy_policy_load_failed: " << e.what()
                      << '\n';
            return EXIT_FAILURE;
        }
    }
    const int source_count = (actions_path.empty() ? 0 : 1)
        + (actions_json.empty() ? 0 : 1)
        + (resume_path.empty() ? 0 : 1);
    if (source_count != 1) {
        std::cerr << "error autonomy_input_conflict: use exactly one of "
                     "--actions, --actions-json, or --resume\n";
        return EXIT_FAILURE;
    }
    if (policy.max_relative_volume_delta < 0.0
        || policy.max_intermediate_relative_volume_delta < 0.0
        || policy.min_piece_volume_fraction < 0.0
        || policy.max_removal_volume_fraction < 0.0
        || policy.max_surface_deviation < 0.0) {
        std::cerr << "error invalid_autonomy_policy: tolerances must be non-negative\n";
        return EXIT_FAILURE;
    }
    if (!std::isfinite(preparation.unit_scale_to_mm_override)
        || preparation.unit_scale_to_mm_override < 0.0) {
        std::cerr << "error invalid_autonomy_preparation: scale must be finite and non-negative\n";
        return EXIT_FAILURE;
    }

    const auto conversion_options_for = [](
        const csgify::AutonomyPreparationPolicy& prep,
        const csgify::AutonomyPolicy& authority) {
        csgify::ConversionOptions options;
        options.heal_shape = prep.heal_shape;
        options.unit_override = prep.unit_override;
        options.unit_scale_to_mm_override = prep.unit_scale_to_mm_override;
        options.normalize_to_unit = prep.normalize_to_unit;
        options.approximation = authority.approximation;
        return options;
    };

    csgify::AutonomyRequest request;
    csgify::AutonomyQueue persisted;
    bool replay_completed_output = false;
    std::unique_ptr<csgify::ProjectStore> autonomy_project;
    std::string project_request_digest;
    const std::string project_lease_owner = "autonomy:"
        + std::to_string(std::chrono::high_resolution_clock::now()
                             .time_since_epoch().count())
        + ':' + std::to_string(
            std::hash<std::thread::id>{}(std::this_thread::get_id()));
    const std::int64_t project_lease_expires_at =
        static_cast<std::int64_t>(std::time(nullptr)) + 300;
    struct ProjectClaimGuard {
        csgify::ProjectStore* project = nullptr;
        std::string request_id;
        std::string request_digest;
        std::string lease_owner;
        csgify::ProjectAutonomyRequestState previous_state =
            csgify::ProjectAutonomyRequestState::running;
        bool active = false;
        bool stop_requested = false;
        std::mutex heartbeat_mutex;
        std::condition_variable heartbeat_condition;
        std::thread heartbeat;

        ~ProjectClaimGuard() {
            stop_heartbeat();
            if (!active || !project) return;
            project->rollback_autonomy_request_claim(
                request_id, request_digest, lease_owner, previous_state,
                nullptr);
        }

        void stop_heartbeat() {
            {
                std::lock_guard<std::mutex> lock(heartbeat_mutex);
                stop_requested = true;
            }
            heartbeat_condition.notify_all();
            if (heartbeat.joinable()) heartbeat.join();
        }

        void arm(csgify::ProjectStore& store,
                 const std::string& id,
                 const std::string& digest,
                 const std::string& owner,
                 csgify::ProjectAutonomyRequestState state) {
            project = &store;
            request_id = id;
            request_digest = digest;
            lease_owner = owner;
            previous_state = state;
            active = true;
            heartbeat = std::thread([this]() {
                std::unique_lock<std::mutex> lock(heartbeat_mutex);
                while (!heartbeat_condition.wait_for(
                    lock, std::chrono::seconds(30),
                    [this]() { return stop_requested; })) {
                    lock.unlock();
                    project->renew_autonomy_request_lease(
                        request_id, request_digest, lease_owner,
                        static_cast<std::int64_t>(std::time(nullptr)) + 300,
                        nullptr);
                    lock.lock();
                }
            });
        }

        void disarm() {
            stop_heartbeat();
            active = false;
        }
    } project_claim;
    try {
        if (!resume_path.empty()) {
            persisted = csgify::read_autonomy_queue_json(resume_path);
            if (persisted.request_id.empty() || persisted.request_digest.empty())
                throw std::runtime_error(
                    "legacy queue has no request identity; fresh evaluation is required");
            if (!persisted.source_path.empty()
                && persisted.source_path != input_path) {
                throw std::runtime_error("queue source_path does not match input STEP");
            }
            const std::string current_digest =
                csgify::fnv1a128_file_hex(input_path);
            if (current_digest.empty())
                throw std::runtime_error("input STEP digest could not be computed");
            if (persisted.source_digest.empty())
                throw std::runtime_error(
                    "legacy queue has no source_digest; fresh evaluation is required");
            if (persisted.source_digest != current_digest)
                throw std::runtime_error(
                    "queue source_digest does not match input STEP; fresh evaluation is required");
            csgify::AutonomyRequest persisted_identity;
            persisted_identity.request_id = persisted.request_id;
            persisted_identity.source_digest = persisted.source_digest;
            persisted_identity.expected_revision = persisted.expected_revision;
            for (const csgify::AutonomyQueueItem& item : persisted.items)
                persisted_identity.actions.push_back(item.proposal);
            if (persisted.request_digest
                != csgify::autonomy_request_digest(persisted_identity)) {
                throw std::runtime_error(
                    "queue request_digest does not match persisted actions; fresh evaluation is required");
            }
            if (persisted.policy_digest.empty())
                throw std::runtime_error(
                    "legacy queue has no policy_digest; fresh evaluation is required");
            if (persisted.policy_digest
                != csgify::autonomy_policy_digest(persisted.policy)) {
                throw std::runtime_error(
                    "queue policy_digest does not match persisted policy; fresh evaluation is required");
            }
            if (persisted.conversion_policy_digest.empty())
                throw std::runtime_error(
                    "legacy queue has no conversion_policy_digest; fresh evaluation is required");
            const csgify::ConversionOptions persisted_options =
                conversion_options_for(persisted.preparation,
                                       persisted.policy);
            if (persisted.conversion_policy_digest
                != csgify::autonomy_conversion_policy_digest(
                    persisted_options)) {
                throw std::runtime_error(
                    "queue conversion_policy_digest does not match preparation policy; fresh evaluation is required");
            }
            if (preparation_was_set) {
                const csgify::ConversionOptions requested_options =
                    conversion_options_for(preparation, persisted.policy);
                if (csgify::autonomy_conversion_policy_digest(
                        requested_options)
                    != persisted.conversion_policy_digest) {
                    throw std::runtime_error(
                        "resume preparation options differ from the evaluated queue; fresh evaluation is required");
                }
            }
            const csgify::AutonomyMode requested_mode = policy.mode;
            policy = persisted.policy;
            if (mode_was_set) policy.mode = requested_mode;
            preparation = persisted.preparation;
            request.request_id = persisted.request_id;
            request.source_digest = persisted.source_digest;
            request.expected_revision = persisted.expected_revision;
            for (const csgify::AutonomyQueueItem& item : persisted.items) {
                if (!item.executed && !item.rejected
                    && item.disposition != csgify::AutonomyDisposition::denied
                    && item.proposal.op.rfind("coverage_", 0) != 0)
                    request.actions.push_back(item.proposal);
            }
        } else if (!actions_json.empty()) {
            request = csgify::parse_autonomy_request_json(actions_json);
        } else {
            request = csgify::read_autonomy_request_json(actions_path);
        }
        if (resume_path.empty()) {
            const std::string current_digest =
                csgify::fnv1a128_file_hex(input_path);
            if (current_digest.empty())
                throw std::runtime_error(
                    "input STEP digest could not be computed");
            if (request.source_digest != current_digest)
                throw std::runtime_error(
                    "request source_digest does not match input STEP");
        }
    } catch (const std::exception& e) {
        std::cerr << "error autonomy_request_invalid: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
    csgify::ConversionOptions autonomy_options =
        conversion_options_for(preparation, policy);
    if (!project_path.empty()) {
        autonomy_project = std::make_unique<csgify::ProjectStore>(project_path);
        std::string project_error;
        if (!autonomy_project->open(&project_error)) {
            std::cerr << "error autonomy_project_open_failed: "
                      << project_error << '\n';
            return EXIT_FAILURE;
        }
        const std::string policy_digest =
            csgify::autonomy_policy_digest(policy);
        const std::string conversion_digest =
            csgify::autonomy_conversion_policy_digest(autonomy_options);
        if (!resume_path.empty()) {
            const auto existing = autonomy_project->find_autonomy_request(
                persisted.request_id, &project_error);
            if (!project_error.empty()) {
                std::cerr << "error autonomy_project_lookup_failed: "
                          << project_error << '\n';
                return EXIT_FAILURE;
            }
            if (existing) {
                if (existing->source_digest != persisted.source_digest
                    || existing->policy_digest != policy_digest
                    || existing->conversion_policy_digest != conversion_digest) {
                    std::cerr << "error autonomy_project_request_conflict: request_id "
                              << persisted.request_id
                              << " is bound to different source or policy input\n";
                    return EXIT_FAILURE;
                }
                project_request_digest = existing->request_digest;
                if (existing->state
                    == csgify::ProjectAutonomyRequestState::completed) {
                    // Reconstruction may still be requested to regenerate an
                    // output file, but the durable completed outcome is final.
                    autonomy_project.reset();
                } else if (!autonomy_project->claim_autonomy_request(
                               existing->request_id,
                               existing->request_digest,
                               project_lease_owner,
                               project_lease_expires_at,
                               &project_error)) {
                    std::cerr << "error autonomy_project_claim_failed: "
                              << project_error << '\n';
                    return EXIT_FAILURE;
                } else {
                    project_claim.arm(
                        *autonomy_project, existing->request_id,
                        existing->request_digest, project_lease_owner,
                        existing->state);
                }
            } else {
                csgify::ProjectAutonomyRequestRecord reservation;
                reservation.request_id = persisted.request_id;
                reservation.request_digest = persisted.request_digest;
                reservation.source_digest = persisted.source_digest;
                reservation.policy_digest = policy_digest;
                reservation.conversion_policy_digest = conversion_digest;
                reservation.lease_owner = project_lease_owner;
                reservation.lease_expires_at = project_lease_expires_at;
                reservation.created_at = static_cast<std::int64_t>(
                    std::time(nullptr));
                const auto reserved =
                    autonomy_project->reserve_autonomy_request(
                        reservation, &project_error);
                if (reserved.status
                    != csgify::ProjectAutonomyReservationStatus::reserved) {
                    std::cerr << "error autonomy_project_reservation_failed: "
                              << (project_error.empty()
                                      ? "request identity conflict"
                                      : project_error)
                              << '\n';
                    return EXIT_FAILURE;
                }
                project_request_digest = reservation.request_digest;
                project_claim.arm(
                    *autonomy_project, reservation.request_id,
                    reservation.request_digest, project_lease_owner,
                    csgify::ProjectAutonomyRequestState::running);
            }
        } else {
            csgify::ProjectAutonomyRequestRecord reservation;
            reservation.request_id = request.request_id;
            reservation.request_digest =
                csgify::autonomy_request_digest(request);
            reservation.source_digest = request.source_digest;
            reservation.policy_digest = policy_digest;
            reservation.conversion_policy_digest = conversion_digest;
            reservation.lease_owner = project_lease_owner;
            reservation.lease_expires_at = project_lease_expires_at;
            reservation.created_at = static_cast<std::int64_t>(
                std::time(nullptr));
            const auto reserved = autonomy_project->reserve_autonomy_request(
                reservation, &project_error);
            if (reserved.status
                == csgify::ProjectAutonomyReservationStatus::conflict) {
                std::cerr << "error autonomy_project_request_conflict: request_id "
                          << request.request_id
                          << " is already bound to different input\n";
                return EXIT_FAILURE;
            }
            if (reserved.status
                == csgify::ProjectAutonomyReservationStatus::error) {
                std::cerr << "error autonomy_project_reservation_failed: "
                          << project_error << '\n';
                return EXIT_FAILURE;
            }
            project_request_digest = reservation.request_digest;
            if (reserved.status
                == csgify::ProjectAutonomyReservationStatus::reserved) {
                project_claim.arm(
                    *autonomy_project, reservation.request_id,
                    reservation.request_digest, project_lease_owner,
                    csgify::ProjectAutonomyRequestState::running);
            }
            if (reserved.status
                    == csgify::ProjectAutonomyReservationStatus::existing) {
                const csgify::ProjectAutonomyRequestRecord& existing =
                    *reserved.record;
                if (!existing.queue_artifact) {
                    std::cerr << "error autonomy_project_request_in_progress: "
                              << request.request_id << '\n';
                    return 2;
                }
                if (!autonomy_project->artifacts().validate(
                        *existing.queue_artifact, &project_error)) {
                    std::cerr << "error autonomy_project_queue_invalid: "
                              << project_error << '\n';
                    return EXIT_FAILURE;
                }
                const std::filesystem::path stored_queue =
                    autonomy_project->artifacts().absolute_path(
                        *existing.queue_artifact);
                const bool deliver_completed_output = existing.state
                        == csgify::ProjectAutonomyRequestState::completed
                    && (!output_path.empty() || !step_output_path.empty());
                if (deliver_completed_output) {
                    try {
                        persisted = csgify::read_autonomy_queue_json(
                            stored_queue.string());
                    } catch (const std::exception& e) {
                        std::cerr << "error autonomy_project_queue_read_failed: "
                                  << e.what() << '\n';
                        return EXIT_FAILURE;
                    }
                    // The database digest identifies the submitted request;
                    // the queue digest identifies its normalized executable
                    // actions. They intentionally differ after normalization.
                    if (persisted.request_id != existing.request_id
                        || persisted.source_digest != existing.source_digest) {
                        std::cerr << "error autonomy_project_queue_identity_mismatch\n";
                        return EXIT_FAILURE;
                    }
                    policy = persisted.policy;
                    preparation = persisted.preparation;
                    autonomy_options = conversion_options_for(
                        preparation, policy);
                    request.actions.clear();
                    for (const csgify::AutonomyQueueItem& item
                         : persisted.items) {
                        if (!item.executed && !item.rejected
                            && item.disposition
                                != csgify::AutonomyDisposition::denied
                            && item.proposal.op.rfind("coverage_", 0) != 0) {
                            request.actions.push_back(item.proposal);
                        }
                    }
                    replay_completed_output = true;
                    // The queue result is already durable and final. Rebuild
                    // geometry only to deliver explicitly requested outputs;
                    // do not claim or rewrite the logical request.
                    autonomy_project.reset();
                    std::cerr << "autonomy_request_output_replayed "
                              << request.request_id << '\n';
                } else {
                    std::ifstream stored(stored_queue, std::ios::binary);
                    if (!stored) {
                        std::cerr << "error autonomy_project_queue_read_failed\n";
                        return EXIT_FAILURE;
                    }
                    if (queue_report_path == "-") {
                        std::cout << stored.rdbuf();
                    } else {
                        std::ofstream output(
                            queue_report_path,
                            std::ios::binary | std::ios::trunc);
                        output << stored.rdbuf();
                        if (!output) {
                            std::cerr
                                << "error autonomy_queue_report_write_failed: "
                                << queue_report_path << '\n';
                            return EXIT_FAILURE;
                        }
                    }
                    std::cerr << "autonomy_request_reused "
                              << request.request_id << '\n';
                    return existing.state
                            == csgify::ProjectAutonomyRequestState::completed
                        ? EXIT_SUCCESS
                        : existing.state
                                == csgify::ProjectAutonomyRequestState::awaiting_decision
                            ? 2
                            : EXIT_FAILURE;
                }
            }
        }
    }
    const bool reconstructing_queue =
        !resume_path.empty() || replay_completed_output;
    csgify::SessionPtr session = csgify::create_session(autonomy_options);
    if (!log_path.empty() && !reconstructing_queue) {
        try {
            csgify::attach_decision_log(*session, log_path, input_path);
        } catch (const std::exception& e) {
            std::cerr << "error autonomy_decision_log_open_failed: "
                      << e.what() << '\n';
            return EXIT_FAILURE;
        }
    }
    csgify::DocumentId document;
    try {
        document = csgify::load_step(*session, input_path);
    } catch (const std::exception& e) {
        std::cerr << "error autonomy_execution_audit_failed: "
                  << e.what() << '\n';
        return EXIT_FAILURE;
    }
    if (!reconstructing_queue
        && request.expected_revision
               != csgify::document_revision(*session, document)) {
        std::cerr << "error autonomy_request_stale: expected revision "
                  << request.expected_revision << ", current revision "
                  << csgify::document_revision(*session, document) << '\n';
        return EXIT_FAILURE;
    }
    for (const csgify::MaterialDefinition& material
         : policy.contained_medium_materials) {
        csgify::CommandResult configured;
        try {
            configured = csgify::define_material(
                *session, document, material);
        } catch (const std::exception& e) {
            std::cerr << "error autonomy_execution_audit_failed: "
                      << e.what() << '\n';
            return EXIT_FAILURE;
        }
        if (!configured.ok) {
            std::cerr << "error contained_medium_material_configuration_failed\n";
            return EXIT_FAILURE;
        }
    }
    csgify::analyze(*session, document);
    if (reconstructing_queue) {
        for (const csgify::AutonomyQueueItem& item : persisted.items) {
            if (!item.executed || !item.execution_ok || item.rejected) continue;
            const csgify::OperationDescriptor* replay_descriptor =
                csgify::find_operation(item.proposal.op);
            if (replay_descriptor
                && replay_descriptor->autonomy_kind
                    == csgify::AutonomyOperationKind::decompose_to_fixed_point
                && !item.evidence.mutation_applied) {
                continue;
            }
            csgify::AutonomyQueueItem replay_item = item;
            const csgify::AutonomyOperationExecution reconstruction =
                csgify::execute_autonomy_operation(
                    *session, document, replay_item, policy);
            if (!reconstruction.ok) {
                std::cerr << "error autonomy_resume_reconstruction_failed\n";
                return EXIT_FAILURE;
            }
        }
        const std::size_t reconstructed_revision =
            csgify::document_revision(*session, document);
        for (const csgify::AutonomyQueueItem& item : persisted.items) {
            if (!item.executed && !item.rejected
                && item.disposition != csgify::AutonomyDisposition::denied
                && item.proposal.op.rfind("coverage_", 0) != 0
                && item.evaluated_revision != reconstructed_revision) {
                std::cerr << "error autonomy_resume_stale_revision: queued revision "
                          << item.evaluated_revision << ", reconstructed revision "
                          << reconstructed_revision << '\n';
                return EXIT_FAILURE;
            }
        }
    }
    // Reconstructing already-applied queue items restores session state but is
    // not a new decision. Attach the requested log only after reconstruction
    // so a resume does not duplicate historical operation records.
    if (!log_path.empty() && !resume_path.empty()) {
        try {
            csgify::attach_decision_log(*session, log_path, input_path);
        } catch (const std::exception& e) {
            std::cerr << "error autonomy_decision_log_open_failed: "
                      << e.what() << '\n';
            return EXIT_FAILURE;
        }
    }
    csgify::AutonomyQueue queue = csgify::evaluate_autonomy_queue(
        *session, document, request.actions, policy);
    try {
        csgify::execute_autonomy_queue(*session, queue, policy);
    } catch (const std::exception& e) {
        std::cerr << "error autonomy_execution_audit_failed: "
                  << e.what() << '\n';
        return EXIT_FAILURE;
    }
    queue.source_path = input_path;
    queue.source_digest = csgify::fnv1a128_file_hex(input_path);
    queue.preparation = preparation;
    queue.policy_digest = csgify::autonomy_policy_digest(policy);
    queue.conversion_policy_digest =
        csgify::autonomy_conversion_policy_digest(autonomy_options);
    if (reconstructing_queue) {
        queue.request_id = persisted.request_id;
        queue.request_digest = persisted.request_digest;
        queue.expected_revision = persisted.expected_revision;
        queue.coverage_certificate_path =
            persisted.coverage_certificate_path;
        queue.coverage_certificate_digest =
            persisted.coverage_certificate_digest;
        std::vector<csgify::AutonomyQueueItem> combined;
        for (const csgify::AutonomyQueueItem& item : persisted.items) {
            if (item.executed || item.rejected
                || item.disposition == csgify::AutonomyDisposition::denied
                || item.proposal.op.rfind("coverage_", 0) == 0)
                combined.push_back(item);
        }
        combined.insert(combined.end(), queue.items.begin(), queue.items.end());
        queue.items = std::move(combined);
        csgify::refresh_autonomy_queue_counts(queue);
    } else {
        queue.request_id = request.request_id;
        queue.expected_revision = request.expected_revision;
        csgify::AutonomyRequest normalized_identity;
        normalized_identity.request_id = queue.request_id;
        normalized_identity.source_digest = queue.source_digest;
        normalized_identity.expected_revision = queue.expected_revision;
        for (const csgify::AutonomyQueueItem& item : queue.items)
            normalized_identity.actions.push_back(item.proposal);
        queue.request_digest =
            csgify::autonomy_request_digest(normalized_identity);
    }

    const auto checkpoint_project_queue = [&]() {
        if (!autonomy_project) return true;
        const auto nonce = std::chrono::steady_clock::now()
            .time_since_epoch().count();
        const std::filesystem::path temporary =
            autonomy_project->artifacts().root() / "tmp"
            / ("autonomy-" + queue.request_digest + '-'
               + std::to_string(nonce) + ".json");
        if (csgify::write_autonomy_queue_json(queue, temporary.string()) != 0) {
            std::cerr << "error autonomy_project_queue_write_failed\n";
            return false;
        }
        csgify::ArtifactRecord artifact;
        std::string project_error;
        const bool published = autonomy_project->artifacts().publish_file(
            "autonomy-queue", "json", temporary, &artifact, &project_error);
        std::error_code remove_error;
        std::filesystem::remove(temporary, remove_error);
        if (!published) {
            std::cerr << "error autonomy_project_queue_publish_failed: "
                      << project_error << '\n';
            return false;
        }
        const csgify::ProjectAutonomyRequestState state =
            queue.denied_count > 0 || queue.failed_count > 0
            ? csgify::ProjectAutonomyRequestState::failed
            : queue.human_review_count > 0
                ? csgify::ProjectAutonomyRequestState::awaiting_decision
                : csgify::ProjectAutonomyRequestState::completed;
        if (!autonomy_project->finish_autonomy_request(
                queue.request_id, project_request_digest,
                project_lease_owner, state, artifact,
                static_cast<std::int64_t>(std::time(nullptr)),
                &project_error)) {
            std::cerr << "error autonomy_project_checkpoint_failed: "
                      << project_error << '\n';
            return false;
        }
        project_claim.disarm();
        return true;
    };

    if (csgify::write_autonomy_queue_json(queue, queue_report_path) != 0) {
        std::cerr << "error autonomy_queue_report_write_failed: "
                  << queue_report_path << '\n';
        return EXIT_FAILURE;
    }
    if (!checkpoint_project_queue()) return EXIT_FAILURE;
    if (open_review && queue.human_review_count > 0) {
#if CSGIFY_HAVE_TKOPENGL && CSGIFY_HAVE_QT6_VIEWER
        const int review_result = csgify::run_qt_autonomy_queue(
            *session, input_path, queue, queue_report_path);
        if (review_result != 0) return review_result;
        csgify::refresh_autonomy_queue_counts(queue);
        if (csgify::write_autonomy_queue_json(queue, queue_report_path) != 0) {
            std::cerr << "error autonomy_queue_report_write_failed: "
                      << queue_report_path << '\n';
            return EXIT_FAILURE;
        }
        if (!checkpoint_project_queue()) return EXIT_FAILURE;
#else
        const csgify::AutonomyQueueItem* escalated = nullptr;
        for (const csgify::AutonomyQueueItem& item : queue.items) {
            if (item.disposition == csgify::AutonomyDisposition::human_review) {
                escalated = &item;
                break;
            }
        }
        if (escalated) {
            csgify::ViewerReviewContext review;
            review.action = escalated->proposal.op;
            review.target_id = escalated->proposal.target_id;
            review.candidate_id = escalated->proposal.candidate_id;
            review.review_id = escalated->proposal.id;
            review.audit.actor = "human";
            review.audit.proposal_actor = escalated->proposal.actor;
            review.audit.proposal_id = escalated->proposal.id;
            review.audit.rationale = escalated->proposal.rationale;
            review.audit.policy_rule = escalated->policy_rule;
            review.audit.approval_mode = "manual";
            std::string preselect = escalated->proposal.target_id;
            csgify::DecompositionProposal decomposition;
            const csgify::DecompositionProposal* proposal = nullptr;
            if (escalated->proposal.op == "apply_decomposition_candidate") {
                review.kind = "decomposition";
                decomposition = csgify::propose_decomposition(
                    *session, escalated->proposal.target_id);
                proposal = &decomposition;
            } else if (escalated->proposal.op
                       == "apply_surface_replacement_candidate") {
                review.kind = "surface_replacement";
                const csgify::SurfaceReplacementProposal surface =
                    csgify::propose_surface_replacement(
                        *session, escalated->proposal.target_id);
                for (const csgify::SurfaceReplacementCandidate& candidate
                     : surface.candidates) {
                    if (candidate.id == escalated->proposal.candidate_id) {
                        review.surface_replacement = candidate;
                        review.has_surface_replacement = true;
                        preselect = candidate.owner_target_id;
                        break;
                    }
                }
            } else if (escalated->proposal.op == "remove_solid") {
                review.kind = "removal";
            } else if (escalated->proposal.op == "apply_contained_medium"
                       && escalated->proposal.medium_request) {
                review.kind = "contained_medium";
                review.medium_request = *escalated->proposal.medium_request;
                review.candidate_id = review.medium_request.id;
                review.medium_proposal = csgify::propose_contained_medium(
                    *session, document, review.medium_request);
                if (!review.medium_proposal.ok) {
                    std::cerr << "error autonomy_medium_review_certificate_failed\n";
                    return 2;
                }
                review.has_medium = true;
                preselect = review.medium_proposal.host_shape_id;
            } else {
                std::cerr << "error autonomy_review_unsupported: "
                          << escalated->proposal.op << '\n';
                return 2;
            }
#if CSGIFY_HAVE_TKOPENGL
#if CSGIFY_HAVE_QT6_VIEWER
            return csgify::run_qt_viewer_command(
                *session, input_path, document, preselect, proposal,
                escalated->proposal.candidate_id, review, output_path);
#elif CSGIFY_HAVE_X11_VIEWER
            return csgify::run_viewer_command(
                *session, input_path, document, preselect, proposal,
                escalated->proposal.candidate_id, review, output_path);
#else
            std::cerr << "error viewer_not_built\n";
            return 2;
#endif
#else
            std::cerr << "error viewer_not_built: use the queue report for external review\n";
            return 2;
#endif
        }
#endif
    }
    const bool hard_failed = queue.denied_count > 0 || queue.failed_count > 0;
    const bool blocked = queue.human_review_count > 0 || hard_failed;
    // Preserve certified partial progress for the next bounded shatter/review
    // step, but never export an unchanged STEP for a zero-progress attempt.
    if (!hard_failed && queue.applied_count > 0 && !step_output_path.empty()) {
        csgify::CommandResult result;
        try {
            result = csgify::export_document_step(
                *session, document, step_output_path);
        } catch (const std::exception& e) {
            std::error_code remove_error;
            std::filesystem::remove(step_output_path, remove_error);
            std::cerr << "error autonomy_output_audit_failed: "
                      << e.what() << '\n';
            return EXIT_FAILURE;
        }
        if (!result.ok) return EXIT_FAILURE;
    }
    if (!blocked && !output_path.empty()) {
        csgify::CommandResult result;
        try {
            result = csgify::emit_document(
                *session, document, output_path);
        } catch (const std::exception& e) {
            std::error_code remove_error;
            std::filesystem::remove(output_path, remove_error);
            std::cerr << "error autonomy_output_audit_failed: "
                      << e.what() << '\n';
            return EXIT_FAILURE;
        }
        if (!result.ok) return EXIT_FAILURE;
    }
    if (queue.denied_count > 0 || queue.failed_count > 0) return EXIT_FAILURE;
    if (queue.human_review_count > 0) return 2;
    return EXIT_SUCCESS;
}

int run_view_command(int argc, char** argv) {
    if (wants_help(argc, argv)) return print_verb_help_and_exit(argv[0], "view");
    if (argc < 3) {
        std::cerr << "usage: " << argv[0] << " view input.step"
                     " [--select doc:N/solid:M|solid:M]"
                     " [--candidate candidate:N|best]"
                     " [--review-removal removal:N]"
                     " [--review-removals]"
                     " [--review-hole-fill hole-fill:N]"
                     " [--review-decomp candidate:N]"
                     " [--review-surface-replacement candidate:N --select-face face:N.M]"
                     " [--log decisions.json] [--out output.i]"
                     " [--viewer-backend auto|qt|x11]"
                     " [--keys A,Tab,A,Q  (headless: replay keys, no window)]\n";
        return EXIT_FAILURE;
    }
    const std::string input_path = argv[2];

    std::string preselect_id;
    std::string candidate_id;
    std::string review_removal_id;
    bool review_removals_all = false;
    std::string review_hole_fill_id;
    std::string review_decomp_id;
    std::string review_surface_id;
    std::string select_face_id;
    std::string log_path;
    std::string commit_out_path;
    std::string headless_keys;
    std::string viewer_backend = "auto";
    csgify::HoleFillOptions hole_fill_options;

    for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "--select") == 0 && i + 1 < argc) {
            preselect_id = argv[++i];
        } else if (std::strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            log_path = argv[++i];
        } else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            commit_out_path = argv[++i];
        } else if (std::strcmp(argv[i], "--keys") == 0 && i + 1 < argc) {
            headless_keys = argv[++i];
        } else if (std::strcmp(argv[i], "--viewer-backend") == 0 && i + 1 < argc) {
            viewer_backend = argv[++i];
        } else if (std::strcmp(argv[i], "--candidate") == 0 && i + 1 < argc) {
            candidate_id = argv[++i];
        } else if (std::strcmp(argv[i], "--review-removal") == 0 && i + 1 < argc) {
            review_removal_id = argv[++i];
        } else if (std::strcmp(argv[i], "--review-removals") == 0) {
            review_removals_all = true;
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

    if (viewer_backend != "auto" && viewer_backend != "qt"
        && viewer_backend != "x11") {
        std::cerr << "error invalid_viewer_backend: expected auto, qt, or x11\n";
        return EXIT_FAILURE;
    }

    const int review_count = (review_removal_id.empty() ? 0 : 1)
        + (review_removals_all ? 1 : 0)
        + (review_hole_fill_id.empty() ? 0 : 1)
        + (review_decomp_id.empty() ? 0 : 1)
        + (review_surface_id.empty() ? 0 : 1);
    if (review_count > 1) {
        std::cerr << "error view_review_conflict: use only one --review-* option\n";
        return EXIT_FAILURE;
    }

    csgify::ConversionOptions options;
    csgify::SessionPtr session = csgify::create_session(options);
    if (!log_path.empty()) {
        csgify::attach_decision_log(*session, log_path, input_path);
    }
    const csgify::DocumentId doc_id = csgify::load_step(*session, input_path);
    csgify::analyze(*session, doc_id, csgify::ReportIdMode::scoped);

    const csgify::DecompositionProposal* proposal = nullptr;
    csgify::DecompositionProposal owned_proposal;
    csgify::ViewerReviewContext review;

    if (!review_removal_id.empty()) {
        csgify::RemovalProposal removals =
            csgify::suggest_removals(*session,
                                      doc_id,
                                      csgify::ReportIdMode::scoped);
        const csgify::RemovalCandidate* selected = nullptr;
        for (const csgify::RemovalCandidate& c : removals.candidates) {
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

    if (review_removals_all) {
        csgify::RemovalProposal removals =
            csgify::suggest_removals(*session,
                                      doc_id,
                                      csgify::ReportIdMode::scoped);
        if (removals.candidates.empty()) {
            std::cerr << "view_review_removals: no removal candidates in "
                      << input_path << "\n";
        }
        review.kind = "removal_set";
        review.action = "remove_solid";
        for (const csgify::RemovalCandidate& c : removals.candidates) {
            csgify::ViewerReviewAlternative alt;
            alt.review_id = c.id;
            alt.target_id = c.solid_id;
            alt.component_instance_id = c.component_instance_id;
            alt.component_definition_id = c.component_definition_id;
            alt.label = c.label;
            alt.name = c.name;
            alt.hierarchy_path = c.hierarchy_path;
            alt.confidence = c.confidence;
            alt.volume_fraction = c.volume_fraction;
            alt.score = c.score;
            alt.reason_codes = c.reason_codes;
            alt.blockers = c.blockers;
            alt.suggested_action = c.suggested_action;
            review.alternatives.push_back(std::move(alt));
        }
    }

    if (!review_hole_fill_id.empty()) {
        csgify::HoleFillProposal holes =
            csgify::propose_hole_fills(*session,
                                        doc_id,
                                        hole_fill_options,
                                        csgify::ReportIdMode::scoped);
        const csgify::HoleFillCandidate* selected = nullptr;
        for (const csgify::HoleFillCandidate& c : holes.candidates) {
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
        csgify::SurfaceReplacementProposal surface =
            csgify::propose_surface_replacement(*session,
                                                 select_face_id,
                                                 csgify::ReportIdMode::scoped);
        const csgify::SurfaceReplacementCandidate* selected = nullptr;
        for (const csgify::SurfaceReplacementCandidate& c : surface.candidates) {
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
        owned_proposal = csgify::propose_decomposition(
            *session, preselect_id, csgify::ReportIdMode::scoped);
        proposal = &owned_proposal;
        if (candidate_id == "best") {
            candidate_id = owned_proposal.best_candidate_id;
            if (review_decomp_id == "best") review_decomp_id = candidate_id;
        }
        const csgify::DecompositionCandidate* selected = nullptr;
        for (const csgify::DecompositionCandidate& c : owned_proposal.candidates) {
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
        if (selected) {
            // Even without --review-decomp, a resolved candidate makes A apply
            // it: the controller dispatches on review.action/target/candidate.
            review.action = "apply_decomposition_candidate";
            review.target_id = selected->target_id;
            review.candidate_id = selected->id;
            review.kind = "decomposition";
            review.review_id = selected->id;
        }
    }

    // Headless replay path: no window, no display, no TKOpenGl needed.
    if (!headless_keys.empty()) {
        return csgify::run_viewer_headless(*session, doc_id, review,
                                            headless_keys, commit_out_path,
                                            std::cout, std::cerr);
    }

    if (!review.action.empty()) {
        review.audit.actor = "human";
        review.audit.proposal_actor = "human";
        review.audit.proposal_id = review.review_id;
        review.audit.rationale = "accepted in the interactive geometry reviewer";
        review.audit.policy_rule = "interactive.human_decision";
        review.audit.approval_mode = "manual";
    }

#if CSGIFY_HAVE_TKOPENGL
#if CSGIFY_HAVE_QT6_VIEWER
    if (viewer_backend != "x11") {
        return csgify::run_qt_viewer_command(
            *session, input_path, doc_id, preselect_id, proposal, candidate_id,
            review, commit_out_path);
    }
#else
    if (viewer_backend == "qt") {
        std::cerr << "error qt_viewer_not_built: rebuild with Qt 6 Widgets or "
                     "use --viewer-backend x11\n";
        return EXIT_FAILURE;
    }
#endif
#if CSGIFY_HAVE_X11_VIEWER
    return csgify::run_viewer_command(
        *session, input_path, doc_id, preselect_id, proposal, candidate_id,
        review, commit_out_path);
#else
    std::cerr << "error x11_viewer_not_built: use the Qt viewer on this platform\n";
    return EXIT_FAILURE;
#endif
#else
    (void)input_path; (void)preselect_id; (void)proposal; (void)candidate_id;
    std::cerr << "error viewer_not_built: csgify was built without TKOpenGl "
                 "support. Use --keys for headless review.\n";
    return EXIT_FAILURE;
#endif
}

int run_viewer_check_command() {
    std::cout << "occt_version " << csgify::occt_version() << '\n';

#if CSGIFY_HAVE_TKOPENGL
    if (Graphic3d_GraphicDriverFactory::DefaultDriverFactory().IsNull()) {
        Graphic3d_GraphicDriverFactory::RegisterFactory(
            new OpenGl_GraphicDriverFactory(),
            true);
    }
#endif

    const bool has_factory =
        !Graphic3d_GraphicDriverFactory::DefaultDriverFactory().IsNull();
    std::cout << "tkopengl_linked "
#if CSGIFY_HAVE_TKOPENGL
              << "true"
#else
              << "false"
#endif
              << '\n';
    std::cout << "qt6_viewer "
#if CSGIFY_HAVE_QT6_VIEWER
              << "true"
#else
              << "false"
#endif
              << '\n';
    std::cout << "default_viewer_backend "
#if CSGIFY_HAVE_QT6_VIEWER
              << "qt"
#elif CSGIFY_HAVE_X11_VIEWER
              << "x11"
#else
              << "none"
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

#if !defined(__APPLE__)
    if ((!display || !display[0]) && (!wayland || !wayland[0])) {
        std::cout << "status probably_unavailable\n";
        std::cout << "reason no DISPLAY or WAYLAND_DISPLAY is set for the OCCT viewer.\n";
        return EXIT_FAILURE;
    }

#endif
#if !CSGIFY_HAVE_QT6_VIEWER && !CSGIFY_HAVE_X11_VIEWER
    std::cout << "status unavailable\nreason no viewer frontend was built\n";
    return EXIT_FAILURE;
#endif
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

    csgify::ConversionOptions options;
    csgify::SessionPtr session = csgify::create_session(options);
    csgify::load_step(*session, input_path);
    csgify::DecompositionProposal proposal =
        csgify::propose_decomposition(*session,
                                       target_id,
                                       csgify::ReportIdMode::local);

    if (!report_path.empty()) {
        if (write_decomp_proposal_or_stdout(proposal, report_path) != 0) {
            std::cerr << "error report_write_failed: " << report_path << '\n';
            return EXIT_FAILURE;
        }
    } else {
        std::cout << csgify::decomposition_proposal_json_string(proposal);
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
    csgify::DecisionPackOptions pack_options;
    csgify::HoleFillOptions hole_fill_options;
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

    csgify::ConversionOptions options;
    csgify::CadModel model = csgify::load_step_model(input_path, options);
    const csgify::DocumentId document_id = "doc:1";
    csgify::ConversionReport decision_report;
    if (use_hole_fill_proposal) {
        csgify::HoleFillProposal proposal =
            csgify::propose_hole_fill_candidates(model,
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
        decision_report = csgify::analyze_model(model, options);
        csgify::detail::detect_solid_overlaps(model, options, decision_report);
    }

    csgify::DecisionPackResult result =
        csgify::write_decision_packs(decision_report, model, pack_options);
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

    csgify::ConversionOptions options;
    csgify::SessionPtr session = csgify::create_session(options);
    const auto load_started = std::chrono::steady_clock::now();
    std::cerr << "stage load_step: started input=" << input_path << '\n';
    const csgify::DocumentId document_id = csgify::load_step(*session, input_path);
    const double load_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - load_started).count();
    std::cerr << "stage load_step: completed elapsed_s="
              << std::fixed << std::setprecision(1) << load_seconds << '\n';
    csgify::RemovalProposal proposal =
        csgify::suggest_removals(*session,
                                  document_id,
                                  csgify::ReportIdMode::local);

    if (!report_path.empty()) {
        if (write_removal_proposal_or_stdout(proposal, report_path) != 0) {
            std::cerr << "error report_write_failed: " << report_path << '\n';
            return EXIT_FAILURE;
        }
    } else {
        std::cout << csgify::removal_proposal_json_string(proposal);
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
    csgify::HoleFillOptions hole_options;

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

    csgify::ConversionOptions options;
    csgify::SessionPtr session = csgify::create_session(options);
    const csgify::DocumentId document_id = csgify::load_step(*session, input_path);
    csgify::HoleFillProposal proposal =
        csgify::propose_hole_fills(*session,
                                    document_id,
                                    hole_options,
                                    csgify::ReportIdMode::local);

    if (!report_path.empty()) {
        if (write_hole_fill_proposal_or_stdout(proposal, report_path) != 0) {
            std::cerr << "error report_write_failed: " << report_path << '\n';
            return EXIT_FAILURE;
        }
    } else {
        std::cout << csgify::hole_fill_proposal_json_string(proposal);
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

    csgify::ConversionOptions options;
    csgify::SessionPtr session = csgify::create_session(options);
    csgify::load_step(*session, input_path);
    csgify::SurfaceReplacementProposal proposal =
        csgify::propose_surface_replacement(*session,
                                             face_id,
                                             csgify::ReportIdMode::local);

    if (!report_path.empty()) {
        if (write_surface_proposal_or_stdout(proposal, report_path) != 0) {
            std::cerr << "error report_write_failed: " << report_path << '\n';
            return EXIT_FAILURE;
        }
    } else {
        std::cout << csgify::surface_replacement_proposal_json_string(proposal);
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

    csgify::ReplacementLimits replacement_limits;
    std::string report_path;
    std::string proposal_report_path;
    std::string log_path;
    std::string step_out_path;
    std::string by_face_id;
    std::string candidate_id;
    std::string overlap_owner_id;
    double max_depth_value = 4.0;
    csgify::DecompositionPolicy decomposition_policy;
    csgify::HoleFillOptions hole_fill_options;
    bool has_max_deviation = false;
    double max_deviation = 0.0;
    bool allow_overlaps = false;
    bool allow_shatter_volume_drift = false;
    bool union_decomposed_pieces = false;
    std::string approximation_percent;
    std::string approximation_body_percent;
    std::string approximation_action_percent;
    std::string approximation_added_percent;
    std::string approximation_removed_percent;
    std::string approximation_preset;
    std::vector<std::string> protected_targets;
    std::vector<int> protected_materials;
    bool allow_containment_change = false;
    bool allow_topology_change = false;
    std::string mcnp_domain_id;
    bool require_domain_coverage = false;

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
        if (scan.take_string("--owner", &overlap_owner_id)) continue;
        if (scan.take_string("--approx-volume", &approximation_percent)) continue;
        if (scan.take_string("--approx-body-volume", &approximation_body_percent)) continue;
        if (scan.take_string("--approx-action-volume", &approximation_action_percent)) continue;
        if (scan.take_string("--approx-added-volume", &approximation_added_percent)) continue;
        if (scan.take_string("--approx-removed-volume", &approximation_removed_percent)) continue;
        if (scan.take_string("--approx-preset", &approximation_preset)) continue;
        std::string protected_target;
        if (scan.take_string("--protect", &protected_target)) {
            protected_targets.push_back(protected_target);
            continue;
        }
        std::size_t protected_material = 0;
        if (scan.take_size("--protect-material", &protected_material)) {
            if (protected_material > static_cast<std::size_t>(
                    std::numeric_limits<int>::max())) {
                std::cerr << "error invalid_protected_material\n";
                return EXIT_FAILURE;
            }
            protected_materials.push_back(static_cast<int>(protected_material));
            continue;
        }
        if (scan.take_string("--mcnp-domain", &mcnp_domain_id)) continue;
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
        if (scan.take_flag("--allow-overlaps")) { allow_overlaps = true; continue; }
        if (scan.take_flag("--allow-shatter-volume-drift")) {
            allow_shatter_volume_drift = true;
            continue;
        }
        if (scan.take_flag("--union-decomposed-pieces")) {
            union_decomposed_pieces = true;
            continue;
        }
        if (scan.take_flag("--require-domain-coverage")) {
            require_domain_coverage = true;
            continue;
        }
        if (scan.take_flag("--allow-containment-change")) {
            allow_containment_change = true;
            continue;
        }
        if (scan.take_flag("--allow-topology-change")) {
            allow_topology_change = true;
            continue;
        }
        std::cerr << "error unrecognized_option: " << scan.peek() << '\n';
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    std::cerr << "csgify: OCCT " << csgify::occt_version()
              << ", Alea " << alea_version() << '\n';

    csgify::ConversionOptions options;
    options.try_planar_decomposition = false;
    if (allow_overlaps) options.allow_overlaps = true;
    if (union_decomposed_pieces) options.union_decomposed_pieces = true;
    options.mcnp_domain_id = mcnp_domain_id;
    options.require_domain_coverage = require_domain_coverage;
    if (!approximation_preset.empty()) {
        if (!approximation_percent.empty()) {
            std::cerr << "error approximation_preset_conflict: use a preset or "
                         "--approx-volume, not both\n";
            return EXIT_FAILURE;
        }
        if (approximation_preset == "conservative") approximation_percent = "0.1%";
        else if (approximation_preset == "moderate") approximation_percent = "1%";
        else if (approximation_preset == "permissive") approximation_percent = "2%";
        else {
            std::cerr << "error invalid_approximation_preset: expected "
                         "conservative|moderate|permissive\n";
            return EXIT_FAILURE;
        }
    }
    const bool has_axis_override = !approximation_body_percent.empty()
        || !approximation_action_percent.empty()
        || !approximation_added_percent.empty()
        || !approximation_removed_percent.empty();
    if (approximation_percent.empty() && has_axis_override) {
        std::cerr << "error approximation_global_budget_required: specify "
                     "--approx-volume with axis overrides\n";
        return EXIT_FAILURE;
    }
    if (!approximation_percent.empty()) {
        double fraction = 0.0;
        std::string parse_error;
        if (!csgify::parse_approximation_percent(
                approximation_percent, &fraction, &parse_error)) {
            std::cerr << "error invalid_approximation_percent: "
                      << parse_error << '\n';
            return EXIT_FAILURE;
        }
        options.approximation.enabled = true;
        options.approximation.max_global_changed_volume_fraction = fraction;
        options.approximation.max_body_changed_volume_fraction = fraction;
        options.approximation.max_action_changed_volume_fraction = fraction;
        options.approximation.max_global_added_volume_fraction = fraction;
        options.approximation.max_global_removed_volume_fraction = fraction;
        options.approximation.max_body_added_volume_fraction = fraction;
        options.approximation.max_body_removed_volume_fraction = fraction;
        options.approximation.require_containment_preservation =
            !allow_containment_change;
        options.approximation.require_topology_preservation =
            !allow_topology_change;
        options.approximation.protected_target_ids = protected_targets;
        options.approximation.protected_material_ids = protected_materials;
        if (has_max_deviation)
            options.approximation.max_surface_deviation = max_deviation;
        auto override_percent = [&](const std::string& text,
                                    const char* name,
                                    double* destination) {
            if (text.empty()) return true;
            std::string override_error;
            if (!csgify::parse_approximation_percent(
                    text, destination, &override_error)) {
                std::cerr << "error invalid_approximation_percent " << name
                          << ": " << override_error << '\n';
                return false;
            }
            return true;
        };
        if (!override_percent(approximation_body_percent,
                              "--approx-body-volume",
                              &options.approximation.max_body_changed_volume_fraction)
            || !override_percent(approximation_action_percent,
                                 "--approx-action-volume",
                                 &options.approximation.max_action_changed_volume_fraction)
            || !override_percent(approximation_added_percent,
                                 "--approx-added-volume",
                                 &options.approximation.max_global_added_volume_fraction)
            || !override_percent(approximation_removed_percent,
                                 "--approx-removed-volume",
                                 &options.approximation.max_global_removed_volume_fraction)) {
            return EXIT_FAILURE;
        }
        if (!approximation_added_percent.empty())
            options.approximation.max_body_added_volume_fraction =
                options.approximation.max_global_added_volume_fraction;
        if (!approximation_removed_percent.empty())
            options.approximation.max_body_removed_volume_fraction =
                options.approximation.max_global_removed_volume_fraction;
    }
    csgify::SessionPtr session = csgify::create_session(options);
    if (!log_path.empty()) {
        try {
            csgify::attach_decision_log(*session, log_path, input_path);
        } catch (const std::exception& e) {
            std::cerr << "error decision_log_open_failed: " << e.what() << '\n';
            return EXIT_FAILURE;
        }
    }
    const csgify::DocumentId document_id = csgify::load_step(*session, input_path);

    const bool has_volume_limits =
        replacement_limits.has_absolute_delta || replacement_limits.has_relative_delta;

    csgify::ReplacementOperation operation;
    csgify::CommandResult action_result;
    if (operation_from_action(action, &operation)) {
        if (options.approximation.enabled && has_volume_limits) {
            std::cerr << "error approximation_limit_conflict: use either "
                         "--approx-volume or legacy --max-*-delta limits\n";
            return EXIT_FAILURE;
        }
        if (options.approximation.enabled) {
            csgify::ApproximationPolicy policy = options.approximation;
            if (operation == csgify::ReplacementOperation::bbox
                || operation == csgify::ReplacementOperation::bbox_cylinder)
                policy.allow_bbox_replacement = true;
            else
                policy.allow_surface_replacement = true;
            const csgify::ApproximationCommandResult replacement =
                csgify::apply_budgeted_replacement_operation(
                    *session, target_id, operation, policy,
                    csgify::ReportIdMode::local);
            action_result = replacement.command;
            if (!action_result.ok) {
                std::cerr << "error " << replacement.budget.code << ": "
                          << replacement.budget.message << '\n';
                return EXIT_FAILURE;
            }
        } else if (has_volume_limits) {
            csgify::ReplacementCommandResult replacement =
                csgify::apply_checked_replacement_operation(
                    *session,
                    target_id,
                    operation,
                    replacement_limits,
                    csgify::ReportIdMode::local);
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
            action_result = csgify::apply_replacement_operation(
                *session, target_id, operation, csgify::ReportIdMode::local);
        }
    } else if (csgify::SplineFitKind kind = csgify::SplineFitKind::none;
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
            action_result = csgify::replace_face_with_fit(
                *session,
                id,
                kind,
                has_max_deviation,
                max_deviation,
                csgify::ReportIdMode::local);
            if (!action_result.ok) {
                const csgify::Diagnostic face_diag =
                    action_result.report.diagnostics.empty()
                        ? csgify::Diagnostic{}
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
        if (options.approximation.enabled) {
            csgify::ApproximationPolicy policy = options.approximation;
            policy.allow_sliver_removal = true;
            const csgify::ApproximationCommandResult removed =
                csgify::remove_residual_with_approximation(
                    *session, target_id, policy,
                    csgify::ReportIdMode::local);
            action_result = removed.command;
            if (!action_result.ok) {
                std::cerr << "error " << removed.budget.code << ": "
                          << removed.budget.message << '\n';
                return EXIT_FAILURE;
            }
        } else {
            action_result = csgify::remove_solid(
                *session, target_id, csgify::ReportIdMode::local);
        }
    } else if (action == "--resolve-overlap") {
        if (has_volume_limits) {
            std::cerr << "error replacement_limit_not_applicable: overlap "
                         "ownership requires --approx-volume\n";
            return EXIT_FAILURE;
        }
        if (!options.approximation.enabled) {
            std::cerr << "error approximation_required: --resolve-overlap "
                         "requires --approx-volume P%\n";
            return EXIT_FAILURE;
        }
        if (overlap_owner_id.empty()) {
            std::cerr << "error missing_overlap_owner: --resolve-overlap "
                         "requires --owner shape-id\n";
            return EXIT_FAILURE;
        }
        csgify::ApproximationPolicy policy = options.approximation;
        policy.allow_overlap_resolution = true;
        const csgify::ApproximationCommandResult resolved =
            csgify::resolve_overlap_with_approximation(
                *session, target_id, overlap_owner_id, policy,
                csgify::ReportIdMode::local);
        action_result = resolved.command;
        if (!action_result.ok) {
            std::cerr << "error " << resolved.budget.code << ": "
                      << resolved.budget.message << '\n';
            if (!report_path.empty())
                write_report_or_stdout(action_result.report, report_path);
            return EXIT_FAILURE;
        }
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
        csgify::HoleFillProposal proposal =
            csgify::propose_hole_fills(*session,
                                        document_id,
                                        hole_fill_options,
                                        csgify::ReportIdMode::local);
        if (!proposal.ok) {
            std::cerr << "error hole_fill_proposal_failed\n";
            return EXIT_FAILURE;
        }
        action_result = csgify::fill_hole(
            *session,
            document_id,
            target_id,
            csgify::ReportIdMode::local);
    } else if (action == "--decompose-planar") {
        if (has_volume_limits) {
            std::cerr << "error replacement_limit_not_applicable: volume delta limits "
                      << "apply to replacement commands, not decompose-planar\n";
            return EXIT_FAILURE;
        }
        action_result = csgify::decompose_planar(
            *session, target_id, csgify::ReportIdMode::local);
    } else if (action == "--shatter") {
        if (has_volume_limits) {
            std::cerr << "error replacement_limit_not_applicable: volume delta limits "
                      << "apply to replacement commands, not shatter\n";
            return EXIT_FAILURE;
        }
        if (max_depth_value < 0.0) {
            std::cerr << "error invalid_max_depth: --max-depth must be non-negative\n";
            return EXIT_FAILURE;
        }
        if (options.approximation.enabled) {
            csgify::ApproximationPolicy policy = options.approximation;
            policy.allow_numerical_shatter_loss = true;
            const csgify::ApproximationCommandResult shattered =
                csgify::shatter_with_approximation(
                    *session, target_id, static_cast<int>(max_depth_value),
                    policy, csgify::ReportIdMode::local);
            action_result = shattered.command;
            if (!action_result.ok) {
                std::cerr << "error " << shattered.budget.code << ": "
                          << shattered.budget.message << '\n';
                return EXIT_FAILURE;
            }
        } else {
            action_result = csgify::shatter(
                *session,
                target_id,
                static_cast<int>(max_depth_value),
                allow_shatter_volume_drift,
                csgify::ReportIdMode::local);
        }
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
        action_result = csgify::decompose_by_face(
            *session, target_id, by_face_id, csgify::ReportIdMode::local);
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
        action_result = csgify::auto_decompose_bsp(
            *session,
            target_id,
            static_cast<int>(max_depth_value),
            decomposition_policy,
            csgify::ReportIdMode::local);
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
        action_result = csgify::apply_decomposition_candidate(
            *session,
            target_id,
            candidate_id,
            csgify::ReportIdMode::local);
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
        if (options.approximation.enabled) {
            csgify::ApproximationPolicy policy = options.approximation;
            policy.allow_surface_replacement = true;
            const csgify::ApproximationCommandResult replaced =
                csgify::apply_budgeted_surface_replacement_candidate(
                    *session, target_id, candidate_id, policy,
                    csgify::ReportIdMode::local);
            action_result = replaced.command;
            if (!action_result.ok) {
                std::cerr << "error " << replaced.budget.code << ": "
                          << replaced.budget.message << '\n';
                return EXIT_FAILURE;
            }
        } else {
            action_result = csgify::apply_surface_replacement_candidate(
                *session, target_id, candidate_id,
                csgify::ReportIdMode::local);
        }
    } else {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (!proposal_report_path.empty()) {
        int proposal_rc = 0;
        if (action == "--fill-hole") {
            proposal_rc = write_hole_fill_proposal_or_stdout(
                csgify::last_hole_fill_proposal(*session),
                proposal_report_path);
        } else if (action == "--apply-surface-candidate") {
            proposal_rc = write_surface_proposal_or_stdout(
                csgify::last_surface_replacement_proposal(*session),
                proposal_report_path);
        } else {
            proposal_rc = write_decomp_proposal_or_stdout(
                csgify::last_decomposition_proposal(*session),
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
        csgify::CommandResult step_result =
            csgify::export_document_step(*session,
                                          document_id,
                                          step_out_path,
                                          csgify::ReportIdMode::local);
        if (!step_result.ok) {
            print_diagnostics(step_result.report);
            return EXIT_FAILURE;
        }
    }

    csgify::CommandResult result =
        csgify::emit_document(*session,
                               document_id,
                               output_path,
                               csgify::ReportIdMode::local);
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
    if (!has_max_deviation) {
        std::cerr << "error max_deviation_required: auto-fit requires an "
                     "explicit --max-deviation approximation budget\n";
        return EXIT_FAILURE;
    }
    if (!std::isfinite(max_deviation) || max_deviation < 0.0) {
        std::cerr << "error invalid_max_deviation: expected a finite "
                     "non-negative value\n";
        return EXIT_FAILURE;
    }

    std::cerr << "csgify: OCCT " << csgify::occt_version()
              << ", Alea " << alea_version() << '\n';

    csgify::ConversionOptions options;
    csgify::SessionPtr session = csgify::create_session(options);
    const csgify::DocumentId document_id = csgify::load_step(*session, input_path);

    csgify::AutoFitCommandResult auto_fit =
        csgify::auto_fit_splines_with_summary(*session,
                                               document_id,
                                               has_max_deviation,
                                               max_deviation,
                                               csgify::ReportIdMode::local);
    std::cerr << "auto-fit: " << auto_fit.summary.applied << "/"
              << auto_fit.summary.spline_faces_seen
              << " spline faces replaced, "
              << auto_fit.summary.skipped_over_budget << " over budget, "
              << auto_fit.summary.skipped_no_fit << " no candidate\n";

    csgify::CommandResult result =
        csgify::emit_document(*session,
                               document_id,
                               output_path,
                               csgify::ReportIdMode::local);
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
    std::string project_path;
    std::string progress_json_path;
    std::string units_override;
    std::string normalize_to;
    double scale_override = 0.0;
    bool disable_overlap_check = false;
    bool allow_overlaps = false;
    std::vector<std::string> analysis_targets;

    OptScanner scan(argc, argv, 3);
    while (!scan.done()) {
        if (scan.take_string("--report", &report_path)) continue;
        if (scan.take_string("--log", &log_path)) continue;
        if (scan.take_string("--project", &project_path)) continue;
        if (scan.take_string("--progress-json", &progress_json_path)) continue;
        if (scan.take_string("--units", &units_override)) continue;
        if (scan.take_string("--normalize-units", &normalize_to)) continue;
        std::string target_id;
        if (scan.take_string("--target", &target_id)) {
            analysis_targets.push_back(target_id);
            continue;
        }
        if (scan.take_double("--scale", &scale_override)) continue;
        if (scan.take_flag("--no-overlap-check")) { disable_overlap_check = true; continue; }
        if (scan.take_flag("--allow-overlaps")) { allow_overlaps = true; continue; }
        std::cerr << "error unrecognized_option: " << scan.peek() << '\n';
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    std::cerr << "csgify: OCCT " << csgify::occt_version()
              << ", Alea " << alea_version() << '\n';

    csgify::ConversionOptions options;
    options.unit_override = units_override;
    options.normalize_to_unit = normalize_to;
    options.unit_scale_to_mm_override = scale_override;
    options.analysis_target_ids = analysis_targets;
    if (!analysis_targets.empty()) options.check_overlaps = false;
    if (disable_overlap_check) options.check_overlaps = false;
    if (allow_overlaps) options.allow_overlaps = true;
    csgify::SessionPtr session = csgify::create_session(options);
    if (!log_path.empty()) {
        try {
            csgify::attach_decision_log(*session, log_path, input_path);
        } catch (const std::exception& e) {
            std::cerr << "error decision_log_open_failed: " << e.what() << '\n';
            return EXIT_FAILURE;
        }
    }
    const csgify::DocumentId document_id = csgify::load_step(*session, input_path);
    std::ofstream progress_file;
    std::ostream* progress_out = nullptr;
    if (!progress_json_path.empty()) {
        if (progress_json_path == "-") {
            progress_out = &std::cout;
        } else {
            progress_file.open(progress_json_path, std::ios::trunc);
            if (!progress_file) {
                std::cerr << "error progress_json_open_failed: "
                          << progress_json_path << '\n';
                return EXIT_FAILURE;
            }
            progress_out = &progress_file;
        }
    }
    const csgify::JobProgressCallback progress = progress_out
        ? csgify::JobProgressCallback([progress_out](
              const csgify::JobProgressEvent& event) {
              *progress_out << csgify::job_progress_event_json(event) << '\n';
              progress_out->flush();
              return static_cast<bool>(*progress_out);
          })
        : csgify::JobProgressCallback{};
    csgify::CommandResult inspect =
        csgify::analyze(*session, document_id,
                        csgify::ReportIdMode::local, progress);
    csgify::ConversionReport report = inspect.report;
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
    if (!project_path.empty()) {
        std::string project_error;
        csgify::ProjectStore project(project_path);
        const csgify::ProjectSnapshot snapshot =
            csgify::project_snapshot_from_report(report);
        if (!project.open(&project_error)
            || !project.persist_snapshot(snapshot, &project_error)
            || !project.replace_spatial_entries(
                snapshot.source_digest,
                csgify::spatial_entries_from_report(report),
                &project_error)) {
            std::cerr << "error project_persist_failed: " << project_error << '\n';
            return EXIT_FAILURE;
        }
        csgify::DefinitionCacheSyncResult cache_sync;
        if (!project.sync_definition_recognition_cache(
                report, options, &cache_sync, &project_error)) {
            std::cerr << "error definition_cache_sync_failed: "
                      << project_error << '\n';
            return EXIT_FAILURE;
        }
        std::cerr << "Definition recognition cache: " << cache_sync.hits
                  << " hit(s), " << cache_sync.misses << " miss(es), "
                  << cache_sync.published << " published, "
                  << cache_sync.skipped_unverified << " skipped.\n";
        if (!report_path.empty() && report_path != "-") {
            csgify::ArtifactRecord artifact;
            if (!project.artifacts().publish_file(
                    "inspection-report", "json", report_path,
                    &artifact, &project_error)
                || !project.record_artifact(artifact, &project_error)) {
                std::cerr << "error project_artifact_failed: "
                          << project_error << '\n';
                return EXIT_FAILURE;
            }
        }
    }
    return report.ok() ? EXIT_SUCCESS : EXIT_FAILURE;
}

int run_handoff_command(int argc, char** argv) {
    if (wants_help(argc, argv)) {
        return print_verb_help_and_exit(argv[0], "handoff");
    }
    if (argc < 3) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    const std::string input_path = argv[2];
    std::string output_path;
    std::string project_path;
    std::string report_path;
    OptScanner scan(argc, argv, 3);
    while (!scan.done()) {
        if (scan.take_string("--out", &output_path)) continue;
        if (scan.take_string("--project", &project_path)) continue;
        if (scan.take_string("--report", &report_path)) continue;
        std::cerr << "error unrecognized_option: " << scan.peek() << '\n';
        return EXIT_FAILURE;
    }
    if (output_path.empty()) {
        std::cerr << "error handoff_output_required: use --out PATH|-\n";
        return EXIT_FAILURE;
    }

    std::cerr << "stage alea_handoff_load_and_convert: started\n";
    csgify::ConversionOptions options;
    csgify::ConversionResult conversion =
        csgify::convert_step_file(input_path, options);
    if (!project_path.empty()) {
        csgify::ProjectStore project(project_path);
        csgify::ArtifactRecord geometry_artifact;
        csgify::ArtifactRecord manifest_artifact;
        std::string project_error;
        if (!project.open(&project_error)) {
            std::cerr << "error alea_handoff_project_open_failed: "
                      << project_error << '\n';
            return EXIT_FAILURE;
        }
        const std::filesystem::path temporary_geometry =
            project.artifacts().root() / "tmp" /
            ("alea-handoff-" + std::to_string(
                static_cast<unsigned long long>(
                    std::chrono::steady_clock::now().time_since_epoch().count()))
             + ".i");
        const bool geometry_exported =
            csgify::export_mcnp(conversion.system.get(),
                                temporary_geometry.string()) == 0;
        const bool geometry_published = geometry_exported
            && project.artifacts().publish_file(
                "alea_geometry_mcnp", "i", temporary_geometry,
                &geometry_artifact, &project_error)
            && project.record_artifact(geometry_artifact, &project_error)
            && csgify::add_alea_handoff_chunk(
                conversion.handoff, geometry_artifact, &project_error);
        std::remove(temporary_geometry.string().c_str());
        if (!geometry_published
            || !csgify::publish_alea_handoff_manifest(
                project.artifacts(), conversion.handoff, &manifest_artifact,
                &project_error)
            || !project.record_artifact(manifest_artifact, &project_error)) {
            std::cerr << "error alea_handoff_project_publish_failed: "
                      << project_error << '\n';
            return EXIT_FAILURE;
        }
        conversion.report.alea_handoff.contract_digest =
            conversion.handoff.contract_digest;
        conversion.report.alea_handoff.chunk_count =
            conversion.handoff.chunks.size();
        std::cerr << "stage alea_handoff_project_publish: completed manifest="
                  << manifest_artifact.digest << ", geometry="
                  << geometry_artifact.digest << '\n';
    }
    std::string validation_error;
    const bool valid = csgify::validate_alea_handoff_manifest(
        conversion.handoff, &validation_error);
    if (!valid) {
        std::cerr << "error alea_handoff_invalid: " << validation_error << '\n';
    }
    const std::string serialized =
        csgify::serialize_alea_handoff_manifest(conversion.handoff);
    if (output_path == "-") {
        std::cout << serialized;
    } else {
        std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
        output << serialized;
        if (!output) {
            std::cerr << "error alea_handoff_write_failed: " << output_path
                      << '\n';
            return EXIT_FAILURE;
        }
    }
    if (!report_path.empty()
        && write_report_or_stdout(conversion.report, report_path) != 0) {
        std::cerr << "error report_write_failed: " << report_path << '\n';
        return EXIT_FAILURE;
    }
    std::cerr << "stage alea_handoff: completed cells="
              << conversion.handoff.cells.size()
              << ", surfaces=" << conversion.handoff.surfaces.size()
              << ", universes=" << conversion.handoff.universes.size()
              << ", sidecar_entities=" << conversion.handoff.sidecar.size()
              << ", contract_digest=" << conversion.handoff.contract_digest
              << '\n';
    return valid && conversion.report.ok() ? EXIT_SUCCESS : EXIT_FAILURE;
}

int run_voids_command(int argc, char** argv) {
    if (wants_help(argc, argv)) {
        return print_verb_help_and_exit(argv[0], "voids");
    }
    if (argc < 3) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    const std::string input_path = argv[2];
    std::string output_path;
    std::string mcnp_path;
    std::string project_path;
    std::string domain_id = "domain:modeled";
    std::string bounds_text;
    double margin = 0.05;
    std::size_t max_depth = 8;
    std::size_t workers = 0;
    std::size_t scratch_bytes = 512ULL * 1024ULL * 1024ULL;
    std::size_t samples = 7;
    double min_size = 0.0;
    bool merge = true;
    bool graveyard = true;
    bool open_review = false;
    OptScanner scan(argc, argv, 3);
    while (!scan.done()) {
        if (scan.take_string("--out", &output_path)) continue;
        if (scan.take_string("--out-mcnp", &mcnp_path)) continue;
        if (scan.take_string("--project", &project_path)) continue;
        if (scan.take_string("--domain-id", &domain_id)) continue;
        if (scan.take_string("--bounds", &bounds_text)) continue;
        if (scan.take_double("--margin", &margin)) continue;
        if (scan.take_size("--max-depth", &max_depth)) continue;
        if (scan.take_double("--min-size", &min_size)) continue;
        if (scan.take_size("--workers", &workers)) continue;
        if (scan.take_size("--scratch-bytes", &scratch_bytes)) continue;
        if (scan.take_size("--samples", &samples)) continue;
        if (scan.take_flag("--no-merge")) { merge = false; continue; }
        if (scan.take_flag("--no-graveyard")) { graveyard = false; continue; }
        if (scan.take_flag("--review")) { open_review = true; continue; }
        std::cerr << "error unrecognized_option: " << scan.peek() << '\n';
        return EXIT_FAILURE;
    }
    if (output_path.empty()) {
        std::cerr << "error coverage_output_required: use --out PATH|-\n";
        return EXIT_FAILURE;
    }
    if (margin < 0.0 || max_depth > static_cast<std::size_t>(
            std::numeric_limits<int>::max())
        || samples > 1000) {
        std::cerr << "error invalid_void_policy: invalid margin, depth, workers, or samples\n";
        return EXIT_FAILURE;
    }

    csgify::AleaVoidPolicy policy;
    policy.max_depth = static_cast<int>(max_depth);
    policy.min_size = min_size;
    policy.requested_workers = workers;
    policy.max_parallel_scratch_bytes = scratch_bytes;
    policy.coverage_samples_per_axis = samples;
    policy.merge = merge;
    policy.add_graveyard = graveyard;

    std::cerr << "stage alea_void_load_and_convert: started\n";
    csgify::ConversionOptions conversion_options;
    csgify::CadModel source_model =
        csgify::load_step_model(input_path, conversion_options);
    csgify::ConversionReport source_analysis =
        csgify::analyze_model(source_model, conversion_options);
    csgify::ConversionResult conversion =
        csgify::convert_model(source_model, conversion_options);
    if (conversion.report.solids_seen == 0
        || conversion.report.solids_converted
               != conversion.report.solids_seen) {
        std::cerr << "error alea_void_input_not_emit_ready: converted "
                  << conversion.report.solids_converted << '/'
                  << conversion.report.solids_seen
                  << " solids; simplify/decompose the STEP before void generation\n";
        return EXIT_FAILURE;
    }
    csgify::AleaVoidDomain domain =
        csgify::default_alea_void_domain(source_analysis, margin);
    if (!domain.bounds.valid) {
        domain = csgify::default_alea_void_domain(conversion.handoff, margin);
    }
    domain.domain_id = domain_id;
    if (!bounds_text.empty()) {
        std::replace(bounds_text.begin(), bounds_text.end(), ',', ' ');
        std::istringstream values(bounds_text);
        double trailing = 0.0;
        if (!(values >> domain.bounds.xmin >> domain.bounds.ymin
                    >> domain.bounds.zmin >> domain.bounds.xmax
                    >> domain.bounds.ymax >> domain.bounds.zmax)
            || (values >> trailing)
            || !(domain.bounds.xmin < domain.bounds.xmax)
            || !(domain.bounds.ymin < domain.bounds.ymax)
            || !(domain.bounds.zmin < domain.bounds.zmax)) {
            std::cerr << "error alea_void_bounds_invalid: expected finite "
                         "xmin,ymin,zmin,xmax,ymax,zmax\n";
            return EXIT_FAILURE;
        }
        domain.bounds.valid = std::isfinite(domain.bounds.xmin)
            && std::isfinite(domain.bounds.ymin)
            && std::isfinite(domain.bounds.zmin)
            && std::isfinite(domain.bounds.xmax)
            && std::isfinite(domain.bounds.ymax)
            && std::isfinite(domain.bounds.zmax);
    }
    if (!domain.bounds.valid) {
        std::cerr << "error alea_void_domain_missing: no finite analyzed solid bounds\n";
        return EXIT_FAILURE;
    }
    const std::string domain_digest = csgify::alea_void_domain_digest(domain);
    const std::string policy_digest = csgify::alea_void_policy_digest(policy);
    const std::string cache_key = "alea-void-v1:" +
        conversion.handoff.contract_digest + ":" + domain_digest + ":" +
        policy_digest;

    std::unique_ptr<csgify::ProjectStore> project;
    std::optional<csgify::ClaimedProjectJob> claimed;
    std::string project_error;
    if (!project_path.empty()) {
        project = std::make_unique<csgify::ProjectStore>(project_path);
        if (!project->open(&project_error)) {
            std::cerr << "error alea_void_project_open_failed: "
                      << project_error << '\n';
            return EXIT_FAILURE;
        }
        const auto cached = project->find_conversion_cache(cache_key, &project_error);
        if (!project_error.empty()) {
            std::cerr << "error alea_void_cache_query_failed: "
                      << project_error << '\n';
            return EXIT_FAILURE;
        }
        if (cached && project->artifacts().validate(cached->artifact, &project_error)) {
            const auto certificate_artifact = project->find_artifact(
                cached->certificate_digest, "alea_coverage_certificate",
                &project_error);
            if (certificate_artifact
                && project->artifacts().validate(*certificate_artifact, &project_error)) {
                const std::filesystem::path certificate_source =
                    project->artifacts().absolute_path(*certificate_artifact);
                std::ifstream input(certificate_source, std::ios::binary);
                const std::string certificate_text(
                    (std::istreambuf_iterator<char>(input)),
                    std::istreambuf_iterator<char>());
                if (!input && certificate_text.empty()) {
                    std::cerr << "error alea_void_cached_certificate_read_failed\n";
                    return EXIT_FAILURE;
                }
                if (output_path == "-") {
                    std::cout << certificate_text;
                } else {
                    std::filesystem::copy_file(
                        certificate_source, output_path,
                        std::filesystem::copy_options::overwrite_existing);
                }
                if (!mcnp_path.empty()) {
                    std::filesystem::copy_file(
                        project->artifacts().absolute_path(cached->artifact),
                        mcnp_path,
                        std::filesystem::copy_options::overwrite_existing);
                }
                const bool certified = certificate_text.find(
                    "\"status\": \"certified\"") != std::string::npos;
                std::cerr << "stage alea_void_resume: reused certificate="
                          << cached->certificate_digest << ", geometry="
                          << cached->artifact.digest << '\n';
                if (open_review) {
#if CSGIFY_HAVE_QT6_VIEWER
                    const csgify::AleaCoverageCertificate parsed =
                        csgify::parse_alea_coverage_certificate_json(
                            certificate_text);
                    const int review_result = csgify::run_qt_coverage_review(
                        parsed, certificate_source.string());
                    if (review_result != 0) return review_result;
#else
                    std::cerr << "error viewer_not_built: use `csgify coverage status`\n";
                    return EXIT_FAILURE;
#endif
                }
                return certified ? EXIT_SUCCESS : EXIT_FAILURE;
            }
        }
        project_error.clear();
        csgify::ProjectJobSpec job;
        job.job_id = cache_key;
        job.target_id = domain.domain_id;
        job.operation = "alea_void_generate_v1";
        job.input_digest = conversion.handoff.contract_digest;
        job.policy_digest = policy_digest;
        job.max_attempts = 2;
        job.limits.memory_bytes = scratch_bytes;
        if (!project->enqueue_job(job, {}, &project_error)) {
            std::cerr << "error alea_void_job_enqueue_failed: "
                      << project_error << '\n';
            return EXIT_FAILURE;
        }
        claimed = project->claim_next_job(
            "csgify-void-cli",
            static_cast<std::int64_t>(std::time(nullptr)), &project_error);
        if (!claimed || claimed->spec.job_id != job.job_id) {
            std::cerr << "error alea_void_job_claim_failed: "
                      << (project_error.empty() ? "job unavailable" : project_error)
                      << '\n';
            return EXIT_FAILURE;
        }
    }

    const auto project_run_start = std::chrono::steady_clock::now();
    std::unique_ptr<csgify::ProjectJobProgressSink> project_progress;
    if (project && claimed) {
        project_progress = std::make_unique<csgify::ProjectJobProgressSink>(
            *project, *claimed);
        if (!project_progress->ready()) {
            std::cerr << "error alea_void_progress_sink_failed: "
                      << project_progress->last_error() << '\n';
            std::string ignored;
            project->fail_job(claimed->spec.job_id, claimed->worker_id,
                              "progress_sink_failed",
                              project_progress->last_error(), true,
                              static_cast<std::int64_t>(std::time(nullptr)),
                              &ignored);
            return EXIT_FAILURE;
        }
    }
    const auto project_event = [&](csgify::JobPhase phase,
                                   const std::string& code,
                                   csgify::JobOutcome outcome =
                                       csgify::JobOutcome::none,
                                   const std::string& artifact_digest = {}) {
        if (!project_progress) return true;
        csgify::JobProgressEvent event;
        event.operation = csgify::JobOperation::alea_void_generation;
        event.phase = phase;
        event.outcome = outcome;
        event.message_code = code;
        event.target_id = domain.domain_id;
        event.elapsed_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - project_run_start).count();
        event.artifact_id = artifact_digest;
        return project_progress->consume(event, artifact_digest);
    };
    const auto fail_project_job = [&](const std::string& code,
                                      const std::string& message,
                                      bool retryable) {
        if (!project || !claimed) return;
        if (project_progress && !project_progress->terminal_seen()) {
            project_event(csgify::JobPhase::finished, code,
                          code == "job_cancelled"
                              ? csgify::JobOutcome::cancelled
                              : csgify::JobOutcome::failed);
        }
        std::string ignored;
        project->fail_job(claimed->spec.job_id, claimed->worker_id,
                          code, message, retryable,
                          static_cast<std::int64_t>(std::time(nullptr)),
                          &ignored);
    };

    if (!project_event(csgify::JobPhase::certifying_solids,
                       "alea_void_generation_started")) {
        const bool cancelled = project_progress
            && project_progress->cancellation_seen();
        fail_project_job(cancelled ? "job_cancelled" : "progress_sink_failed",
                         cancelled ? "Cancellation requested before void generation"
                                   : project_progress->last_error(),
                         !cancelled);
        return EXIT_FAILURE;
    }

    std::cerr << "stage alea_void_generate: started domain=" << domain.domain_id
              << ", max_depth=" << policy.max_depth
              << ", workers=" << policy.requested_workers
              << ", scratch_bytes=" << policy.max_parallel_scratch_bytes << '\n';
    csgify::AleaCoverageCertificate certificate;
    std::string void_error;
    std::mutex project_monitor_mutex;
    std::condition_variable project_monitor_cv;
    bool project_monitor_done = false;
    std::atomic_bool project_monitor_cancelled{false};
    std::atomic_bool project_monitor_failed{false};
    std::string project_monitor_error;
    std::thread project_monitor;
    if (project && claimed) {
        const std::filesystem::path monitor_root = project->root();
        const std::string monitor_job_id = claimed->spec.job_id;
        const std::string monitor_worker_id = claimed->worker_id;
        project_monitor = std::thread([&, monitor_root, monitor_job_id,
                                       monitor_worker_id]() {
            csgify::ProjectStore monitor_store(monitor_root);
            std::string monitor_error;
            if (!monitor_store.open(&monitor_error)) {
                project_monitor_error = monitor_error;
                project_monitor_failed.store(true);
                return;
            }
            std::unique_lock<std::mutex> lock(project_monitor_mutex);
            while (!project_monitor_cv.wait_for(
                lock, std::chrono::seconds(5),
                [&]() { return project_monitor_done; })) {
                lock.unlock();
                const auto timestamp =
                    static_cast<std::int64_t>(std::time(nullptr));
                bool cancelled = false;
                if (!monitor_store.heartbeat_job(
                        monitor_job_id, monitor_worker_id, timestamp,
                        &monitor_error)
                    || !monitor_store.job_cancellation_requested(
                        monitor_job_id, monitor_worker_id, &cancelled,
                        &monitor_error)) {
                    project_monitor_error = monitor_error;
                    project_monitor_failed.store(true);
                    return;
                }
                if (cancelled) project_monitor_cancelled.store(true);
                lock.lock();
            }
        });
    }
    const bool generated = csgify::generate_alea_domain_voids(
            conversion.system.get(), source_analysis, conversion.handoff,
            domain, policy, &certificate, &void_error);
    {
        std::lock_guard<std::mutex> lock(project_monitor_mutex);
        project_monitor_done = true;
    }
    project_monitor_cv.notify_one();
    if (project_monitor.joinable()) project_monitor.join();
    if (project_monitor_failed.load()) {
        fail_project_job("project_heartbeat_failed", project_monitor_error, true);
        std::cerr << "error project_heartbeat_failed: "
                  << project_monitor_error << '\n';
        return EXIT_FAILURE;
    }
    if (project_monitor_cancelled.load()) {
        fail_project_job("job_cancelled",
                         "Cancellation requested during void generation",
                         false);
        std::cerr << "error job_cancelled: void generation result discarded\n";
        return EXIT_FAILURE;
    }
    if (!generated) {
        fail_project_job("alea_void_generation_failed", void_error, true);
        std::cerr << "error alea_void_generation_failed: " << void_error << '\n';
        return EXIT_FAILURE;
    }
    if (!project_event(csgify::JobPhase::terminal_verification,
                       "alea_void_validating_certificate")) {
        const bool cancelled = project_progress
            && project_progress->cancellation_seen();
        fail_project_job(cancelled ? "job_cancelled" : "progress_sink_failed",
                         cancelled ? "Cancellation requested after void generation"
                                   : project_progress->last_error(),
                         !cancelled);
        return EXIT_FAILURE;
    }
    std::string validation_error;
    if (!csgify::validate_alea_coverage_certificate(
            certificate, &validation_error)) {
        std::cerr << "error alea_coverage_certificate_invalid: "
                  << validation_error << '\n';
        fail_project_job("alea_coverage_certificate_invalid",
                         validation_error, false);
        return EXIT_FAILURE;
    }
    const std::string certificate_text =
        csgify::serialize_alea_coverage_certificate(certificate);
    if (output_path == "-") {
        std::cout << certificate_text;
    } else {
        std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
        output << certificate_text;
        if (!output) {
            std::cerr << "error alea_coverage_write_failed: " << output_path << '\n';
            fail_project_job("alea_coverage_write_failed",
                             "Could not write " + output_path, true);
            return EXIT_FAILURE;
        }
    }
    if (!mcnp_path.empty()
        && csgify::export_mcnp(conversion.system.get(), mcnp_path) != 0) {
        std::cerr << "error alea_void_mcnp_export_failed: " << alea_error() << '\n';
        fail_project_job("alea_void_mcnp_export_failed", alea_error(), true);
        return EXIT_FAILURE;
    }

    if (project && claimed) {
        if (!project_event(csgify::JobPhase::committing,
                           "alea_void_publishing_artifacts")) {
            const bool cancelled = project_progress->cancellation_seen();
            fail_project_job(cancelled ? "job_cancelled" : "progress_sink_failed",
                             cancelled ? "Cancellation requested before artifact publication"
                                       : project_progress->last_error(),
                             !cancelled);
            return EXIT_FAILURE;
        }
        const std::filesystem::path temporary_geometry =
            project->artifacts().root() / "tmp" /
            ("alea-void-" + std::to_string(
                static_cast<unsigned long long>(
                    std::chrono::steady_clock::now().time_since_epoch().count()))
             + ".i");
        csgify::ArtifactRecord geometry_artifact;
        csgify::ArtifactRecord certificate_artifact;
        const bool published =
            csgify::export_mcnp(conversion.system.get(),
                                temporary_geometry.string()) == 0
            && project->artifacts().publish_file(
                "alea_void_geometry_mcnp", "i", temporary_geometry,
                &geometry_artifact, &project_error)
            && project->record_artifact(geometry_artifact, &project_error)
            && csgify::publish_alea_coverage_certificate(
                project->artifacts(), certificate, &certificate_artifact,
                &project_error)
            && project->record_artifact(certificate_artifact, &project_error);
        std::remove(temporary_geometry.string().c_str());
        if (!published) {
            std::cerr << "error alea_void_project_publish_failed: "
                      << project_error << '\n';
            fail_project_job("alea_void_project_publish_failed",
                             project_error, true);
            return EXIT_FAILURE;
        }
        csgify::JobProgressEvent terminal_event;
        terminal_event.operation = csgify::JobOperation::alea_void_generation;
        terminal_event.phase = csgify::JobPhase::finished;
        terminal_event.outcome = csgify::JobOutcome::committed;
        terminal_event.message_code = "alea_void_generation_finished";
        terminal_event.target_id = domain.domain_id;
        terminal_event.elapsed_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - project_run_start).count();
        if (!project_progress->complete(
                terminal_event, geometry_artifact,
                certificate_artifact.digest)) {
            project_error = project_progress->last_error();
            std::cerr << "error alea_void_project_checkpoint_failed: "
                      << project_error << '\n';
            const bool cancelled = project_progress->cancellation_seen();
            fail_project_job(cancelled ? "job_cancelled"
                                       : "alea_void_project_checkpoint_failed",
                             cancelled ? "Cancellation won the final commit race"
                                       : project_error,
                             !cancelled);
            return EXIT_FAILURE;
        }
        csgify::ConversionCacheEntry cache;
        cache.cache_key = cache_key;
        cache.geometry_digest = conversion.handoff.contract_digest;
        cache.policy_digest = policy_digest;
        cache.artifact = geometry_artifact;
        cache.certificate_digest = certificate_artifact.digest;
        if (!project->record_conversion_cache(cache, &project_error)) {
            std::cerr << "warning alea_void_cache_checkpoint_failed: "
                      << project_error << '\n';
        }
        std::cerr << "stage alea_void_checkpoint: completed certificate="
                  << certificate_artifact.digest << ", geometry="
                  << geometry_artifact.digest << '\n';
    }
    std::cerr << "stage alea_void_generate: completed status="
              << csgify::alea_coverage_status_name(certificate.status)
              << ", void_cells=" << certificate.intentional_void_cell_count
              << ", coverage_samples=" << certificate.coverage.sample_count
              << ", workers=" << certificate.execution.actual_workers << '\n';
    if (open_review) {
#if CSGIFY_HAVE_QT6_VIEWER
        const int review_result = csgify::run_qt_coverage_review(
            certificate, output_path == "-" ? std::string("generated certificate")
                                             : output_path);
        if (review_result != 0) return review_result;
#else
        std::cerr << "error viewer_not_built: use `csgify coverage status`\n";
        return EXIT_FAILURE;
#endif
    }
    return certificate.status == csgify::AleaCoverageCertificateStatus::certified
        ? EXIT_SUCCESS : EXIT_FAILURE;
}

int run_project_command(int argc, char** argv) {
    if (wants_help(argc, argv)) return print_verb_help_and_exit(argv[0], "project");
    if (argc >= 3 && std::string(argv[2]) == "review"
        && (argc == 3 || (argc == 4 && std::string(argv[3]) == "--smoke-test"))) {
#if CSGIFY_HAVE_QT6_VIEWER
        return csgify::run_qt_project_launcher(argc == 4);
#else
        std::cerr << "error qt_viewer_not_built: project review requires Qt 6 Widgets\n";
        return EXIT_FAILURE;
#endif
    }
    if (argc < 4) {
        std::cerr << "error project_usage: expected init|status|events|cancel DIR or review [DIR]\n";
        return EXIT_FAILURE;
    }
    const std::string action = argv[2];
    const std::string project_path = argv[3];
    std::string source_path;
    std::string coverage_path;
    std::string queue_path;
    std::size_t limit = 20;
    std::size_t after_sequence = 0;
    std::string job_id;
    bool smoke_test = false;
    OptScanner scan(argc, argv, 4);
    while (!scan.done()) {
        if (scan.take_string("--source", &source_path)) continue;
        if (scan.take_string("--coverage", &coverage_path)) continue;
        if (scan.take_string("--queue", &queue_path)) continue;
        if (scan.take_size("--limit", &limit)) continue;
        if (scan.take_size("--after", &after_sequence)) continue;
        if (scan.take_string("--job", &job_id)) continue;
        if (scan.take_flag("--smoke-test")) { smoke_test = true; continue; }
        std::cerr << "error unrecognized_option: " << scan.peek() << '\n';
        return EXIT_FAILURE;
    }
    if (action != "init" && action != "status" && action != "events" && action != "cancel"
        && action != "review") {
        std::cerr << "error project_action_invalid: expected init, status, events, cancel, or review\n";
        return EXIT_FAILURE;
    }
    csgify::ProjectStore project(project_path);
    std::string error;
    if (action == "init") {
        if (argc != 4) {
            std::cerr << "error project_usage: init accepts only DIR\n";
            return EXIT_FAILURE;
        }
        if (!project.open(&error)) {
            std::cerr << "error project_init_failed: " << error << '\n';
            return EXIT_FAILURE;
        }
        const std::filesystem::path policy_path =
            project.root() / "project-policy.json";
        std::error_code policy_error;
        const bool policy_exists = std::filesystem::exists(
            policy_path, policy_error);
        if (policy_error) {
            std::cerr << "error project_policy_probe_failed: "
                      << policy_error.message() << '\n';
            return EXIT_FAILURE;
        }
        if (policy_exists) {
            if (!std::filesystem::is_regular_file(policy_path, policy_error)) {
                std::cerr << "error project_policy_invalid: expected regular file "
                          << policy_path.string() << '\n';
                return EXIT_FAILURE;
            }
            try {
                (void)csgify::read_autonomy_project_policy_json(
                    policy_path.string());
            } catch (const std::exception& e) {
                std::cerr << "error project_policy_invalid: " << e.what()
                          << '\n';
                return EXIT_FAILURE;
            }
        } else {
            std::string project_id =
                project.root().lexically_normal().filename().string();
            if (project_id.empty()) project_id = "csgify-project";
            if (csgify::write_default_autonomy_project_policy_json(
                    policy_path.string(), project_id) != 0) {
                std::cerr << "error project_policy_create_failed: "
                          << policy_path.string() << '\n';
                return EXIT_FAILURE;
            }
        }
        std::cout << "project_initialized " << project.root()
                  << " schema=" << project.schema_version(&error)
                  << " policy=" << policy_path.string() << '\n';
        return error.empty() ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    std::error_code project_path_error;
    const bool initialized = std::filesystem::is_regular_file(
        project.database_path(), project_path_error);
    if (project_path_error || !initialized) {
        std::cerr << "error project_open_failed: project is not initialized; run `"
                  << argv[0] << " project init " << project_path << "`\n";
        return EXIT_FAILURE;
    }
    const bool read_only = action == "status" || action == "events";
    if (!(read_only ? project.open_read_only(&error) : project.open(&error))) {
        std::cerr << "error project_open_failed: " << error << '\n';
        return EXIT_FAILURE;
    }
    if (action == "events") {
        if (job_id.empty()) {
            std::cerr << "error project_job_required: events requires --job JOB_ID\n";
            return EXIT_FAILURE;
        }
        const auto events = project.list_job_events(
            job_id, static_cast<std::uint64_t>(after_sequence), limit, &error);
        if (!error.empty()) {
            std::cerr << "error project_job_events_failed: " << error << '\n';
            return EXIT_FAILURE;
        }
        for (const auto& event : events)
            std::cout << csgify::job_progress_event_json(event.event) << '\n';
        return EXIT_SUCCESS;
    }
    if (action == "cancel") {
        if (job_id.empty()) {
            std::cerr << "error project_job_required: cancel requires --job JOB_ID\n";
            return EXIT_FAILURE;
        }
        const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
        if (!project.request_job_cancellation(job_id, now, &error)) {
            std::cerr << "error project_job_cancel_failed: " << error << '\n';
            return EXIT_FAILURE;
        }
        std::cout << "cancel_requested " << job_id << '\n';
        return EXIT_SUCCESS;
    }
    if (action == "status") {
        const csgify::ProjectCounts counts = project.counts(&error);
        const csgify::ProjectJobCounts jobs = project.job_counts(&error);
        if (!error.empty()) {
            std::cerr << "error project_query_failed: " << error << '\n';
            return EXIT_FAILURE;
        }
        std::cout << "Project: " << project.root() << '\n'
                  << "Schema: " << project.schema_version(&error) << '\n'
                  << "Assembly: sources=" << counts.sources
                  << ", definitions=" << counts.component_definitions
                  << ", instances=" << counts.component_instances
                  << ", materials=" << counts.materials << '\n'
                  << "Cache: artifacts=" << counts.artifacts
                  << ", conversions=" << counts.conversion_cache_entries
                  << ", spatial=" << counts.spatial_entities
                  << ", autonomy_requests=" << counts.autonomy_requests << '\n'
                  << "Jobs: queued=" << jobs.queued << ", running=" << jobs.running
                  << ", succeeded=" << jobs.succeeded << ", failed=" << jobs.failed
                  << ", cancelled=" << jobs.cancelled << '\n';
        for (const auto& source : project.list_sources(limit, 0, &error))
            std::cout << "source " << source.source_digest << ' ' << source.source_path << '\n';
        for (const auto& job : project.list_jobs(limit, 0, {}, &error))
            std::cout << "job " << job.job_id << ' '
                      << csgify::project_job_state_name(job.state) << ' '
                      << job.operation << ' ' << job.target_id << " attempt="
                      << job.attempt << '/' << job.max_attempts
                      << " phase=" << (job.latest_sequence
                          ? csgify::job_phase_name(job.latest_phase) : "none")
                      << " sequence=" << job.latest_sequence
                      << " progress=" << job.completed << '/'
                      << (job.total_known ? std::to_string(job.total)
                                          : std::string("unknown"))
                      << " heartbeat=" << job.heartbeat_at << '\n';
        return error.empty() ? EXIT_SUCCESS : EXIT_FAILURE;
    }
#if CSGIFY_HAVE_QT6_VIEWER
    return csgify::run_qt_project_workspace(
        project_path, source_path, coverage_path, queue_path, smoke_test);
#else
    std::cerr << "error qt_viewer_not_built: project review requires Qt 6 Widgets\n";
    return EXIT_FAILURE;
#endif
}

int run_coverage_command(int argc, char** argv) {
    if (wants_help(argc, argv)) {
        return print_verb_help_and_exit(argv[0], "coverage");
    }
    if (argc < 4) {
        std::cerr << "error coverage_usage: expected status|review|queue coverage.json\n";
        return EXIT_FAILURE;
    }
    const std::string action = argv[2];
    if (action != "status" && action != "review" && action != "queue") {
        std::cerr << "error coverage_action_invalid: expected status, review, or queue\n";
        return EXIT_FAILURE;
    }
    if (action != "queue" && argc != 4) {
        std::cerr << "error coverage_usage: status/review accept only coverage.json\n";
        return EXIT_FAILURE;
    }
    csgify::AleaCoverageCertificate certificate;
    try {
        certificate = csgify::read_alea_coverage_certificate_json(argv[3]);
    } catch (const std::exception& exception) {
        std::cerr << "error coverage_certificate_invalid: "
                  << exception.what() << '\n';
        return EXIT_FAILURE;
    }
    const auto findings = csgify::alea_coverage_review_items(certificate);
    const bool certified = certificate.status
        == csgify::AleaCoverageCertificateStatus::certified;
    if (action == "queue") {
        std::string output_path;
        std::string source_path;
        std::string resume_path;
        OptScanner scan(argc, argv, 4);
        while (!scan.done()) {
            if (scan.take_string("--out", &output_path)) continue;
            if (scan.take_string("--source", &source_path)) continue;
            if (scan.take_string("--resume", &resume_path)) continue;
            std::cerr << "error unrecognized_option: " << scan.peek() << '\n';
            return EXIT_FAILURE;
        }
        if (output_path.empty()) {
            std::cerr << "error coverage_queue_output_required: use --out queue.json|-\n";
            return EXIT_FAILURE;
        }
        csgify::AutonomyQueue queue;
        if (!resume_path.empty()) {
            try {
                queue = csgify::read_autonomy_queue_json(resume_path);
            } catch (const std::exception& exception) {
                std::cerr << "error coverage_queue_resume_invalid: "
                          << exception.what() << '\n';
                return EXIT_FAILURE;
            }
            if (!source_path.empty()) queue.source_path = source_path;
            csgify::reconcile_coverage_autonomy_queue(
                queue, certificate, argv[3]);
        } else {
            csgify::AutonomyPolicy policy;
            policy.mode = csgify::AutonomyMode::propose;
            queue = csgify::make_coverage_autonomy_queue(
                certificate, argv[3], source_path, policy);
        }
        if (csgify::write_autonomy_queue_json(queue, output_path) != 0) {
            std::cerr << "error coverage_queue_write_failed: " << output_path << '\n';
            return EXIT_FAILURE;
        }
        std::cerr << "stage coverage_queue: completed blockers="
                  << queue.human_review_count << ", certificate="
                  << certificate.certificate_digest << '\n';
        return certified ? EXIT_SUCCESS : 2;
    }
    if (action == "status") {
        std::cout << "Coverage domain: " << certificate.domain.domain_id << '\n'
                  << "Status: "
                  << csgify::alea_coverage_status_name(certificate.status) << '\n'
                  << "Cells: material=" << certificate.material_cell_count
                  << ", void=" << certificate.intentional_void_cell_count
                  << ", graveyard=" << certificate.graveyard_cell_count << '\n'
                  << "Ownership: samples=" << certificate.coverage.sample_count
                  << ", unique=" << certificate.coverage.unique_count
                  << ", gaps=" << certificate.coverage.gap_count
                  << ", overlaps=" << certificate.coverage.overlap_count
                  << ", undefined_fills="
                  << certificate.coverage.undefined_fill_count
                  << ", unresolved=" << certificate.coverage.unresolved_count
                  << ", truncated=" << certificate.coverage.truncated_count
                  << '\n'
                  << "Source overlap evidence: "
                  << certificate.source_overlap_count << " overlap(s), "
                  << certificate.source_overlap_unknown_count << " unknown\n"
                  << "Execution: workers=" << certificate.execution.actual_workers
                  << ", reserved_scratch_bytes="
                  << certificate.execution.reserved_parallel_scratch_bytes
                  << '\n'
                  << "Instance reuse: fills=" << certificate.fill_instance_count
                  << ", universes=" << certificate.reused_universe_count
                  << ", duplicated_cells_avoided="
                  << certificate.estimated_duplicated_cells_avoided << '\n';
        for (const csgify::AleaCoverageReviewItem& finding : findings) {
            std::cout << (finding.blocking ? "BLOCKER " : "INFO ")
                      << finding.id << " count=" << finding.count << ": "
                      << finding.summary << " Next: "
                      << finding.recommended_action << '\n';
        }
        return certified ? EXIT_SUCCESS : 2;
    }
#if CSGIFY_HAVE_QT6_VIEWER
    const int viewer_result = csgify::run_qt_coverage_review(
        certificate, argv[3]);
    if (viewer_result != 0) return viewer_result;
    return certified ? EXIT_SUCCESS : 2;
#else
    std::cerr << "error viewer_not_built: use `csgify coverage status`\n";
    return EXIT_FAILURE;
#endif
}

// Human-readable face description used by `explain` to surface why a
// face is blocking emission. Anchored on the AnalyzedFace fields the
// analyzer already populates -- explain is a *read-only view* over
// existing analysis, not new geometry logic.
const char* face_blocker_reason(const csgify::AnalyzedFace& f) {
    if (f.exact_csg_supported) return "supported";
    if (f.needs_spline_replacement) return "spline (needs analytic fit)";
    if (f.needs_surface_replacement) return "surface needs replacement";
    if (f.needs_emitter) return "no emitter wired for this surface";
    return "unsupported";
}

const char* face_suggested_action(const csgify::AnalyzedFace& f) {
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

    csgify::ConversionOptions options;
    csgify::SessionPtr session = csgify::create_session(options);
    csgify::load_step(*session, input_path);
    csgify::CommandResult analysis =
        csgify::analyze(*session, "doc:1", csgify::ReportIdMode::local);
    const csgify::ConversionReport& report = analysis.report;

    // Locate the target. Accept solid:N, piece:N, and face:N.M (with or
    // without doc:1/ prefix). The analyzer's report carries one row per
    // active solid/piece, plus one row per face per solid.
    const csgify::AnalyzedSolid* solid_hit = nullptr;
    const csgify::AnalyzedDecomposedPiece* piece_hit = nullptr;
    const csgify::AnalyzedFace* face_hit = nullptr;

    auto strip_doc_prefix = [](std::string s) {
        const std::string prefix = "doc:1/";
        if (s.rfind(prefix, 0) == 0) s.erase(0, prefix.size());
        return s;
    };
    const std::string id = strip_doc_prefix(target_id);

    for (const csgify::AnalyzedSolid& s : report.solids) {
        if (s.id == id) { solid_hit = &s; break; }
    }
    if (!solid_hit) {
        for (const csgify::AnalyzedDecomposedPiece& p : report.decomposed_pieces) {
            if (p.id == id) { piece_hit = &p; break; }
        }
    }
    if (!solid_hit && !piece_hit) {
        for (const csgify::AnalyzedFace& f : report.faces) {
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
        std::vector<const csgify::AnalyzedFace*> blocking_faces;
        for (const csgify::AnalyzedFace& f : report.faces) {
            if (f.solid_id == solid_hit->id && !f.exact_csg_supported) {
                blocking_faces.push_back(&f);
            }
        }
        if (!blocking_faces.empty()) {
            std::cout << "\nBlocking faces:\n";
            for (const csgify::AnalyzedFace* f : blocking_faces) {
                std::cout << "  " << f->id << "  type=" << f->surface_type
                          << "  " << face_blocker_reason(*f) << "\n";
                std::cout << "    action: " << face_suggested_action(*f) << "\n";
            }
        }

        // Decomposition proposals (if non-convex).
        if (solid_hit->needs_decomposition) {
            csgify::DecompositionProposal proposal =
                csgify::propose_decomposition(*session, solid_hit->id,
                                                csgify::ReportIdMode::local);
            std::cout << "\nDecomposition candidates: " << proposal.candidates.size();
            if (!proposal.best_candidate_id.empty()) {
                std::cout << " (best: " << proposal.best_candidate_id << ")";
            }
            std::cout << "\n";
            const std::size_t limit = std::min<std::size_t>(
                3, proposal.candidates.size());
            for (std::size_t i = 0; i < limit; ++i) {
                const csgify::DecompositionCandidate& c = proposal.candidates[i];
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
            csgify::SurfaceReplacementProposal proposal =
                csgify::propose_surface_replacement(*session, face_hit->id,
                                                       csgify::ReportIdMode::local);
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
                    const csgify::SurfaceReplacementCandidate& c =
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
    std::string progress_json_path;
    std::string policy_name = "auto";
    std::size_t max_depth_value = 4;
    bool has_max_depth = false;
    bool allow_medium = false;
    bool allow_bbox_fallback = false;
    std::string apply_removal_confidence;
    std::size_t max_splits_value = 16;
    std::size_t prismatic_max_pieces = 4096;
    std::size_t prismatic_max_splitter_calls = 10000;
    std::size_t max_targets_value = 0;
    std::vector<std::string> selected_targets;
    bool allow_overlaps = false;
    std::string mcnp_domain_id;
    bool require_domain_coverage = false;
    bool fixed_point = true;
    bool fixed_point_explicit = false;
    bool no_partial_commit = false;
    bool allow_shatter_fallback = false;
    bool use_incremental_trials = true;
    bool run_parallel_plane_batches = true;
    bool verify_incremental_state = false;
    std::size_t fixed_max_actions = 64;
    std::size_t fixed_max_active_pieces = 4096;
    std::size_t fixed_max_candidate_evaluations = 4096;
    std::size_t fixed_max_candidates_per_target = 64;
    std::size_t fixed_max_splitter_calls = 4096;
    std::size_t fixed_max_residual_pieces = 256;
    std::size_t fixed_max_elapsed_ms = 0;
    std::size_t fixed_max_parallel_families = 8;
    std::size_t fixed_max_planes_per_batch = 64;
    std::size_t fixed_max_local_boolean_calls = 2048;
    std::size_t fixed_max_stagnant_target_visits = 2;
    double fixed_max_cumulative_volume_delta = 1.0e-6;
    double fixed_max_action_volume_delta = 1.0e-6;
    std::string approximation_percent;

    OptScanner scan(argc, argv, 5);
    while (!scan.done()) {
        std::string target_id;
        if (scan.take_string("--log", &log_path)) continue;
        if (scan.take_string("--out-step", &step_out_path)) continue;
        if (scan.take_string("--report", &report_path)) continue;
        if (scan.take_string("--progress-json", &progress_json_path)) continue;
        if (scan.take_string("--policy", &policy_name)) continue;
        if (scan.take_string("--apply-removal-candidates",
                             &apply_removal_confidence)) continue;
        if (scan.take_size("--max-depth", &max_depth_value, &has_max_depth)) continue;
        if (scan.take_size("--max-splits", &max_splits_value)) continue;
        if (scan.take_size("--prismatic-max-pieces",
                           &prismatic_max_pieces)) continue;
        if (scan.take_size("--prismatic-max-splitter-calls",
                           &prismatic_max_splitter_calls)) continue;
        if (scan.take_size("--max-targets", &max_targets_value)) continue;
        if (scan.take_size("--max-actions", &fixed_max_actions)) continue;
        if (scan.take_size("--max-active-pieces", &fixed_max_active_pieces)) continue;
        if (scan.take_size("--max-candidate-evaluations",
                           &fixed_max_candidate_evaluations)) continue;
        if (scan.take_size("--max-candidates-per-target",
                           &fixed_max_candidates_per_target)) continue;
        if (scan.take_size("--max-splitter-calls",
                           &fixed_max_splitter_calls)) continue;
        if (scan.take_size("--max-residual-pieces",
                           &fixed_max_residual_pieces)) continue;
        if (scan.take_size("--max-elapsed-ms", &fixed_max_elapsed_ms)) continue;
        if (scan.take_size("--max-parallel-families",
                           &fixed_max_parallel_families)) continue;
        if (scan.take_size("--max-planes-per-batch",
                           &fixed_max_planes_per_batch)) continue;
        if (scan.take_size("--max-local-boolean-calls",
                           &fixed_max_local_boolean_calls)) continue;
        if (scan.take_size("--max-stagnant-target-visits",
                           &fixed_max_stagnant_target_visits)) continue;
        if (scan.take_double("--max-cumulative-volume-delta",
                             &fixed_max_cumulative_volume_delta)) continue;
        if (scan.take_double("--max-action-volume-delta",
                             &fixed_max_action_volume_delta)) continue;
        if (scan.take_string("--approx-volume", &approximation_percent)) continue;
        if (scan.take_string("--target", &target_id)) {
            selected_targets.push_back(target_id);
            continue;
        }
        if (scan.take_string("--mcnp-domain", &mcnp_domain_id)) continue;
        if (scan.take_flag("--allow-medium-confidence-auto")) { allow_medium = true; continue; }
        if (scan.take_flag("--allow-bbox-fallback-auto")) { allow_bbox_fallback = true; continue; }
        if (scan.take_flag("--allow-overlaps")) { allow_overlaps = true; continue; }
        if (scan.take_flag("--fixed-point")) {
            fixed_point = true;
            fixed_point_explicit = true;
            continue;
        }
        if (scan.take_flag("--single-pass")) { fixed_point = false; continue; }
        if (scan.take_flag("--no-partial-commit")) { no_partial_commit = true; continue; }
        if (scan.take_flag("--allow-shatter-fallback")) { allow_shatter_fallback = true; continue; }
        if (scan.take_flag("--legacy-global-trials")) {
            use_incremental_trials = false;
            continue;
        }
        if (scan.take_flag("--no-parallel-plane-batches")) {
            run_parallel_plane_batches = false;
            continue;
        }
        if (scan.take_flag("--verify-incremental-state")) {
            verify_incremental_state = true;
            continue;
        }
        if (scan.take_flag("--require-domain-coverage")) {
            require_domain_coverage = true;
            continue;
        }
        std::cerr << "error unrecognized_option: " << scan.peek() << '\n';
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (step_out_path.empty()) {
        const std::size_t slash = output_path.find_last_of("/\\");
        const std::size_t dot = output_path.find_last_of('.');
        const bool has_extension = dot != std::string::npos
            && (slash == std::string::npos || dot > slash);
        step_out_path = (has_extension ? output_path.substr(0, dot) : output_path)
            + "_decomposed.step";
    }

    csgify::DecompositionPolicy policy;
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
        policy.allow_low_confidence_auto = true;
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

    std::cerr << "csgify: OCCT " << csgify::occt_version()
              << ", Alea " << alea_version() << '\n';

    csgify::ConversionOptions options;
    // This command performs its own bounded BSP decomposition below.  If a
    // target remains unresolved, do not let emission retry with the legacy
    // all-planar-faces splitter: on solids with many planar faces that single
    // splitter call can construct a combinatorial arrangement and exhaust
    // memory (while also ignoring --max-splits/--max-depth).
    options.try_planar_decomposition = false;
    options.prismatic_max_pieces = prismatic_max_pieces;
    options.prismatic_max_splitter_calls = prismatic_max_splitter_calls;
    if (allow_overlaps) options.allow_overlaps = true;
    options.mcnp_domain_id = mcnp_domain_id;
    options.require_domain_coverage = require_domain_coverage;
    if (!approximation_percent.empty()) {
        std::string parse_error;
        if (!set_uniform_approximation_budget(
                approximation_percent, &options.approximation, &parse_error)) {
            std::cerr << "error invalid_approximation_percent: "
                      << parse_error << '\n';
            return EXIT_FAILURE;
        }
        options.approximation.allow_overlap_resolution = true;
    }
    csgify::SessionPtr session = csgify::create_session(options);
    if (!log_path.empty()) {
        try {
            csgify::attach_decision_log(*session, log_path, input_path);
        } catch (const std::exception& e) {
            std::cerr << "error decision_log_open_failed: " << e.what() << '\n';
            return EXIT_FAILURE;
        }
    }
    const auto decompose_load_started = std::chrono::steady_clock::now();
    std::cerr << "stage load_step: started input=" << input_path << '\n';
    const csgify::DocumentId document_id = csgify::load_step(*session, input_path);
    const double decompose_load_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - decompose_load_started).count();
    std::cerr << "stage load_step: completed elapsed_s="
              << std::fixed << std::setprecision(1)
              << decompose_load_seconds << '\n';

    // A positive-volume region must belong to exactly one MCNP component.
    // With an explicit budget, resolve each detected input overlap using a
    // stable ownership rule: lexicographically smaller stable ID owns the
    // region, the other component is cut. Geometry measurement and the
    // cumulative ledger decide whether "small" is actually acceptable.
    if (options.approximation.enabled) {
        const csgify::CommandResult overlap_analysis = csgify::analyze(
            *session, document_id, csgify::ReportIdMode::local);
        const std::vector<csgify::SolidOverlap> overlaps =
            overlap_analysis.report.overlaps;
        for (const csgify::SolidOverlap& overlap : overlaps) {
            const std::string owner = std::min(overlap.solid_a_id,
                                               overlap.solid_b_id);
            const std::string loser = owner == overlap.solid_a_id
                ? overlap.solid_b_id : overlap.solid_a_id;
            const csgify::ApproximationCommandResult resolved =
                csgify::resolve_overlap_with_approximation(
                    *session, loser, owner, options.approximation,
                    csgify::ReportIdMode::local);
            if (!resolved.command.ok) {
                std::cerr << "error " << resolved.budget.code << ": "
                          << resolved.budget.message << " (owner=" << owner
                          << ", loser=" << loser << ")\n";
                if (!report_path.empty())
                    write_report_or_stdout(resolved.command.report,
                                           report_path);
                return EXIT_FAILURE;
            }
            std::cerr << "overlap ownership: owner=" << owner
                      << ", loser=" << loser
                      << ", removed=" << resolved.metrics.removed_volume
                      << ", fraction="
                      << resolved.metrics.changed_volume_fraction << '\n';
        }
    }

    if (!apply_removal_confidence.empty()) {
        csgify::RemovalProposal removals =
            csgify::suggest_removals(*session,
                                      document_id,
                                      csgify::ReportIdMode::local);
        std::size_t removed = 0;
        for (const csgify::RemovalCandidate& candidate : removals.candidates) {
            const bool allowed = candidate.confidence == "high"
                || (apply_removal_confidence == "medium"
                    && candidate.confidence == "medium");
            if (!allowed || candidate.suggested_action == "manual") {
                continue;
            }
            csgify::CommandResult r =
                csgify::remove_solid(*session,
                                      candidate.solid_id,
                                      csgify::ReportIdMode::local);
            if (r.ok) {
                ++removed;
            }
        }
        std::cerr << "preprocess: removed " << removed
                  << " component(s) from removal candidates"
                  << " (policy=" << apply_removal_confidence << ")\n";
    }

    // The legacy target-filtered form remains a single-pass debugging API.
    // An explicitly requested fixed-point run rejects the filter so callers
    // cannot mistake a local score for the document-wide invariant.
    if (fixed_point && !fixed_point_explicit && !selected_targets.empty()) {
        fixed_point = false;
    }

    if (fixed_point) {
        if (!selected_targets.empty()) {
            std::cerr << "error fixed_point_target_filter_not_supported: "
                         "fixed-point currently evaluates the complete document\n";
            return EXIT_FAILURE;
        }
        csgify::FixedPointDecompositionPolicy fixed_policy;
        fixed_policy.candidate_policy = policy;
        fixed_policy.max_actions = fixed_max_actions;
        fixed_policy.max_candidate_evaluations =
            fixed_max_candidate_evaluations;
        fixed_policy.max_candidates_per_target =
            fixed_max_candidates_per_target;
        fixed_policy.max_splitter_calls = fixed_max_splitter_calls;
        fixed_policy.max_active_pieces = fixed_max_active_pieces;
        fixed_policy.max_residual_pieces = fixed_max_residual_pieces;
        fixed_policy.max_elapsed_ms = fixed_max_elapsed_ms;
        fixed_policy.max_target_depth = has_max_depth
            ? static_cast<int>(max_depth_value) : fixed_policy.max_target_depth;
        fixed_policy.max_cumulative_relative_volume_delta =
            fixed_max_cumulative_volume_delta;
        fixed_policy.max_action_relative_volume_delta =
            fixed_max_action_volume_delta;
        fixed_policy.min_piece_volume_fraction =
            policy.min_piece_volume_fraction;
        fixed_policy.allow_medium_certified_progress =
            policy_name != "conservative";
        fixed_policy.allow_low_certified_progress = policy_name == "low";
        fixed_policy.allow_bbox_fallback = policy.allow_bbox_fallback_auto;
        fixed_policy.run_shatter_fallback = allow_shatter_fallback;
        fixed_policy.commit_certified_partial_progress = !no_partial_commit;
        fixed_policy.use_incremental_trials = use_incremental_trials;
        fixed_policy.run_parallel_plane_batches = run_parallel_plane_batches;
        fixed_policy.verify_incremental_state_each_action =
            verify_incremental_state;
        fixed_policy.max_parallel_families_per_target =
            fixed_max_parallel_families;
        fixed_policy.max_planes_per_batch = fixed_max_planes_per_batch;
        fixed_policy.max_local_boolean_calls =
            fixed_max_local_boolean_calls;
        fixed_policy.max_stagnant_target_visits =
            fixed_max_stagnant_target_visits;

        std::ofstream progress_file;
        std::ostream* progress_out = nullptr;
        if (!progress_json_path.empty()) {
            if (progress_json_path == "-") progress_out = &std::cout;
            else {
                progress_file.open(progress_json_path);
                if (!progress_file) {
                    std::cerr << "error progress_json_open_failed: "
                              << progress_json_path << '\n';
                    return EXIT_FAILURE;
                }
                progress_out = &progress_file;
            }
        }
        const csgify::FixedPointProgressCallback progress =
            [progress_out](const csgify::FixedPointProgressEvent& event) {
                if (event.phase == csgify::FixedPointProgressPhase::started) {
                    std::cerr
                        << "stage fixed_point_initial_analysis_and_overlap: "
                           "started\n";
                } else if (event.phase == csgify::FixedPointProgressPhase::
                               initial_analysis_completed) {
                    std::cerr
                        << "stage fixed_point_initial_analysis_and_overlap: "
                           "completed, residuals="
                        << event.state.residual_count
                        << ", exact_bodies=" << event.state.exact_body_count
                        << ", overlap_errors="
                        << event.state.overlap_error_count
                        << ", elapsed_s=" << std::fixed
                        << std::setprecision(1)
                        << event.elapsed_ms / 1000.0 << '\n';
                } else if (event.phase
                           == csgify::FixedPointProgressPhase::target_started) {
                    std::cerr << "stage fixed_point_target: target="
                              << event.target_id;
                    if (!event.strategy.empty()) {
                        std::cerr << ", strategy=" << event.strategy;
                    }
                    std::cerr << ", elapsed_s=" << std::fixed
                              << std::setprecision(1)
                              << event.elapsed_ms / 1000.0 << '\n';
                } else if (event.phase
                    == csgify::FixedPointProgressPhase::action_accepted) {
                    std::cerr << "fixed-point progress: action="
                              << event.actions_applied
                              << ", target=" << event.target_id
                              << ", strategy=" << event.strategy
                              << ", residuals=" << event.state.residual_count
                              << ", active-pieces="
                              << event.state.active_piece_count << '\n';
                } else if (event.phase
                           == csgify::FixedPointProgressPhase::
                               terminal_verification_started) {
                    std::cerr
                        << "stage fixed_point_terminal_verification: started"
                        << ", actions=" << event.actions_applied
                        << ", elapsed_s=" << std::fixed
                        << std::setprecision(1)
                        << event.elapsed_ms / 1000.0 << '\n';
                } else if (event.phase
                           == csgify::FixedPointProgressPhase::finished) {
                    std::cerr << "stage fixed_point: finished stop="
                              << csgify::fixed_point_stop_reason_name(
                                     event.stop_reason)
                              << ", elapsed_s=" << std::fixed
                              << std::setprecision(1)
                              << event.elapsed_ms / 1000.0 << '\n';
                }
                if (progress_out) {
                    // IDs and strategy names are generated by csgify and do
                    // not contain JSON control characters.
                    *progress_out
                        << "{\"schema\":\"csgify.fixed-point-progress\""
                        << ",\"phase\":\""
                        << csgify::fixed_point_progress_phase_name(event.phase)
                        << "\",\"actions_applied\":" << event.actions_applied
                        << ",\"candidates_evaluated\":"
                        << event.candidates_evaluated
                        << ",\"splitter_calls\":" << event.splitter_calls
                        << ",\"target\":\"" << event.target_id
                        << "\",\"candidate\":\"" << event.candidate_id
                        << "\",\"strategy\":\"" << event.strategy
                        << "\",\"residual_count\":"
                        << event.state.residual_count
                        << ",\"residual_volume\":" << std::setprecision(17)
                        << event.state.residual_volume
                        << ",\"active_piece_count\":"
                        << event.state.active_piece_count
                        << ",\"exact_body_count\":"
                        << event.state.exact_body_count
                        << ",\"elapsed_ms\":" << event.elapsed_ms
                        << ",\"stop_reason\":\""
                        << (event.phase
                                == csgify::FixedPointProgressPhase::finished
                            ? csgify::fixed_point_stop_reason_name(
                                  event.stop_reason)
                            : "none")
                        << "\"}\n";
                    progress_out->flush();
                }
                return true;
            };
        std::mutex heartbeat_mutex;
        std::condition_variable heartbeat_cv;
        bool heartbeat_done = false;
        const auto fixed_started = std::chrono::steady_clock::now();
        std::thread heartbeat([&]() {
            std::unique_lock<std::mutex> lock(heartbeat_mutex);
            while (!heartbeat_cv.wait_for(
                lock, std::chrono::seconds(30),
                [&]() { return heartbeat_done; })) {
                const double seconds = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - fixed_started).count();
                std::cerr << "stage fixed_point: working elapsed_s="
                          << std::fixed << std::setprecision(1) << seconds
                          << '\n';
            }
        });
        csgify::FixedPointDecompositionResult fixed =
            csgify::decompose_to_fixed_point(
                *session, document_id, fixed_policy,
                csgify::ReportIdMode::local, progress);
        {
            std::lock_guard<std::mutex> lock(heartbeat_mutex);
            heartbeat_done = true;
        }
        heartbeat_cv.notify_one();
        heartbeat.join();
        std::cerr << "fixed-point: stop="
                  << csgify::fixed_point_stop_reason_name(
                         fixed.summary.stop_reason)
                  << ", actions=" << fixed.summary.actions_applied
                  << ", residuals=" << fixed.summary.final_state.residual_count
                  << ", active-pieces="
                  << fixed.summary.final_state.active_piece_count
                  << ", candidates=" << fixed.summary.candidates_evaluated
                  << ", splitters=" << fixed.summary.splitter_calls << '\n';

        if (!fixed.summary.exact) {
            if (!step_out_path.empty() && fixed.summary.mutated) {
                csgify::CommandResult step_result =
                    csgify::export_document_step(
                        *session, document_id, step_out_path,
                        csgify::ReportIdMode::local);
                if (!step_result.ok) print_diagnostics(step_result.report);
            }
            if (!report_path.empty()
                && write_report_or_stdout(fixed.command.report,
                                          report_path) != 0) {
                std::cerr << "error report_write_failed: " << report_path << '\n';
            }
            print_diagnostics(fixed.command.report);
            print_summary(fixed.command.report);
            return EXIT_FAILURE;
        }
        if (!step_out_path.empty()) {
            csgify::CommandResult step_result = csgify::export_document_step(
                *session, document_id, step_out_path,
                csgify::ReportIdMode::local);
            if (!step_result.ok) {
                print_diagnostics(step_result.report);
                return EXIT_FAILURE;
            }
        }
        csgify::CommandResult emitted = csgify::emit_document(
            *session, document_id, output_path, csgify::ReportIdMode::local);
        print_diagnostics(emitted.report);
        print_summary(emitted.report);
        if (!report_path.empty()
            && write_report_or_stdout(emitted.report, report_path) != 0) {
            std::cerr << "error report_write_failed: " << report_path << '\n';
            return EXIT_FAILURE;
        }
        return emitted.report.ok() && emitted.report.solids_converted > 0
            ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    // Legacy single-pass target discovery needs a complete analysis. The
    // fixed-point branch above deliberately avoids this pass because its
    // initial ledger reconciliation computes the same authoritative state;
    // analyzing immediately beforehand doubles the assembly-wide cost.
    csgify::CommandResult analysis =
        csgify::analyze(*session, document_id, csgify::ReportIdMode::local);

    // analysis.ok may be false here: nonconvex_solid_requires_decomposition
    // is an error-severity diagnostic and is *exactly* the condition this
    // command is meant to resolve. We snapshot the targets regardless and
    // bail only if the snapshot is also empty (i.e., nothing to do and
    // analysis already failed for other reasons).
    std::vector<std::string> targets = selected_targets;
    if (targets.empty()) {
        for (const csgify::AnalyzedSolid& s : analysis.report.solids) {
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
    std::size_t prismatic_applied = 0;
    std::size_t shell_differences_applied = 0;
    std::size_t pipe_annuli_applied = 0;
    std::size_t revolutions_applied = 0;
    std::size_t partial = 0;
    csgify::CommandResult last_operation_result = analysis;
    for (const std::string& id : targets) {
        ++attempted;
        const auto analyzed = std::find_if(
            analysis.report.solids.begin(), analysis.report.solids.end(),
            [&](const csgify::AnalyzedSolid& solid) { return solid.id == id; });
        const bool use_shell_difference = analyzed != analysis.report.solids.end()
            && analyzed->topology.cavity_shell_count > 0;
        const bool use_pipe_annulus = !use_shell_difference
            && analyzed != analysis.report.solids.end()
            && analyzed->topology.pipe_annulus.candidate;
        const bool use_revolution = !use_shell_difference && !use_pipe_annulus
            && analyzed != analysis.report.solids.end()
            && analyzed->topology.revolution.candidate
            && analyzed->topology.revolution.full_revolution;
        const bool use_prismatic = !use_shell_difference && !use_pipe_annulus
            && !use_revolution
            && analyzed != analysis.report.solids.end()
            && analyzed->topology.prismatic_candidate
            && !(has_max_depth && max_depth_value == 0);
        csgify::CommandResult r = use_shell_difference
            ? csgify::decompose_shell_difference(*session, id,
                                                 csgify::ReportIdMode::local)
            : use_pipe_annulus
                ? csgify::decompose_pipe_annulus(*session, id,
                                                 csgify::ReportIdMode::local)
                : use_revolution
                    ? csgify::decompose_revolution(
                          *session, id, csgify::ReportIdMode::local)
                    : use_prismatic
                ? csgify::decompose_prismatic(*session, id,
                                              csgify::ReportIdMode::local)
                : csgify::auto_decompose_bsp(*session, id,
                                        static_cast<int>(max_depth_value),
                                        policy,
                                        csgify::ReportIdMode::local,
                                        /*defer_analysis=*/true);
        last_operation_result = r;
        const bool has_residual = std::any_of(
            r.report.decomposed_pieces.begin(),
            r.report.decomposed_pieces.end(),
            [](const csgify::AnalyzedDecomposedPiece& piece) {
                return piece.needs_decomposition
                    || piece.needs_surface_replacement;
            });
        if (r.ok) {
            if (has_residual) ++partial;
            else ++succeeded;
            if (use_prismatic) ++prismatic_applied;
            if (use_shell_difference) ++shell_differences_applied;
            if (use_pipe_annulus) ++pipe_annuli_applied;
            if (use_revolution) ++revolutions_applied;
        } else {
            const bool stuck_on_confidence = std::any_of(
                r.report.diagnostics.begin(), r.report.diagnostics.end(),
                [](const csgify::Diagnostic& d) {
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
              << ", prismatic=" << prismatic_applied
              << ", shell-difference=" << shell_differences_applied
              << ", pipe-annulus=" << pipe_annuli_applied
              << ", revolution=" << revolutions_applied
              << ", partial=" << partial
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

    // A progressive decomposition may commit an exact volume partition while
    // honestly retaining residual non-convex cells.  Return that report and
    // optional STEP artifact, but do not ask emission to convert a model that
    // is known to be incomplete.
    if (partial > 0 || other_failure > 0) {
        if (!report_path.empty()
            && write_report_or_stdout(last_operation_result.report,
                                      report_path) != 0) {
            std::cerr << "error report_write_failed: " << report_path << '\n';
            return EXIT_FAILURE;
        }
        if (!step_out_path.empty()) {
            csgify::CommandResult step_result =
                csgify::export_document_step(*session, document_id,
                                              step_out_path,
                                              csgify::ReportIdMode::local);
            if (!step_result.ok) print_diagnostics(step_result.report);
        }
        print_diagnostics(last_operation_result.report);
        print_summary(last_operation_result.report);
        return EXIT_FAILURE;
    }

    if (!step_out_path.empty()) {
        csgify::CommandResult step_result =
            csgify::export_document_step(*session, document_id, step_out_path,
                                          csgify::ReportIdMode::local);
        if (!step_result.ok) {
            print_diagnostics(step_result.report);
            return EXIT_FAILURE;
        }
    }

    csgify::CommandResult result =
        csgify::emit_document(*session, document_id, output_path,
                               csgify::ReportIdMode::local);
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

int run_shatter_command(int argc, char** argv) {
    if (wants_help(argc, argv)) return print_verb_help_and_exit(argv[0], "shatter");
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
    double max_depth_value = 8.0;
    bool shatter_all = false;
    bool allow_volume_drift = false;
    bool allow_overlaps = false;
    std::vector<std::string> selected_targets;
    std::string mcnp_domain_id;
    bool require_domain_coverage = false;
    std::string approximation_percent;
    bool allow_containment_change = false;
    bool allow_topology_change = false;

    OptScanner scan(argc, argv, 5);
    while (!scan.done()) {
        std::string target_id;
        if (scan.take_string("--log", &log_path)) continue;
        if (scan.take_string("--out-step", &step_out_path)) continue;
        if (scan.take_string("--report", &report_path)) continue;
        if (scan.take_string("--mcnp-domain", &mcnp_domain_id)) continue;
        if (scan.take_string("--approx-volume", &approximation_percent)) continue;
        if (scan.take_double("--max-depth", &max_depth_value)) continue;
        if (scan.take_string("--target", &target_id)) {
            selected_targets.push_back(target_id);
            continue;
        }
        if (scan.take_flag("--all")) { shatter_all = true; continue; }
        if (scan.take_flag("--allow-shatter-volume-drift")) {
            allow_volume_drift = true;
            continue;
        }
        if (scan.take_flag("--allow-overlaps")) { allow_overlaps = true; continue; }
        if (scan.take_flag("--require-domain-coverage")) {
            require_domain_coverage = true;
            continue;
        }
        if (scan.take_flag("--allow-containment-change")) {
            allow_containment_change = true;
            continue;
        }
        if (scan.take_flag("--allow-topology-change")) {
            allow_topology_change = true;
            continue;
        }
        std::cerr << "error unrecognized_option: " << scan.peek() << '\n';
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (!shatter_all && selected_targets.empty()) {
        std::cerr << "error missing_target: shatter requires --all or "
                     "--target solid:N\n";
        return EXIT_FAILURE;
    }
    if (max_depth_value < 0.0) {
        std::cerr << "error invalid_max_depth: --max-depth must be non-negative\n";
        return EXIT_FAILURE;
    }

    std::cerr << "csgify: OCCT " << csgify::occt_version()
              << ", Alea " << alea_version() << '\n';

    csgify::ConversionOptions options;
    if (allow_overlaps) options.allow_overlaps = true;
    options.mcnp_domain_id = mcnp_domain_id;
    options.require_domain_coverage = require_domain_coverage;
    if (!approximation_percent.empty()) {
        std::string error;
        if (!set_uniform_approximation_budget(
                approximation_percent, &options.approximation, &error)) {
            std::cerr << "error invalid_approximation_percent: " << error << '\n';
            return EXIT_FAILURE;
        }
        options.approximation.allow_numerical_shatter_loss = true;
        options.approximation.require_containment_preservation =
            !allow_containment_change;
        options.approximation.require_topology_preservation =
            !allow_topology_change;
    }
    if (allow_volume_drift && options.approximation.enabled) {
        std::cerr << "error shatter_drift_policy_conflict: use --approx-volume "
                     "instead of --allow-shatter-volume-drift\n";
        return EXIT_FAILURE;
    }
    csgify::SessionPtr session = csgify::create_session(options);
    if (!log_path.empty()) {
        try {
            csgify::attach_decision_log(*session, log_path, input_path);
        } catch (const std::exception& e) {
            std::cerr << "error decision_log_open_failed: " << e.what() << '\n';
            return EXIT_FAILURE;
        }
    }
    const csgify::DocumentId document_id = csgify::load_step(*session, input_path);

    // Explicit --target wins; otherwise (--all) snapshot every solid that
    // fails convex validation. solid:N IDs are stable across shatters (a
    // shattered solid is suppressed, not renumbered), so a single up-front
    // snapshot is valid for the whole loop.
    std::vector<std::string> targets = selected_targets;
    if (targets.empty()) {
        csgify::CommandResult analysis =
            csgify::analyze(*session, document_id, csgify::ReportIdMode::local);
        for (const csgify::AnalyzedSolid& s : analysis.report.solids) {
            if (s.needs_decomposition) {
                targets.push_back(s.id);
            }
        }
        if (targets.empty()) {
            std::cerr << "shatter: no solids require decomposition; "
                         "emitting as-is\n";
        }
    }

    std::size_t attempted = 0;
    std::size_t succeeded = 0;
    std::size_t residual = 0;
    std::size_t partial = 0;
    std::size_t other_failure = 0;
    csgify::CommandResult last_operation_result;
    for (const std::string& id : targets) {
        ++attempted;
        csgify::CommandResult r;
        if (options.approximation.enabled) {
            r = csgify::shatter_with_approximation(
                *session, id, static_cast<int>(max_depth_value),
                options.approximation, csgify::ReportIdMode::local).command;
        } else {
            r = csgify::shatter(*session, id,
                                 static_cast<int>(max_depth_value),
                                 allow_volume_drift,
                                 csgify::ReportIdMode::local);
        }
        last_operation_result = r;
        const bool has_partial = std::any_of(
            r.report.decomposed_pieces.begin(),
            r.report.decomposed_pieces.end(),
            [](const csgify::AnalyzedDecomposedPiece& piece) {
                return piece.needs_decomposition
                    || piece.needs_surface_replacement;
            });
        if (r.ok) {
            if (has_partial) ++partial;
            else ++succeeded;
        } else {
            const bool stuck = std::any_of(
                r.report.diagnostics.begin(), r.report.diagnostics.end(),
                [](const csgify::Diagnostic& d) {
                    return d.code == "shatter_residual_nonconvex";
                });
            if (stuck) ++residual;
            else ++other_failure;
        }
    }

    std::cerr << "shatter: " << succeeded << "/" << attempted
              << " solid(s) reduced to convex pieces"
              << " (partial=" << partial
              << ", max-depth=" << static_cast<int>(max_depth_value) << ")\n";
    if (residual > 0) {
        std::cerr << "  " << residual
                  << " solid(s) left residual (need surface replacement, not"
                     " decomposition); see report diagnostics.\n";
    }
    if (other_failure > 0) {
        std::cerr << "  " << other_failure
                  << " failed for other reasons; see report diagnostics.\n";
    }

    // Do not feed an unresolved source solid into emit's legacy fallback.
    // Preserve the operation diagnostics first; callers must always receive a
    // report even when a later STEP export is also requested.
    if (partial > 0 || residual > 0 || other_failure > 0) {
        if (!report_path.empty()
            && write_report_or_stdout(last_operation_result.report,
                                      report_path) != 0) {
            std::cerr << "error report_write_failed: " << report_path << '\n';
            return EXIT_FAILURE;
        }
        if (!step_out_path.empty()) {
            csgify::CommandResult step_result =
                csgify::export_document_step(*session, document_id,
                                              step_out_path,
                                              csgify::ReportIdMode::local);
            if (!step_result.ok) {
                print_diagnostics(step_result.report);
            }
        }
        print_diagnostics(last_operation_result.report);
        print_summary(last_operation_result.report);
        return EXIT_FAILURE;
    }

    if (!step_out_path.empty()) {
        csgify::CommandResult step_result =
            csgify::export_document_step(*session, document_id, step_out_path,
                                          csgify::ReportIdMode::local);
        if (!step_result.ok) {
            print_diagnostics(step_result.report);
            return EXIT_FAILURE;
        }
    }

    csgify::CommandResult result =
        csgify::emit_document(*session, document_id, output_path,
                               csgify::ReportIdMode::local);
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
    std::size_t prismatic_max_pieces = 4096;
    std::size_t prismatic_max_splitter_calls = 10000;
    bool dry_run = false;
    bool allow_overlaps = false;
    bool has_max_deviation = false;
    double max_deviation = 0.0;

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
        if (scan.take_size("--prismatic-max-pieces",
                           &prismatic_max_pieces)) continue;
        if (scan.take_size("--prismatic-max-splitter-calls",
                           &prismatic_max_splitter_calls)) continue;
        if (scan.take_size("--max-targets", &max_targets_value)) continue;
        if (scan.take_double("--max-deviation", &max_deviation,
                             &has_max_deviation)) continue;
        if (scan.take_string("--target", &target_id)) {
            selected_targets.push_back(target_id);
            continue;
        }
        if (scan.take_flag("--allow-medium-confidence-auto")) { allow_medium = true; continue; }
        if (scan.take_flag("--allow-bbox-fallback-auto")) { allow_bbox_fallback = true; continue; }
        if (scan.take_flag("--safe-all")) { safe_all = true; continue; }
        if (scan.take_flag("--dry-run")) { dry_run = true; continue; }
        if (scan.take_flag("--allow-overlaps")) { allow_overlaps = true; continue; }
        std::cerr << "error unrecognized_option: " << scan.peek() << '\n';
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (step_out_path.empty() && !dry_run) {
        std::cerr << "error preprocess_requires_out_step: preprocess requires --out-step (omit only with --dry-run)\n";
        return EXIT_FAILURE;
    }
    if (has_max_deviation
        && (!std::isfinite(max_deviation) || max_deviation < 0.0)) {
        std::cerr << "error invalid_max_deviation: expected a finite "
                     "non-negative value\n";
        return EXIT_FAILURE;
    }
    // Feature removal changes modeling intent, so even high-confidence
    // candidates remain opt-in. Hole fills are likewise explicit decisions.

    csgify::DecompositionPolicy policy;
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
        policy.allow_low_confidence_auto = true;
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

    std::cerr << "csgify: OCCT " << csgify::occt_version()
              << ", Alea " << alea_version() << '\n';

    csgify::ConversionOptions options;
    options.prismatic_max_pieces = prismatic_max_pieces;
    options.prismatic_max_splitter_calls = prismatic_max_splitter_calls;
    if (allow_overlaps) options.allow_overlaps = true;
    csgify::SessionPtr session = csgify::create_session(options);
    if (!log_path.empty()) {
        try {
            csgify::attach_decision_log(*session, log_path, input_path);
        } catch (const std::exception& e) {
            std::cerr << "error decision_log_open_failed: " << e.what() << '\n';
            return EXIT_FAILURE;
        }
    }
    const csgify::DocumentId document_id = csgify::load_step(*session, input_path);

    // --safe-all runs the full analyze first so we can refuse to
    // proceed if any solids interpenetrate. Auto-fit / BSP / removal
    // can only make a clean model out of a clean model -- they may
    // also INTRODUCE new overlaps (a bbox replacement that lands on a
    // neighbor), and trying to do that on top of pre-existing overlaps
    // would compound the problem. The user must resolve pairs first,
    // or pass --allow-overlaps to override (only for advanced flows
    // that plan to fix the overlap in a later step).
    if (safe_all && !allow_overlaps) {
        csgify::CommandResult precheck =
            csgify::analyze(*session, document_id,
                              csgify::ReportIdMode::local);
        if (!precheck.report.overlaps.empty()) {
            std::cerr << "error preprocess_overlaps_present: --safe-all "
                      << "refuses to proceed; "
                      << precheck.report.overlaps.size()
                      << " interpenetrating solid pair(s) detected. "
                      << "Resolve them first (decompose / remove / fix "
                      << "the source CAD), or pass --allow-overlaps.\n";
            for (const csgify::SolidOverlap& ov : precheck.report.overlaps) {
                std::cerr << "  " << ov.solid_a_id << " <-> " << ov.solid_b_id
                          << "  intersection_volume=" << ov.intersection_volume
                          << ", fraction_of_smaller=" << ov.min_fraction
                          << '\n';
            }
            return EXIT_FAILURE;
        }
    }

    // --safe-all extends preprocess with a leading auto-fit pass over
    // spline faces that meet --max-deviation. This is the only place
    // we touch face geometry; the rest of preprocess still operates
    // on whole solids. Skipped in --dry-run mode -- we just count.
    std::size_t auto_fit_applied = 0;
    std::size_t auto_fit_seen = 0;
    if (safe_all && has_max_deviation && !dry_run) {
        csgify::AutoFitCommandResult af =
            csgify::auto_fit_splines_with_summary(
                *session, document_id,
                /*has_max_deviation=*/true, max_deviation,
                csgify::ReportIdMode::local);
        auto_fit_applied = af.summary.applied;
        auto_fit_seen = af.summary.spline_faces_seen;
        std::cerr << "preprocess: auto-fit replaced " << auto_fit_applied
                  << "/" << auto_fit_seen << " spline faces"
                  << " (over-budget=" << af.summary.skipped_over_budget
                  << ", no-fit=" << af.summary.skipped_no_fit << ")\n";
    } else if (safe_all && has_max_deviation && dry_run) {
        csgify::CommandResult analyse =
            csgify::analyze(*session, document_id, csgify::ReportIdMode::local);
        for (const csgify::AnalyzedFace& f : analyse.report.faces) {
            if (f.needs_spline_replacement) ++auto_fit_seen;
        }
        std::cerr << "preprocess [dry-run]: would auto-fit up to "
                  << auto_fit_seen << " spline faces with max_deviation="
                  << max_deviation << "\n";
    } else if (safe_all) {
        std::cerr << "preprocess: spline auto-fit skipped; pass --max-deviation "
                     "to provide an explicit approximation budget\n";
    }

    std::size_t removed = 0;
    std::size_t removal_candidates_total = 0;
    if (!apply_removal_confidence.empty()) {
        csgify::RemovalProposal removals =
            csgify::suggest_removals(*session,
                                      document_id,
                                      csgify::ReportIdMode::local);
        for (const csgify::RemovalCandidate& candidate : removals.candidates) {
            const bool allowed = candidate.confidence == "high"
                || (apply_removal_confidence == "medium"
                    && candidate.confidence == "medium");
            if (!allowed || candidate.suggested_action == "manual") {
                continue;
            }
            ++removal_candidates_total;
            if (dry_run) continue;
            csgify::CommandResult r =
                csgify::remove_solid(*session,
                                      candidate.solid_id,
                                      csgify::ReportIdMode::local);
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

    csgify::CommandResult analysis =
        csgify::analyze(*session, document_id, csgify::ReportIdMode::local);
    std::vector<std::string> targets = selected_targets;
    if (targets.empty()) {
        for (const csgify::AnalyzedSolid& s : analysis.report.solids) {
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
    std::size_t prismatic_applied = 0;
    std::size_t shell_differences_applied = 0;
    std::size_t pipe_annuli_applied = 0;
    std::size_t revolutions_applied = 0;
    if (dry_run) {
        std::cerr << "preprocess [dry-run]: would attempt BSP decomposition on "
                  << targets.size() << " non-convex target(s)"
                  << " (policy=" << policy_name
                  << ", max-depth=" << max_depth_value << ")\n";
    } else {
        for (const std::string& id : targets) {
            ++attempted;
            const auto analyzed = std::find_if(
                analysis.report.solids.begin(), analysis.report.solids.end(),
                [&](const csgify::AnalyzedSolid& solid) { return solid.id == id; });
            const bool use_shell_difference =
                analyzed != analysis.report.solids.end()
                && analyzed->topology.cavity_shell_count > 0;
            const bool use_pipe_annulus = !use_shell_difference
                && analyzed != analysis.report.solids.end()
                && analyzed->topology.pipe_annulus.candidate;
            const bool use_revolution = !use_shell_difference
                && !use_pipe_annulus
                && analyzed != analysis.report.solids.end()
                && analyzed->topology.revolution.candidate
                && analyzed->topology.revolution.full_revolution;
            const bool use_prismatic = !use_shell_difference && !use_pipe_annulus
                && !use_revolution
                && analyzed != analysis.report.solids.end()
                && analyzed->topology.prismatic_candidate;
            csgify::CommandResult r = use_shell_difference
                ? csgify::decompose_shell_difference(
                      *session, id, csgify::ReportIdMode::local)
                : use_pipe_annulus
                    ? csgify::decompose_pipe_annulus(
                          *session, id, csgify::ReportIdMode::local)
                    : use_revolution
                        ? csgify::decompose_revolution(
                              *session, id, csgify::ReportIdMode::local)
                        : use_prismatic
                    ? csgify::decompose_prismatic(*session, id,
                                                  csgify::ReportIdMode::local)
                    : csgify::auto_decompose_bsp(*session, id,
                                            static_cast<int>(max_depth_value),
                                            policy,
                                            csgify::ReportIdMode::local,
                                            /*defer_analysis=*/true);
            if (r.ok) {
                ++succeeded;
                if (use_prismatic) ++prismatic_applied;
                if (use_shell_difference) ++shell_differences_applied;
                if (use_pipe_annulus) ++pipe_annuli_applied;
                if (use_revolution) ++revolutions_applied;
            } else {
                const bool stuck_on_confidence = std::any_of(
                    r.report.diagnostics.begin(), r.report.diagnostics.end(),
                    [](const csgify::Diagnostic& d) {
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
                  << ", prismatic=" << prismatic_applied
                  << ", shell-difference=" << shell_differences_applied
                  << ", pipe-annulus=" << pipe_annuli_applied
                  << ", revolution=" << revolutions_applied
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
    csgify::CommandResult step_result =
        csgify::export_document_step(*session, document_id, step_out_path,
                                      csgify::ReportIdMode::local);

    // Re-analyze once so the closing summary and the on-disk report
    // both reflect the final post-mutation state; this also picks up
    // the export_step_ms accumulated by the call above.
    csgify::CommandResult final_analysis =
        csgify::analyze(*session, document_id, csgify::ReportIdMode::local);

    // Closing "what's left for you" summary. After all safe ops, count
    // solids that still need attention. This is the user-facing answer
    // to "what does --safe-all leave behind?".
    if (safe_all) {
        std::size_t still_needs = 0;
        std::size_t still_decisions = 0;
        for (const csgify::AnalyzedSolid& s : final_analysis.report.solids) {
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
    csgify::HoleFillOptions hole_options;
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

    csgify::ConversionOptions options;
    csgify::SessionPtr session = csgify::create_session(options);
    const csgify::DocumentId document_id =
        csgify::load_step(*session, input_path);

    if (kind == "decompose") {
        csgify::DecompositionProposal p =
            csgify::propose_decomposition(*session, target_id,
                                            csgify::ReportIdMode::local);
        if (!report_path.empty()) {
            if (write_decomp_proposal_or_stdout(p, report_path) != 0) {
                std::cerr << "error report_write_failed: " << report_path << '\n';
                return EXIT_FAILURE;
            }
        } else {
            std::cout << csgify::decomposition_proposal_json_string(p);
        }
        std::cerr << "propose decompose: " << p.candidates.size()
                  << " candidate(s), best: "
                  << (p.best_candidate_id.empty() ? "-" : p.best_candidate_id)
                  << ", ok: " << (p.ok ? "true" : "false") << '\n';
        return p.ok ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (kind == "surface") {
        csgify::SurfaceReplacementProposal p =
            csgify::propose_surface_replacement(*session, target_id,
                                                  csgify::ReportIdMode::local);
        if (!report_path.empty()) {
            if (write_surface_proposal_or_stdout(p, report_path) != 0) {
                std::cerr << "error report_write_failed: " << report_path << '\n';
                return EXIT_FAILURE;
            }
        } else {
            std::cout << csgify::surface_replacement_proposal_json_string(p);
        }
        std::cerr << "propose surface: " << p.candidates.size()
                  << " candidate(s), best: "
                  << (p.best_candidate_id.empty() ? "-" : p.best_candidate_id)
                  << ", ok: " << (p.ok ? "true" : "false") << '\n';
        return p.ok ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (kind == "removal") {
        csgify::RemovalProposal p =
            csgify::suggest_removals(*session, document_id,
                                       csgify::ReportIdMode::local);
        if (!report_path.empty()) {
            if (write_removal_proposal_or_stdout(p, report_path) != 0) {
                std::cerr << "error report_write_failed: " << report_path << '\n';
                return EXIT_FAILURE;
            }
        } else {
            std::cout << csgify::removal_proposal_json_string(p);
        }
        std::cerr << "propose removal: " << p.candidates.size()
                  << " candidate(s), high-confidence: " << p.high_confidence_count
                  << ", ok: " << (p.ok ? "true" : "false") << '\n';
        return p.ok ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    // kind == "hole-fill"
    csgify::HoleFillProposal p =
        csgify::propose_hole_fills(*session, document_id, hole_options,
                                     csgify::ReportIdMode::local);
    if (!report_path.empty()) {
        if (write_hole_fill_proposal_or_stdout(p, report_path) != 0) {
            std::cerr << "error report_write_failed: " << report_path << '\n';
            return EXIT_FAILURE;
        }
    } else {
        std::cout << csgify::hole_fill_proposal_json_string(p);
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
    std::string step_out_path;
    OptScanner scan(argc, argv, 5);
    while (!scan.done()) {
        if (scan.take_string("--out", &output_path)) continue;
        if (scan.take_string("--report", &report_path)) continue;
        if (scan.take_string("--log", &log_path)) continue;
        if (scan.take_string("--candidate", &candidate_id)) continue;
        if (scan.take_string("--out-step", &step_out_path)) continue;
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

    csgify::ConversionOptions options;
    csgify::SessionPtr session = csgify::create_session(options);
    if (!log_path.empty()) {
        try {
            csgify::attach_decision_log(*session, log_path, input_path);
        } catch (const std::exception& e) {
            std::cerr << "error decision_log_open_failed: " << e.what() << '\n';
            return EXIT_FAILURE;
        }
    }
    const csgify::DocumentId document_id =
        csgify::load_step(*session, input_path);

    csgify::CommandResult action_result;
    if (kind == "decompose") {
        action_result = csgify::apply_decomposition_candidate(
            *session, target_id, candidate_id, csgify::ReportIdMode::local);
    } else if (kind == "surface") {
        action_result = csgify::apply_surface_replacement_candidate(
            *session, target_id, candidate_id, csgify::ReportIdMode::local);
    } else if (kind == "removal") {
        action_result = csgify::remove_solid(
            *session, target_id, csgify::ReportIdMode::local);
    } else if (kind == "hole-fill") {
        action_result = csgify::fill_hole(
            *session, document_id, candidate_id, csgify::ReportIdMode::local);
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

    // Write the as-applied geometry before emit so callers can chain another
    // pass (remove -> decompose -> surface -> emit) off the same STEP, matching
    // simplify --out-step. emit does not mutate, so export order is moot, but we
    // keep simplify's ordering for consistency.
    if (!step_out_path.empty()) {
        csgify::CommandResult step_result =
            csgify::export_document_step(*session, document_id, step_out_path,
                                          csgify::ReportIdMode::local);
        if (!step_result.ok) {
            print_diagnostics(step_result.report);
            return EXIT_FAILURE;
        }
    }

    csgify::CommandResult emit_result =
        csgify::emit_document(*session, document_id, output_path,
                                csgify::ReportIdMode::local);
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
    std::cerr << "csgify: OCCT " << csgify::occt_version()
              << ", Alea " << alea_version() << '\n';

    csgify::ConversionOptions options;
    csgify::SessionPtr session = csgify::create_session(options);
    const csgify::DocumentId document_id = csgify::load_step(*session, argv[1]);

    // The default one-shot convert must run the explicit analyze pass
    // (which includes overlap detection) before emit, otherwise the
    // overlap diagnostics would never be added to the model and the
    // subsequent emit would silently write a broken MCNP deck. Verbs
    // like `simplify` and `apply` are iterative and trust the user to
    // resolve overlaps step by step; the default convert path has no
    // such hand-off.
    csgify::analyze(*session, document_id, csgify::ReportIdMode::local);

    csgify::CommandResult result =
        csgify::emit_document(*session,
                               document_id,
                               argv[2],
                               csgify::ReportIdMode::local);

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
    return csgify::replay_recipe(recipe_path, step_override,
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
    bool preserve_step_exports = false;
    OptScanner scan(argc, argv, 3);
    while (!scan.done()) {
        if (scan.take_string("--out", &recipe_path)) continue;
        if (scan.take_flag("--preserve-step-exports")) {
            preserve_step_exports = true;
            continue;
        }
        std::cerr << "error unrecognized_option: " << scan.peek() << '\n';
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (recipe_path.empty()) {
        std::cerr << "error: compact requires --out path\n";
        return EXIT_FAILURE;
    }
    csgify::CompactRecipeOptions options;
    options.preserve_step_exports = preserve_step_exports;
    return csgify::compact_log_to_recipe(log_path, recipe_path, options) == 0
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

    csgify::ConversionOptions options;
    csgify::CadModel model = csgify::load_step_model(input_path, options);

    for (const csgify::Diagnostic& d : model.diagnostics) {
        std::cerr << severity_name(d.severity) << " " << d.code
                  << ": " << d.message << '\n';
    }
    if (!model.loaded()) {
        std::cerr << "error: could not load " << input_path << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "# solids (" << model.solid_metadata.size() << ")\n";
    std::cout << "# shape_id\tcomponent_entry\tname\thierarchy_path\n";
    for (const csgify::ShapeMetadata& m : model.solid_metadata) {
        std::cout << m.shape_id << '\t'
                  << (m.component_entry.empty() ? "-" : m.component_entry) << '\t'
                  << (m.name.empty() ? "-" : m.name) << '\t'
                  << (m.hierarchy_path.empty() ? "-" : m.hierarchy_path) << '\n';
    }

    std::cout << "\n# assembly nodes (" << model.assembly.size() << ")\n";
    std::cout << "# id\tparent\tkind\tshape_id\tlabel_entry\tpath\n";
    for (const csgify::AssemblyNode& n : model.assembly) {
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

std::string solid_status_label(const csgify::AnalyzedSolid& solid) {
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

void print_solid_tree_line(const csgify::AnalyzedSolid& solid,
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
    const csgify::AssemblyNode& node,
    const std::unordered_map<std::string, std::vector<const csgify::AssemblyNode*>>& children,
    const std::unordered_map<std::string, const csgify::AnalyzedSolid*>& solid_by_id,
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
    const std::vector<const csgify::AssemblyNode*>& node_children = child_it->second;
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

    csgify::ConversionOptions options;
    csgify::SessionPtr session = csgify::create_session(options);
    const csgify::DocumentId doc_id = csgify::load_step(*session, input_path);
    csgify::CommandResult result =
        csgify::analyze(*session, doc_id, csgify::ReportIdMode::scoped);
    const csgify::ConversionReport& report = result.report;

    for (const csgify::Diagnostic& d : report.diagnostics) {
        std::cerr << severity_name(d.severity) << " " << d.code
                  << ": " << d.message << '\n';
    }

    std::unordered_map<std::string, const csgify::AnalyzedSolid*> solid_by_id;
    for (const csgify::AnalyzedSolid& solid : report.solids) {
        solid_by_id[solid.id] = &solid;
    }

    std::unordered_map<std::string, std::vector<const csgify::AssemblyNode*>> children;
    std::vector<const csgify::AssemblyNode*> roots;
    for (const csgify::AssemblyNode& node : report.assembly) {
        children[node.parent_id].push_back(&node);
    }
    auto root_it = children.find("");
    if (root_it != children.end()) {
        roots = root_it->second;
    }

    std::cout << "# csgify tree\n";
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

    std::vector<const csgify::AnalyzedSolid*> unlisted;
    for (const csgify::AnalyzedSolid& solid : report.solids) {
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
    return csgify::run_lua_script(argv[2], argc - 3, argv + 3) == 0
        ? EXIT_SUCCESS
        : EXIT_FAILURE;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
        std::cout << "csgify: OCCT " << csgify::occt_version()
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
    if (verb == "handoff") return run_handoff_command(argc, argv);
    if (verb == "voids") return run_voids_command(argc, argv);
    if (verb == "coverage") return run_coverage_command(argc, argv);
    if (verb == "project") return run_project_command(argc, argv);
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
    if (verb == "shatter") return run_shatter_command(argc, argv);
    if (verb == "compact") return run_compact_command(argc, argv);
    if (verb == "replay") return run_replay_command(argc, argv);
    if (verb == "volume")  return run_volume_command(argc, argv);
    if (verb == "simplify") return run_simplify_command(argc, argv);
    if (verb == "auto-fit") return run_auto_fit_command(argc, argv);
    if (verb == "propose") return run_propose_command(argc, argv);
    if (verb == "apply") return run_apply_command(argc, argv);
    if (verb == "autonomy") return run_autonomy_command(argc, argv);
    if (verb == "context") return run_context_command(argc, argv);
    if (verb == "queue") return run_queue_command(argc, argv);
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

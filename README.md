# csgify

`csgify` converts STEP CAD models into Alea CSG systems for neutronics workflows.

This first engine pass targets Open CASCADE Technology 8.0.0 APIs and links to
`../libalea`. CMake requires `OpenCASCADE 8.0.0 EXACT`, so older system OCCT
packages are rejected.

Current scope:

- import STEP through OCCT
- optionally run OCCT shape healing
- traverse solids and faces
- convert plane, sphere, and cylinder faces to Alea halfspaces
- represent arbitrary cylinder axes as Alea general quadrics
- build one Alea cell per converted solid
- export the resulting system to MCNP through libalea
- emit structured diagnostics for unsupported spline and other analytic faces

Project style is described in [docs/PHILOSOPHY.md](docs/PHILOSOPHY.md): small
core, explicit data flow, command-line first, and useful failure reports.
The broader implementation plan is in
[docs/IMPLEMENTATION_PLAN.md](docs/IMPLEMENTATION_PLAN.md).
The non-convex path is tracked in [docs/DECOMPOSITION.md](docs/DECOMPOSITION.md).
Certified LLM action queues are documented in
[docs/AUTONOMY.md](docs/AUTONOMY.md).
The CLI-only prototype workflow for Codex, Pi, and Claude is documented in
[docs/CLI_AGENT_PROTOTYPE.md](docs/CLI_AGENT_PROTOTYPE.md); it does not require
an MCP server or embedded model runtime.

The autonomous workflow now includes a versioned `context` bundle for external
LLMs, durable `autonomy --resume` queues, audited human rejections through
`queue reject`, and a Qt multi-item geometry-review queue. Geometry mutations
remain gated by freshly regenerated OCCT evidence and the deterministic safety
policy.

`project init` creates a conservative, versioned `project-policy.json` without
overwriting user changes. `context --project` and fresh `autonomy --project`
discover it automatically; `--policy-file` selects an explicit external file.
The policy binds import units and healing settings to the authority and
approximation budgets advertised to an external planner.

With the Qt build, `csgify project review` opens a New/Open launcher. The
workspace can select a STEP source, display and apply the effective project
preparation policy during asynchronous import, edit the supported policy
settings without dropping advanced restrictions, and export reviewed STEP or
gated MCNP output without closing the project. Successful exports are also
copied into the project's immutable artifact store.

The project workspace also has a persistent advisory conversation dock. Set
`OPENAI_API_KEY` and `CSGIFY_OPENAI_MODEL` before launching to use the embedded
OpenAI Responses provider; without both variables it uses a clearly labelled
simulated provider. The current slice answers revision-bound geometry
questions and never mutates the model. Setup and privacy boundaries are in
[docs/AGENT_WORKFLOW.md](docs/AGENT_WORKFLOW.md#embedded-project-conversation).

The Qt queue labels its active authority as Advisory, Assisted, or Delegated
and keeps model-written explanation separate from authoritative engine
evidence and deterministic policy results. Every pending item shows its
evaluated and current document revisions. A revision mismatch is visibly
**stale**, disables review, and requires asynchronous evidence regeneration.

## OCCT 8 setup

The `/home/giovanni/OCCT-8_0_0` directory is the OCCT source tree. Build and
install it into a separate prefix first:

```sh
cmake -S /home/giovanni/OCCT-8_0_0 \
  -B /home/giovanni/OCCT-8_0_0-build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/home/giovanni/OCCT-8_0_0-install \
  -DINSTALL_DIR=/home/giovanni/OCCT-8_0_0-install \
  -DBUILD_MODULE_Draw=OFF \
  -DBUILD_MODULE_Visualization=OFF

cmake --build /home/giovanni/OCCT-8_0_0-build --parallel 2
cmake --install /home/giovanni/OCCT-8_0_0-build
```

`csgify` requires `OpenCASCADE 8.0.0 EXACT`, so CMake will reject the system
OCCT 7.8.1 package.

Build `csgify`:

```sh
cmake --preset occt8
cmake --build --preset occt8
```

Configuration compiles and links an Alea API probe before generating the main
build. If `../libalea/include` is newer than `../libalea/bin`, rebuild libalea;
csgify deliberately refuses a mismatched header/archive pair.

Run:

```sh
./build/csgify input.step output.i
```

Inspect a model before conversion:

```sh
./build/csgify inspect input.step --report report.json
```

Open the Qt 6 human-decision viewer (when Qt6 Widgets was found by CMake):

```sh
./build/csgify viewer-check
./build/csgify view input.step --select solid:1 --candidate best \
  --log decisions.json --out reviewed.i
```

Opening `view` without a precomputed proposal is an actionable inspection
workflow. Selecting a shape marked **Needs decomposition** explains the
blocker and enables **Generate decomposition options**. **Resolve all
certifiable shapes** runs the bounded fixed-point coordinator across the
document; it applies only engine-certified changes and leaves uncertain
residuals unchanged for explicit review.

The window embeds the OCCT view beside the model tree and proposal evidence.
An adaptive XY reference floor and a color-coded XYZ trihedron provide scale
and orientation references; both can be toggled from the review panel. The
floor is rendered behind geometry and placed below the model bounds, so it
does not cross selected parts. Selection
is cyan and remains visually distinct from proposal, warning, overlap, and
provisional job states. Decomposition candidates have non-mutating
Before/Preview/Verified-result modes; the
preview colors exact, residual, and sliver pieces separately and verifies its
piece count and volume against the proposal.
Accept/Reject remain human decisions; `Ctrl+Enter` deliberately accepts a
non-removal proposal, while removal always uses its scoped confirmation.
Risky applicable geometry displays its
volume/deviation/residual metrics and asks for confirmation. Accepted changes
and Undo use the normal session journal and decision log. Use
`--viewer-backend x11` for the legacy frontend, or configure with
`-DCSGIFY_ENABLE_QT_VIEWER=OFF` to build without Qt.

`inspect` writes a machine-readable report with summary counters, diagnostics,
and allowed actions for human or agent preprocessing. It does not create Alea
cells or export neutronics input.

For composed preprocessing, spline approximation is opt-in and bounded:

```sh
./build/csgify preprocess input.step --safe-all --max-deviation 1e-6 \
  --out-step simplified.step --report report.json
```

Without `--max-deviation`, `--safe-all` leaves spline faces untouched and
reports them as remaining decisions. Feature-removal candidates are also left
untouched unless `--apply-removal-candidates high|medium` is supplied.

The standalone `auto-fit` command always requires `--max-deviation`; there is
no unbounded batch-fitting mode in the CLI or Lua API.

The report includes stable traversal IDs for decisions:

- solids: `solid:1`, `solid:2`, ...
- faces: `face:1.4` means solid 1, face 4

Diagnostics include `shape_id`, and the report contains `solids[]` and `faces[]`
sections with volume, area, bounding box, surface type, and convertibility flags.
`solids[]` also includes semantic-cleanup metrics such as `volume_fraction`,
`bbox_diagonal_fraction`, bbox extents/aspect ratio, and `cleanup_candidates`
flags for small-volume, small-bbox, thin-plate, and long-body solids.
It also reports `replacement_candidates` for currently supported simple
replacements, including bbox, conservative bbox-cylinder, fitted cylinder,
fitted sphere, and fitted cone volume metrics. These are decision metrics, not
automatic approval.

Calculate volume, or compare original and simplified models:

```sh
./build/csgify volume input.step
./build/csgify volume original.step --compare simplified.step --rel-tol 1e-4
./build/csgify volume original.step --compare simplified.step --abs-tol 0.01
```

The comparison command exits with success only when the absolute and relative
tolerances that were provided are both satisfied. If no tolerance is provided,
the command reports the delta and requires exact equality.

Run a first in-memory simplification and export:

```sh
./build/csgify simplify input.step --replace-solid-bbox solid:1 --out simplified.i
./build/csgify simplify input.step --replace-solid-bbox-cylinder solid:1 --out simplified.i
./build/csgify simplify input.step --replace-solid-fitted-cylinder solid:1 --out simplified.i
./build/csgify simplify input.step --replace-solid-fitted-sphere solid:1 --out simplified.i
./build/csgify simplify input.step --replace-solid-fitted-cone solid:1 --out simplified.i
./build/csgify simplify input.step --decompose-planar solid:1 --out simplified.i
./build/csgify simplify input.step --remove-solid solid:1 --out simplified.i
```

`--replace-solid-bbox` suppresses the selected CAD solid and adds a synthetic
axis-aligned box from its bounding box. Synthetic bodies are emitted directly to
Alea; no decision file is involved yet.
`--replace-solid-bbox-cylinder` suppresses the selected CAD solid and adds a
conservative axis-aligned cylinder: the longest bbox axis becomes the cylinder
height and the larger transverse bbox extent sets the radius. It reports the
original/replacement volume delta.
`--replace-solid-fitted-cylinder`, `--replace-solid-fitted-sphere`, and
`--replace-solid-fitted-cone` use the dominant matching OCCT analytic face as
the primitive source and report the volume delta before export. Fitted cones are
exported as an explicit CSG intersection of a cone side and two cap planes.
Axis-aligned cones use libalea cone surfaces, producing MCNP `KX`, `KY`, `KZ`
or translated `K/X`, `K/Y`, `K/Z` cards. Arbitrary-axis cones fall back to a
general quadric `GQ` side. Neither path requires libalea's TRC macrobody
exporter.

Replacement commands also accept volume guards:

```sh
./build/csgify simplify input.step --replace-solid-fitted-cylinder solid:1 \
  --out simplified.i --max-rel-delta 0.01
./build/csgify simplify input.step --replace-solid-bbox solid:1 \
  --out simplified.i --max-abs-delta 0.001
./build/csgify simplify input.step --replace-solid-fitted-cylinder solid:1 \
  --out simplified.i --report simplified-report.json
```

When a guard is provided, the command checks the replacement candidate metrics
from the analysis report before modifying the model. `--report` writes the
post-preprocessing conversion report for agent tooling.

The same guard logic is available through the C++ API:
`replacement_metrics_for_operation(...)` reads metrics from an analyzed solid,
and `check_replacement_limits(...)` validates a proposed operation against
`ReplacementLimits`. `apply_replacement_operation(...)` applies an explicit
replacement, while `apply_checked_replacement_operation(...)` validates limits
and mutates the model only if the check passes. This keeps future Lua or agent
bindings on the same decision path as the CLI.

For production approximation, prefer the cumulative symmetric-difference
budget. Percentages require an explicit `%` suffix; equal signed volume does
not hide moved or exchanged geometry:

```sh
./build/csgify simplify input.step --replace-solid-fitted-cylinder solid:1 \
  --out simplified.i --approx-volume 1% --max-deviation 0.05

# Presets expand to explicit report fields: conservative=0.1%, moderate=1%,
# permissive=2%. Per-action/body and added/removed limits can be tighter.
./build/csgify simplify input.step --shatter solid:1 --out simplified.i \
  --approx-preset conservative --approx-action-volume 0.05%
```

Accepted actions accumulate `added + removed` volume in a document and
source-body ledger. Failed actions roll back geometry and ledger together.
`--protect solid:N` and `--protect-material ID` are hard approximation
barriers. Approximation never relaxes the production MCNP overlap gate.

Small input overlaps can instead be repaired by assigning the shared region to
one component. `decompose-apply --approx-volume P%` does this automatically
using stable-ID ownership, charges the exact OCCT subtraction to the losing
component's ledger, decomposes the cut result, and still requires zero overlap
at final emission. For an explicit choice use:

```sh
./build/csgify simplify input.step --resolve-overlap solid:3 \
  --owner solid:1 --out intermediate.i --out-step intermediate.step \
  --approx-volume 0.1%
```

The explicit single-step command may correctly stop before MCNP emission when
the cut result needs decomposition; its STEP/log can be continued, or the
document operation can be followed by `shatter` through Lua/replay.

MCNP output is installed atomically only after geometry safety passes. Positive
cell overlap and unknown Boolean membership always block. Coverage is reported
as `not_configured` unless the user supplies a real non-emitting modeled-domain
container; it is never inferred from the assembly bounding box:

```sh
./build/csgify simplify model.step --shatter solid:2 --out model.i \
  --mcnp-domain solid:1 --require-domain-coverage
```

With a domain, csgify measures `domain - union(explicit material/void cells)`,
checks cells outside the domain, and combines that with the mandatory overlap
test. Status and measured volumes are written under
`mcnp_geometry_safety`. Intentional empty regions must be explicit cells (for
example material ID 0 / zero density), otherwise they are classified as gaps.

Every session mutation is atomic. Successful operations create a monotonic
document revision; failed operations and geometry that introduces overlaps are
rolled back. C++, C, and Lua expose the current revision and undo operation:

```lua
print(s:document_revision(doc)) -- 0 after load
s:remove_solid(doc .. "/solid:2")
s:undo_last_mutation(doc)       -- restored state, new revision
```

For large cleanup sets, `remove_solids` suppresses every listed stable ID as
one atomic mutation and runs the complete post-mutation analysis once:

```lua
s:remove_solids({
    doc .. "/solid:2",
    doc .. "/solid:7",
    doc .. "/solid:19",
})
```

Machine-readable outputs carry explicit schema names and versions. Conversion
reports include `geometry_certificate` records for solids and `provenance[]`
records connecting replacements, decomposed pieces, Boolean bodies, and
removals to source IDs and tolerance budgets. Lua can inspect the same central
operation contract with `csgify.operations()`.

Phase 2 has started with explicit planar decomposition:
`--decompose-planar` splits a selected solid using its planar faces, suppresses
the original solid, and converts the resulting pieces. The command writes
volume-preservation diagnostics and a structured `decomposed_pieces[]` report
section with per-piece volume, bbox, face count, and convertibility flags.
Directed splits are volume-guarded, and final pieces must pass Boolean
face-halfspace containment certificates. Intermediate pieces may still require
further decomposition or surface replacement; uncertified mutations are
rejected atomically.
Axis-aligned torus faces are supported through libalea `TX`, `TY`, and `TZ`
surfaces; arbitrary-axis tori still require replacement or a future
transform-aware emitter.

The engine deliberately stops on unresolved spline faces. It proposes analytic
fits and applies them only under an explicit per-operation deviation budget;
otherwise the face remains a recorded user decision.

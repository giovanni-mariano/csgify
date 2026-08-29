# csgify Implementation Plan

`csgify` converts CAD geometry into CSG geometry for neutronics. The hard part
is not STEP import or file export. The hard part is making CAD explicit enough
that every final Alea cell has a defensible CSG meaning.

The software must therefore be built around a preprocessing phase. This phase
has two different parts:

- semantic cleanup, where a human or agent decides what CAD detail matters for
  the neutronics model
- geometric repair, where deterministic algorithms replace unsupported surfaces
  and decompose solids into CSG-convertible pieces

The final conversion to Alea should be boring. It should receive already
convertible pieces and emit Alea surfaces, nodes, cells, materials, and export
files. If the geometry is not ready, conversion should refuse it.

## Goals

- Convert STEP models through OCCT 8.
- Use `../libalea` as the CSG and neutronics export backend.
- Support human-in-the-loop and agent-in-the-loop preprocessing.
- Make every destructive or approximate CAD operation explicit.
- Prefer exact analytic CSG conversion when possible.
- Allow approximations only when they are recorded with tolerances and accepted.
- Reject geometry that cannot be converted safely.
- Keep the implementation small, inspectable, and command-line first.

## Non-Goals

- Do not silently mesh splines and pretend the result is exact CSG.
- Do not emit one Alea cell for a non-convex solid unless it has been decomposed
  into valid pieces.
- Do not hide preprocessing decisions inside GUI state.
- Do not require a large service stack for normal use.
- Do not build a general CAD editor.

## Architecture

The engine should be split into six explicit stages:

1. Import

   Read STEP with OCCT 8, heal if requested, and build an internal model index.
   No Alea objects are created here.

2. Analyze

   Classify solids, shells, faces, edges, materials, surface types, topology,
   bounding boxes, volumes, and convertibility. Produce diagnostics and proposed
   actions.

3. Semantic Cleanup

   Apply human or agent decisions about modeling intent: remove bolts, labels,
   washers, logos, tiny fasteners, decorative details, or other CAD components
   that are not relevant to the neutronics problem. Add simplified replacement
   bodies when the removed component should still contribute material volume.

4. Geometric Repair

   Replace splines, simplify small geometric features, split/decompose solids,
   and validate the resulting pieces. This stage should rely on deterministic
   OCCT/traditional geometry algorithms where possible.

5. Emit CSG

   Convert validated pieces into Alea surfaces and cells. This stage should be
   strict and simple.

6. Export

   Export through libalea to MCNP, OpenMC, Serpent, or other supported formats.

The current implementation already has parts of Import, Analyze, limited
Semantic Cleanup, and Emit CSG. The next work should make those boundaries
explicit in the code.

## Core Data Model

The project should introduce plain structs for the analyzed CAD model. These
should be independent from Alea and should wrap OCCT shape handles only where
needed.

Suggested structs:

```cpp
struct ShapeId {
    uint64_t value;
};

struct CadModel {
    TopoDS_Shape root;
    std::vector<CadSolid> solids;
    std::vector<CadFace> faces;
    std::vector<CadEdge> edges;
    std::vector<MaterialAssignment> materials;
};

struct CadSolid {
    ShapeId id;
    TopoDS_Solid shape;
    double volume;
    Bnd_Box bbox;
    std::vector<ShapeId> face_ids;
    Convertibility convertibility;
};

struct CadFace {
    ShapeId id;
    ShapeId solid_id;
    TopoDS_Face shape;
    SurfaceKind surface_kind;
    AnalyticSurface analytic;
    double area;
    bool used_by_outer_boundary;
};

struct Convertibility {
    bool exact_csg_possible;
    bool needs_spline_replacement;
    bool needs_decomposition;
    bool needs_user_decision;
    std::vector<Diagnostic> diagnostics;
};
```

Important detail: shape IDs must be stable enough for reports and decision
files. OCCT `TopoDS_Shape` identity is not enough by itself after healing or
splitting. A practical first version can assign deterministic IDs from traversal
order plus geometric fingerprints. Later, IDs can include STEP entity references
when available.

## Diagnostics and Decisions

All uncertain work should be exposed as diagnostics and decision requests. For
now, decisions can be direct API calls or CLI flags. A decision file can be added
later, once the operation vocabulary has stabilized.

Diagnostics describe facts:

```json
{
  "severity": "error",
  "code": "unsupported_spline_face",
  "shape_id": "face:42",
  "message": "B-spline face cannot be emitted as exact CSG.",
  "actions": ["replace_with_plane", "replace_with_cylinder", "approximate", "keep_unconverted"]
}
```

Decisions record what a human or agent chose:

```json
{
  "shape_id": "face:42",
  "action": "replace_with_cylinder",
  "parameters": {
    "max_distance": 0.02,
    "radius": 14.0,
    "axis": [0, 0, 1]
  },
  "accepted_by": "user",
  "reason": "Manufacturing fillet is irrelevant for neutron transport model."
}
```

If a decision file is added later, it should be append-only or at least
diff-friendly. JSON is a reasonable export/import format for agents, but it
should not become the internal control plane.

## Command-Line Workflow

The CLI should support a pipeline that can be used manually or by agents. The
current implementation intentionally starts with direct operations instead of a
decision-file parser:

```sh
csgify inspect input.step --report report.json
csgify simplify input.step --remove-solid solid:12 --out clean.i
csgify simplify input.step --replace-solid-bbox solid:12 --out clean.i
csgify volume original.step --compare simplified.step --rel-tol 1e-4
```

Short form can remain:

```sh
csgify input.step output.i
```

But the short form should only succeed when no decisions are required. If the
model needs simplification, spline replacement, or decomposition choices, it
should stop and point to the report.

Useful CLI flags:

- `--format mcnp|openmc|serpent`
- `--report path.json`
- `--dry-run`
- `--strict`
- `--allow-approximation`
- `--max-error value`
- `--feature-size value`
- `--material-map path.json`
- `--dump-brep path.brep`

## Agentic AI Contract

Agentic tools should not need to understand OCCT internals to help. The tool
should give them a structured problem:

- stable shape IDs
- screenshots or lightweight previews when available
- exact diagnostics
- allowed actions
- numeric parameters and tolerances
- consequences of each action
- validation results after action application

The agent loop should look like this:

1. Run `csgify inspect`.
2. Read the human text output or optional report.
3. Identify semantic cleanup candidates: useless bolts, labels, tiny hardware,
   irrelevant components, or detailed parts that should become simple bodies.
4. Apply direct operations such as `remove_solid` or `replace_solid_with_bbox`.
5. Run volume comparison and inspect again.
6. Ask the human only for ambiguous or high-impact choices.
7. Continue to spline replacement, decomposition, and final conversion.

For human users, the same flow can be wrapped in a GUI or notebook later. The
command-line contract should come first.

## Preprocessing Phase

Preprocessing should be implemented as small operations that transform a CAD
model and record their result.

### Semantic Cleanup

Semantic cleanup comes before spline replacement and decomposition. The agent or
human decides which CAD components matter for the neutronics model. Traditional
geometry algorithms can identify candidates, but they cannot know the modeling
intent.

For neutronics, many CAD details are not meaningful:

- screws, washers, handles, labels, logos
- tiny holes below transport-relevant size
- chamfers and fillets below tolerance
- duplicate or hidden construction parts
- internal details inside homogeneous material regions

The engine should not guess too aggressively. It should detect candidates and
ask for removal unless the user supplies a policy.

Candidate detection should expose simple metrics:

- small volume relative to model or parent assembly
- small bounding box diagonal
- volume fraction
- bounding box diagonal fraction
- thin/long shape ratios
- material marked as ignored
- disconnected component outside region of interest
- name/path pattern from STEP metadata
- repeated tiny fasteners

Actions:

- `remove_solid`
- `remove_component`
- `replace_solid_with_bbox`
- `replace_solid_with_bbox_cylinder`
- `replace_solid_with_fitted_cylinder`
- `replace_solid_with_fitted_sphere`
- `replace_solid_with_fitted_cone`
- `replace_solid_with_synthetic_csg`
- `merge_with_parent_material`
- `keep`

Validation:

- removed volume summary
- removed material summary
- bounding box impact
- list of cells affected
- replacement volume delta
- replacement fill ratio

Some choices are obvious under a user policy. For example, if a component is a
tiny repeated fastener below a configured volume fraction and material
importance threshold, an agent can remove or replace it automatically. The
operation should still be logged in diagnostics or command output.

### Synthetic Replacement Bodies

When a component is removed, it may still need a simplified material volume. The
core API should support explicit synthetic bodies:

- box
- sphere
- cylinder
- cone/frustum
- union
- intersection
- difference

Synthetic bodies should enter the same Alea emission path as preprocessed CAD
pieces. They are not export hacks. Current implemented functions include:

- `add_synthetic_box`
- `add_synthetic_sphere`
- `add_synthetic_cylinder`
- `add_synthetic_cone`
- `add_synthetic_union`
- `add_synthetic_intersection`
- `add_synthetic_difference`

Implemented replacement helpers include:

- `replace_solid_with_bbox`
- `replace_solid_with_bbox_cylinder`
- `replace_solid_with_fitted_cylinder`
- `replace_solid_with_fitted_sphere`
- `replace_solid_with_fitted_cone`

`replace_solid_with_fitted_cone` does not depend on libalea's TRC macrobody
export. csgify emits a fitted cone/frustum as an explicit CSG intersection of
a cone side and two cap planes. Axis-aligned cones use libalea cone surfaces
and export as MCNP `KX`, `KY`, `KZ`, `K/X`, `K/Y`, or `K/Z`; arbitrary-axis
cones fall back to a general quadric `GQ` side. A native TRC exporter in libalea
would still be useful later because it would produce shorter MCNP output.

Each replacement should report:

- original volume
- replacement volume
- absolute volume delta
- relative volume delta
- fill ratio where meaningful
- material assignment
- reason or policy that selected it

The CLI can enforce replacement volume guards before applying a preprocessing
operation:

- `--max-abs-delta value`
- `--max-rel-delta value`
- `--report report.json`

These guards are deliberately simple and deterministic. They let a human or
agent propose an operation while the engine rejects replacements outside the
accepted volume error budget. The report option writes the post-preprocessing
conversion report so an agent can validate what was actually emitted.

The guard mechanism is part of the engine API, not only the CLI:

- `ReplacementOperation`
- `ReplacementLimits`
- `ReplacementCheck`
- `replacement_metrics_for_operation`
- `check_replacement_limits`
- `apply_replacement_operation`
- `apply_checked_replacement_operation`

The current analysis report includes `replacement_candidates` for bbox,
conservative bbox-cylinder, fitted cylinder, fitted sphere, and fitted cone
replacements. These are metrics only; they do not mean the replacement is
automatically correct. A user policy or agent decision must still choose whether
to apply the operation.

### Replace Splines

CSG for neutronics needs analytic or controlled approximate surfaces. Splines
must be handled before Alea emission, but after semantic cleanup has removed
useless components. There is no point replacing spline faces on a bolt or label
that will be removed.

Spline classification:

- nearly planar
- nearly cylindrical
- nearly spherical
- nearly conical
- toroidal or fillet-like
- freeform surface

Replacement actions:

- `replace_with_plane`
- `replace_with_cylinder`
- `replace_with_sphere`
- `replace_with_cone`
- `replace_with_torus`
- `approximate_with_facets`
- `remove_feature`
- `manual`

Each replacement must record:

- fitted surface parameters
- maximum distance error
- RMS distance error
- area affected
- neighboring faces affected
- accepted tolerance
- whether approximation is exact enough for neutronics use

Implementation steps:

1. Use OCCT surface adaptors to detect B-spline and Bezier faces.
2. Sample the face in UV space.
3. Fit candidate analytic surfaces.
4. Score each candidate by max error, RMS error, normal error, and boundary
   compatibility.
5. Propose replacements when error is below policy threshold.
6. Require a decision when several fits are plausible or error is above default
   tolerance.
7. Rebuild the affected solid with the replacement surface.
8. Re-run BRep validity checks.

The first useful implementation can support only near-plane and near-cylinder
replacement. That covers many CAD artifacts without pretending to solve all
freeform geometry.

### Decompose Geometry

The Alea cell emitter should receive pieces that are representable as
intersections of halfspaces or supported macrobodies. Non-convex solids must be
decomposed before emission. Decomposition should happen after semantic cleanup
and after obvious spline replacements, so the decomposition algorithms operate
on a cleaner model.

Current guard:

- The engine validates whether all vertices of a solid lie inside the selected
  halfspaces of its faces.
- If not, the solid is rejected as `nonconvex_solid_requires_decomposition`.

Next decomposition layers:

1. Planar decomposition

   Use existing planar face extensions as cutting tools. This is already present
   as an explicit preprocessing operation through
   `decompose_solid_by_planar_faces` and
   `simplify --decompose-planar solid:N`. The operation suppresses the original
   solid, stores the generated pieces in the model, validates the summed piece
   volume against the original solid, reports the relative delta, and emits a
   structured `decomposed_pieces[]` report section with per-piece volume, bbox,
   face count, and convertibility flags. On OCCT's `screw.step` sample it
   currently produces 5 pieces with about `8.9e-7` relative volume delta; 2
   pieces are directly CSG-supported and 3 still need further decomposition.
   Axis-aligned torus faces in that sample are now supported.

2. Concavity-driven cuts

   Find concave edges and propose cutting planes from adjacent faces or local
   feature directions.

3. Primitive-aware decomposition

   Detect cylinders, cones, spheres, and tori that form holes or protrusions.
   Express the solid as base volume minus primitive volumes where possible.

4. Cell graph decomposition

   Use OCCT Boolean splitting to create candidate pieces. Validate each piece
   with the same convex CSG validator. Keep only valid pieces.

5. User-guided decomposition

   If automatic decomposition produces too many pieces or ambiguous cuts, report
   candidate cut surfaces and ask for a decision.

Actions:

- `split_by_plane`
- `split_by_existing_face`
- `split_by_cylinder`
- `split_by_box`
- `accept_piece_set`
- `reject_piece_set`
- `manual_cut_required`

Validation:

- every piece has positive volume
- every piece passes BRep validity
- every piece passes CSG convertibility
- union of pieces approximates original volume within tolerance
- pieces do not overlap except on boundaries
- total material assignment is preserved

## CSG Emission

Emission should stay strict. It should not perform hidden preprocessing.

Supported initial surfaces:

- plane
- sphere
- cylinder as axis-aligned Alea cylinder when possible
- cylinder as general quadric otherwise
- cone
- torus
- general quadric when exact and supported by Alea

Current implementation supports plane, sphere, cylinder-as-quadric, cone, and
axis-aligned torus faces. Arbitrary-axis torus faces still require replacement
or a future transform-aware emitter.

Emission algorithm:

1. For each preprocessed piece, find an interior point.
2. For each boundary face, create or reuse an Alea surface.
3. Select the halfspace containing the interior point.
4. Intersect all halfspaces to create the cell region.
5. Add material, density, comments, and metadata.
6. Build Alea indexes and run Alea simplification.
7. Export.

Surface reuse and deduplication should be handled through Alea where possible.
`csgify` should still keep enough metadata to explain which CAD face produced
which Alea surface.

## Validation

Validation must happen after every major stage.

Import validation:

- STEP read status
- transferred roots
- BRep validity
- solid count
- open shells
- zero-volume solids

Preprocess validation:

- no invalid BRep after operation
- no unaccepted spline replacements
- no unaccepted removals
- volume and bounding box deltas are reported
- material assignments still exist

Decomposition validation:

- pieces are valid solids
- pieces are CSG-convertible
- piece volume sum matches original within tolerance
- overlap checks pass or are explained

CSG validation:

- Alea cells exist for every emitted piece
- point samples classify consistently between OCCT and Alea
- exported file can be parsed back when the backend supports it
- bounding boxes are finite where required

Sampling checks are important. For each converted piece, sample points in its
bounding box and compare:

- OCCT says inside original preprocessed piece
- Alea says inside emitted cell

Disagreements should fail strict mode.

## File Outputs

Recommended outputs:

- `report.json`: diagnostics, stats, required decisions
- optional future `decisions.json`: accepted preprocessing choices, only if
  direct CLI/API operations become insufficient
- `clean.brep`: preprocessed OCCT model
- `mapping.json`: CAD shape ID to Alea surface/cell mapping
- `model.i`: MCNP output
- `model.xml`: OpenMC output when supported

`mapping.json` is important for debugging and agent workflows:

```json
{
  "cells": [
    {
      "alea_cell_id": 12,
      "source_solid": "solid:3",
      "source_piece": "piece:3.2",
      "material_id": 1
    }
  ],
  "surfaces": [
    {
      "alea_surface_id": 44,
      "source_face": "face:91",
      "surface_kind": "plane"
    }
  ]
}
```

## Code Organization

Suggested directories:

```text
include/csgify/
  engine.hpp
  diagnostics.hpp
  model.hpp
  preprocess.hpp
  emit.hpp

src/
  main.cpp
  import_step.cpp
  analyze.cpp
  preprocess.cpp
  spline_replace.cpp
  decompose.cpp
  emit_alea.cpp
  report_json.cpp
```

Keep dependencies one-way:

```text
import_step -> analyze -> preprocess -> emit_alea -> export
                         -> report_json
```

`emit_alea` should not call preprocessing code. If it sees unsupported geometry,
that is a bug in the earlier pipeline or a user decision still missing.

## Scriptable Core API

The CLI must stay a thin wrapper around a scriptable core. The first C++ layer is
now:

```cpp
CadModel load_step_model(const std::string& path, const ConversionOptions& options);
AnalysisReport analyze_model(const CadModel& model, const ConversionOptions& options);
```

This is not the final Lua API. It is the internal shape that the CLI, future C
ABI, and future Lua binding should all use. The C ABI should later wrap this
with opaque handles:

```c
csgify_model_t* csgify_load_step(const char* path);
csgify_report_t* csgify_analyze(csgify_model_t* model);
void csgify_model_destroy(csgify_model_t* model);
void csgify_report_destroy(csgify_report_t* report);
```

Preprocessing operations should be added first to the C++ core, then exposed to
the CLI, and only after that wrapped for Lua.

The first implemented preprocessing operations are:

- `remove_solid(model, "solid:1")`
- `replace_solid_with_bbox(model, "solid:1")`
- `replace_solid_with_bbox_cylinder(model, "solid:1")`
- `apply_replacement_operation(model, "solid:1", ReplacementOperation::fitted_cylinder)`
- `apply_checked_replacement_operation(model, "solid:1", ReplacementOperation::fitted_cylinder, limits)`
- `decompose_solid_by_planar_faces(model, "solid:1")`
- `add_synthetic_box(...)`
- `add_synthetic_sphere(...)`
- `add_synthetic_cylinder(...)`
- `add_synthetic_cone(...)`
- `add_synthetic_union(...)`
- `add_synthetic_intersection(...)`
- `add_synthetic_difference(...)`

`replace_solid_with_bbox` suppresses the CAD solid and adds a synthetic box body
that the Alea emitter can export as an RPP. `replace_solid_with_bbox_cylinder`
does the same with a conservative axis-aligned cylinder inferred from the
longest bbox axis. These are intentionally simple: they test the whole
remove-and-replace workflow without introducing a decision-file parser.

These functions are explicit operations, not automatic choices. Automatic
simplification should be implemented as a proposal layer that scores candidates.
For example, a bounding-box replacement can be proposed when its fill ratio and
relative volume error are inside user policy. Obvious cases can then be accepted
by policy, while ambiguous or high-impact cases remain visible to the user or
agent.

## Milestones

### Milestone 1: Honest Analysis

- Split current `engine.cpp` into import, analysis, preprocessing, and emission
  files.
- Introduce `CadModel`, `CadSolid`, `CadFace`, and stable IDs.
- Emit `report.json`.
- Keep existing CLI short form.

Done when: `csgify inspect input.step --report report.json` produces a useful
machine-readable report without creating Alea geometry.

### Milestone 2: Semantic Cleanup API

- Support `remove_solid`.
- Support synthetic bodies: box, sphere, cylinder, cone, union, intersection,
  difference.
- Support `replace_solid_with_bbox`.
- Report original and replacement volume metrics.
- Keep operations available as direct C++ API and small CLI commands.

Done when: a user or agent can remove a solid, add a synthetic replacement, and
export the result without a decision-file parser.

### Milestone 3: Cleanup Candidate Detection

- Add volume fraction and bbox diagonal fraction to the analysis report.
- Detect small-component candidates.
- Detect long cylinder-like candidates from bbox/face evidence.
- Detect thin plate-like candidates.
- Add direct replacement helpers for cylinder and sphere.

Done when: `inspect` gives a human or agent enough metrics to decide whether
bolts, labels, tiny hardware, or decorative solids should be removed or replaced.

### Milestone 4: Spline Replacement

- Detect spline faces.
- Fit planes and cylinders.
- Report fit error.
- Apply accepted replacements.
- Revalidate BRep.

Done when: near-planar and near-cylindrical spline faces can be replaced with
recorded tolerance and converted downstream.

### Milestone 5: Decomposition

- Improve planar splitting.
- Detect concave edges.
- Propose split planes.
- Validate piece sets against original volume.
- Emit only valid pieces.

Done when: a non-convex planar solid can be decomposed automatically into valid
Alea cells.

### Milestone 6: Strict CSG Emission

- Move current Alea emission into `emit_alea.cpp`.
- Require prevalidated pieces.
- Extend torus emission beyond axis-aligned tori if Alea transform support is
  available.
- Add CAD-to-Alea mapping output.

Done when: simple convex STEP solids and synthetic replacements convert with
mapping and validation reports.

### Milestone 7: Agent Workflow

- Make diagnostics actionable with allowed actions.
- Add compact optional JSON schemas if direct CLI/API operations are not enough.
- Add examples for Codex/Claude/Pi-style agent loops.
- Add `csgify plan` to generate candidate actions.

Done when: an agent can inspect a model, propose decisions, dry-run them, and
iterate without touching source code.

## Immediate Next Steps

1. Refactor the current monolithic `engine.cpp` into stage files.
2. Add `report.json` output before adding more geometry algorithms.
3. Define stable shape IDs.
4. Make current non-convex and spline diagnostics include allowed actions.
5. Add `decisions.json` parsing for `keep`, `remove_solid`, and `manual`.
6. Improve planar decomposition with concave-edge-driven cuts.
7. Add surface emitters for cone and torus.

The priority is not to maximize automatic conversion immediately. The priority
is to make the tool honest, scriptable, and useful in a human/agent workflow.

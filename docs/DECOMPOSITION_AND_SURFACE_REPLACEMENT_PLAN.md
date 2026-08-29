# Decomposition And Surface Replacement Implementation Plan

This document is the implementation plan for the two engine gaps that block
real csgify usage:

1. Complete decomposition of solids into CSG-convertible pieces.
2. Support replacement of unsupported and spline surfaces.

The objective is not to make every CAD file convert automatically. The
objective is to make the engine deterministic, inspectable, replayable, and
honest: it should convert what it can prove, propose clear candidates when it
cannot prove the next step, and stop before emitting wrong CSG.

The design follows the existing csgify pattern:

```text
analyze -> propose -> review/auto-policy -> apply -> log -> re-analyze
```

The human and the agent interact at the proposal boundary. The engine owns
geometry facts and validation. The agent may select obvious choices, explain
ambiguous ones, or ask the user. The user checks geometry with the viewer.
The durable result is always a command or recipe entry, not hidden GUI state.

## Current State

Already present:

- STEP loading through OCCT 8.
- Solid and face analysis.
- Assembly/name metadata extraction.
- Convex CSG validation for direct Alea cell emission.
- Spline analytic fits for plane, sphere, cylinder, and cone.
- Manual face replacement with fitted analytic surfaces.
- `auto_fit_splines`.
- Planar and face-directed decomposition.
- BSP-style auto-decomposition with conservative policy.
- Decomposition proposals and candidate metrics.
- Decomposition pieces as first-class session targets.
- Viewer review for selection, removal, hole fill, and decomposition planes.
- Decision logs, recipes, Lua API, C ABI, and scoped IDs.

Known gaps:

- Decomposition is not complete enough for general non-convex CAD.
- Split candidate generation still misses many obvious engineering cuts.
- Candidate scoring does not yet fully account for unsupported surface
  reduction, concavity reduction, or repeated failure history.
- Unsupported surfaces are mostly handled as diagnostics or spline fits; there
  is no general replacement proposal layer.
- The viewer can review decomposition and hole fill, but not yet surface
  replacement overlays.
- Large assembly performance is not proven.

## Non-Goals

- Do not silently mesh a spline and call the result exact CSG.
- Do not emit CSG for geometry that failed validation.
- Do not make the GUI a second source of truth.
- Do not depend on an agent for obvious deterministic operations.
- Do not solve void generation here; libalea may handle that separately.
- Do not implement every exotic CAD surface before the proposal/apply/review
  workflow is solid.

## Engine Principles

### Exact Before Approximate

Prefer exact analytic representation when possible:

- plane
- sphere
- cylinder
- cone
- torus, only when the downstream surface registry/emitter supports it

Approximation is allowed only when:

- the candidate has explicit deviation metrics;
- volume delta can be measured or bounded;
- the operation is logged;
- the user or policy accepted the tolerance.

### Decompose Before Guessing

For a non-convex solid, try decomposition before replacing surfaces. A spline
replacement on a non-convex solid may still leave an invalid Alea cell.
Decomposition can also isolate problematic local features so replacement
becomes smaller and safer.

This is not a strict one-pass order. The engine should iterate:

```text
remove useless components
analyze solid/piece
if non-convex: decompose
if unsupported faces remain: replace or split around them
re-analyze produced pieces
emit only validated pieces
```

### User Decisions Are Data

Every accepted manual or agent-selected operation must be represented as a
durable command:

```text
remove_solid doc:1/solid:7
apply_decomposition_candidate doc:1/solid:7 candidate:3
replace_surface_candidate doc:1/face:7.4 candidate:2
```

The viewer may help produce these commands, but it must not hide edits in GUI
state.

### Keep OCCT State Behind The Engine Boundary

Public APIs should expose document IDs, shape IDs, reports, proposals, and
commands. OCCT objects remain internal to the engine, session, and viewer.

## Phase A: Shared Analysis Foundation

Both decomposition and replacement need richer analysis. Implement this first,
or the later heuristics will duplicate work.

### A1. Surface Classification

Extend face analysis with a compact internal classification:

```cpp
enum class SurfaceSupportKind {
    plane,
    sphere,
    cylinder,
    cone,
    torus,
    bspline,
    bezier,
    surface_of_revolution,
    surface_of_extrusion,
    offset,
    blend_like,
    unknown
};
```

Public reports can keep strings for ABI stability:

```json
{
  "surface_type": "bspline",
  "support_kind": "surface_of_revolution",
  "exact_csg_supported": false,
  "needs_replacement": true
}
```

Classification sources:

- `BRepAdaptor_Surface::GetType()`
- OCCT geometric handle inspection when adaptor type is generic
- sampled normal behavior
- UV boundary shape
- relation to neighboring faces

Initial implementation can be conservative:

- exact OCCT analytic types are classified directly;
- BSpline and Bezier are classified as spline;
- revolution/extrusion detection can come later as secondary classifiers.

### A2. Face Adjacency Graph

Build an internal face adjacency graph per solid or piece:

```cpp
struct FaceAdjacency {
    std::string face_a;
    std::string face_b;
    std::string edge_id;
    double edge_length;
    bool convex = false;
    bool concave = false;
    bool tangent = false;
    double dihedral_angle = 0.0;
};
```

Uses:

- identify concave edges for decomposition;
- avoid proposing cuts from tiny decorative faces;
- identify blend/fillet faces;
- detect whether a spline is local and removable or structurally important.

Implementation notes:

- Use `TopExp::MapShapesAndAncestors` or equivalent topology mapping.
- Sample face normals near the shared edge.
- Classify concavity relative to the solid orientation.
- If the classification is ambiguous, mark it unknown; do not guess.

### A3. Per-Target Analysis Cache

Store analysis for original solids and decomposed pieces:

```cpp
struct TargetAnalysis {
    std::string target_id;
    double volume;
    BBox bbox;
    bool exact_csg_supported;
    bool needs_decomposition;
    bool needs_surface_replacement;
    std::vector<AnalyzedFace> faces;
    std::vector<FaceAdjacency> adjacency;
};
```

Avoid recomputing expensive surface sampling for every proposal candidate.
Invalidate only the target affected by an operation.

### A4. Shape Identity Rules

All proposal and apply APIs must accept scoped IDs:

```text
doc:1/solid:7
doc:1/piece:4
doc:1/face:7.3
```

They may accept local IDs as CLI shorthand when only one document is loaded.
Internal logs should prefer scoped IDs.

## Phase B: Complete Decomposition

The decomposition engine should operate on a queue of targets. A target can be
an original solid or a decomposed piece.

```text
queue = [solid or piece]
while queue not empty:
    analyze target
    if exact CSG supported:
        mark done
    else if needs decomposition:
        propose split candidates
        auto-apply only if policy allows
        otherwise emit decision request
    else if needs surface replacement:
        defer to surface replacement planner
```

### B1. Unified Decomposition Target

Current code already supports solid and piece targets in several places.
Finish this as a formal internal abstraction:

```cpp
struct DecompositionTarget {
    std::string id;
    TopoDS_Shape shape;
    double volume;
    BBox bbox;
    std::size_t depth;
    std::vector<std::string> parent_ops;
};
```

Required behavior:

- `propose_decomposition` works on `solid:N` and `piece:N`.
- `apply_decomposition_candidate` works on `solid:N` and `piece:N`.
- generated pieces are addressable immediately.
- the original target is suppressed when a split is applied.
- the operation logs produced piece IDs.

### B2. Candidate Sources

Candidate generation should be layered from safest to most heuristic.

#### B2.1 Directed Existing-Face Cutters

Already partly implemented. Complete and harden:

- planar face -> extended plane cutter
- cylindrical face -> extended cylinder cutter when useful
- conical face -> extended cone cutter when useful
- spherical face -> sphere cutter when useful
- torus face -> natural axis plane first; exact torus only if emitter support
  exists

Rules:

- Prefer faces involved in concave adjacency.
- Penalize very small faces unless they remove a known local feature.
- Penalize tangent blend faces as primary cutters unless the target is a blend
  removal/replacement case.

#### B2.2 Concave-Edge Planes

For every long concave edge, propose one or more planes:

- plane through the edge and average adjacent face normals;
- plane through the edge and target bbox center;
- plane normal to the strongest local concavity direction.

This catches L-shapes, slots, brackets, and ribbed parts better than bbox
center planes.

Candidate metadata:

```json
{
  "tool_kind": "concave_edge_plane",
  "source_edge_id": "edge:7.12",
  "source_face_ids": ["face:7.4", "face:7.5"],
  "reason": "long concave edge"
}
```

#### B2.3 Multi-Plane Groups

Many engineering parts need several parallel cuts at once:

- rails
- slotted brackets
- E-shaped frames
- repeated ribs

Group candidates by:

- nearly parallel plane normals;
- repeated spacing;
- similar concave-edge evidence;
- same analytic support family.

Apply as one `decompose_solid_by_planes` operation so the splitter has all
tools at once.

#### B2.4 Analytic-Feature Splitters

Feature-specific splitters are not replacements. They split the solid so each
piece is easier to represent.

Initial targets:

- torus face -> axis plane and optional orthogonal symmetry plane;
- cylindrical through slot -> planes through slot axis;
- spherical pocket -> planes through sphere center and dominant axes;
- cone/frustum transition -> plane normal to cone axis at transition.

#### B2.5 BBox And OBB Fallbacks

BBox center planes exist as low-confidence fallback. Improve them:

- compute OBB/PCA axes from bbox corners or mass properties;
- propose OBB midplanes;
- propose planes through largest concavity when adjacency data exists;
- use bbox fallback only when face/edge candidates fail.

Default policy:

- do not auto-apply bbox/OBB fallbacks;
- show them in proposals;
- let agent/user accept them explicitly.

### B3. Candidate Evaluation

Every candidate is dry-run with OCCT splitter, then the pieces are analyzed.

Metrics:

- splitter success;
- piece count;
- original volume;
- summed piece volume;
- absolute and relative volume delta;
- sliver count;
- min and max piece volume fraction;
- volume balance;
- exact CSG-supported piece count;
- piece count still needing decomposition;
- piece count still needing surface replacement;
- reduction in unsupported face area;
- reduction in concave adjacency count;
- runtime for the splitter call.

Candidate scoring should be understandable, not magic:

```text
score =
  exact_piece_bonus
  + unsupported_reduction_bonus
  + concavity_reduction_bonus
  + balance_bonus
  - volume_delta_penalty
  - sliver_penalty
  - high_piece_count_penalty
  - invented_cut_penalty
```

Confidence labels:

- `high`: volume preserved, no slivers, all or most pieces exact, cutter comes
  from CAD face or strong concave evidence.
- `medium`: volume preserved, improves state, but leaves unresolved pieces or
  uses heuristic cutter.
- `low`: useful candidate, but invented, unbalanced, or unresolved.
- `rejected`: splitter failed, volume delta too high, invalid pieces, or no
  improvement.

### B4. Auto-Decomposition Policy

Conservative default:

- auto-apply high-confidence CAD-derived or concave-edge candidates;
- require volume delta below tolerance;
- reject sliver-producing candidates;
- stop on equal-score ambiguity;
- stop on medium/low confidence unless policy opts in.

Policy knobs:

```cpp
struct DecompositionPolicy {
    bool allow_high_confidence_face_auto = true;
    bool allow_concave_edge_auto = true;
    bool allow_multi_plane_auto = true;
    bool allow_bbox_fallback_auto = false;
    bool allow_medium_confidence_auto = false;
    double max_relative_delta = 1.0e-8;
    double min_piece_volume_fraction = 1.0e-8;
    std::size_t max_splits_per_target = 16;
    std::size_t max_total_pieces = 10000;
};
```

For large STEP files, `max_total_pieces` is important. A bad decomposition can
explode runtime and produce unusable neutronics input.

### B5. No-Progress Detection

Store a compact signature for each target:

```text
volume bucket
bbox bucket
face count
unsupported face count
concave edge count
exact support flag
```

If a split produces pieces whose aggregate status is not better than the input,
penalize or reject the candidate.

Repeated failures should be recorded:

```json
{
  "target_id": "doc:1/piece:12",
  "failed_candidate_kinds": ["bbox_plane_x", "face_cylinder"],
  "reason": "no_status_improvement"
}
```

### B6. Decision Requests

When auto-decomposition stops, create a decision request:

```json
{
  "kind": "decomposition",
  "target_id": "doc:1/piece:12",
  "reason": "no high-confidence candidate",
  "recommended_action": "review_decomposition",
  "candidates": [
    {
      "id": "candidate:1",
      "action": "apply_decomposition_candidate",
      "confidence": "medium",
      "reason": "concave_edge_plane",
      "metrics": [
        {"name": "relative_delta", "value": 1.2e-10},
        {"name": "piece_count", "value": 3},
        {"name": "exact_piece_count", "value": 2}
      ]
    }
  ]
}
```

Viewer command:

```sh
csgify view model.step --review-decomp candidate:1 --select doc:1/piece:12
```

Expected viewer behavior:

- highlight target;
- draw candidate plane/surface overlays scaled to target bbox;
- `A` emits `command apply_decomposition_candidate ...`;
- `X` emits rejection line.

### B7. Decomposition Milestones

Milestone B1: target unification

- All decomposition proposal/apply commands accept solids and pieces.
- IDs are scoped in reports and viewer.
- Tests cover applying a candidate to a generated piece.

Milestone B2: adjacency and concave-edge candidates

- Face adjacency graph exists internally.
- Concave planar L-shape proposes edge-derived planes.
- Candidate report explains source edge and faces.

Milestone B3: multi-plane candidates

- Rails/slotted fixture produces one multi-plane candidate.
- Dry-run metrics report all produced pieces.
- Policy can auto-apply only when high confidence.

Milestone B4: analytic-feature splitters

- Torus natural plane candidate works.
- Cylindrical slot split candidate works.
- Viewer shows non-plane overlays where supported.

Milestone B5: no-progress and budget controls

- Bad bbox candidates are rejected for no improvement.
- `max_total_pieces` prevents explosion.
- Large assembly run reports where it stopped.

Milestone B6: performance pass

- Profile on small fixtures, medium fixtures, and
  `SHIELDING_ASSY_5638_assembly.stp`.
- Cache target analysis.
- Avoid repeated full-document analysis inside candidate loops when possible.

## Phase C: Unsupported And Spline Surface Replacement

Surface replacement should become a proposal system parallel to decomposition.
The current `replace_face_with_fit` and `auto_fit_splines` are useful building
blocks, but they need a general candidate layer.

### C1. Replacement Target Model

Targets are faces:

```text
doc:1/face:7.4
```

Internal target:

```cpp
struct SurfaceReplacementTarget {
    std::string face_id;
    std::string owner_target_id; // solid or piece
    SurfaceSupportKind support_kind;
    double area;
    BBox bbox;
    bool structurally_important;
};
```

The owner target matters because replacement must validate the resulting solid
or piece, not just the face.

### C2. Candidate Types

Define:

```cpp
enum class SurfaceReplacementKind {
    plane,
    sphere,
    cylinder,
    cone,
    torus,
    piecewise_planes,
    piecewise_cylinders,
    split_before_replace,
    remove_blend,
    unsupported
};

struct SurfaceReplacementCandidate {
    std::string id;
    std::string face_id;
    std::string owner_target_id;
    SurfaceReplacementKind kind;
    std::string confidence;
    std::string reason;
    bool exact = false;
    bool usable = false;
    double score = 0.0;
    double max_deviation = 0.0;
    double rms_deviation = 0.0;
    double max_normal_deviation = 0.0;
    double area_delta = 0.0;
    double owner_volume_delta = 0.0;
    double owner_relative_volume_delta = 0.0;
    std::size_t segment_count = 0;
    std::vector<Diagnostic> diagnostics;
};
```

Proposal:

```cpp
struct SurfaceReplacementProposal {
    bool ok = false;
    std::string face_id;
    std::string owner_target_id;
    std::string best_candidate_id;
    std::vector<SurfaceReplacementCandidate> candidates;
    std::vector<Diagnostic> diagnostics;
};
```

### C3. Candidate Generation By Surface Type

#### C3.1 B-Spline And Bezier Faces

Already started:

- fit plane;
- fit cylinder;
- fit sphere;
- fit cone.

Extend with:

- boundary deviation metrics;
- normal deviation metrics;
- owner volume delta after replacement;
- confidence from tolerance and topology.

Rules:

- Near-analytic fits can be high confidence if max deviation and volume delta
  are below tolerance.
- Fits that alter boundary loops significantly are not high confidence.
- If several fits are similar, require review.

#### C3.2 Surface Of Revolution

Detect when a spline is likely a revolution:

- sampled points rotate around a stable axis;
- normals are consistent with rotational symmetry;
- UV curves indicate one periodic direction.

Candidate strategies:

- exact cone/cylinder/sphere if profile is linear/circular;
- piecewise cones/cylinders along the axis if profile is smooth but not exact;
- split owner target before replacement when the revolution surface spans a
  non-convex region.

Metrics:

- radial profile deviation;
- axial segment count;
- max surface deviation;
- volume delta.

#### C3.3 Extruded Spline Surface

Detect:

- one principal direction is nearly linear;
- cross sections are translated along a stable direction.

Candidate strategies:

- fit plane/cylinder/cone if possible;
- approximate the profile with N planar or cylindrical segments;
- split along profile breakpoints.

This is common for imported sheet-metal-like or swept CAD surfaces.

#### C3.4 Torus

Torus handling depends on downstream support.

Plan:

1. If libalea/Alea emitter has a torus primitive registered and the target
   surface orientation is compatible, propose exact torus emission.
2. Otherwise, propose decomposition through natural torus planes first.
3. If approximation is allowed, propose piecewise cylindrical/conical segments
   with max-deviation guard.

Default:

- do not auto-approximate torus;
- exact torus can become high confidence only when emission support exists;
- torus-axis decomposition can be high or medium depending on split result.

#### C3.5 Offset And Blend-Like Faces

Many small fillets are not important for neutronics, but some are real flow or
shielding geometry. Treat them carefully.

Candidate strategies:

- if the blend is tiny relative to target bbox and volume, propose removal or
  chamfer/plane replacement;
- if the blend bounds a real cavity, propose decomposition instead;
- otherwise require user decision.

Useful metrics:

- blend radius estimate;
- area fraction;
- affected volume fraction;
- neighboring face types;
- whether the blend is attached to a removed component/hole-fill operation.

#### C3.6 Unknown NURBS

For unclassified NURBS:

- try analytic fits;
- try revolution/extrusion classifiers;
- if none pass, stop with `unsupported_surface_needs_decision`.

Do not invent piecewise replacements automatically for unknown surfaces.

### C4. Deviation Metrics

Sampling must be deterministic and bounded.

For each candidate:

- sample the original face in UV space;
- project sampled points to the candidate analytic surface;
- record max distance;
- record RMS distance;
- record normal angle deviation where normals are stable;
- sample boundary curves separately;
- compute area delta when possible;
- build candidate owner shape and compute volume delta when possible.

Sampling density:

- base grid, for example 9x9;
- adaptive refinement where deviation is high;
- max sample cap for performance;
- deterministic order for reproducible reports.

Tolerance policy:

```cpp
struct SurfaceReplacementPolicy {
    double max_deviation = 1.0e-6;
    double max_relative_deviation = 1.0e-6;
    double max_normal_deviation_deg = 1.0;
    double max_owner_relative_volume_delta = 1.0e-8;
    std::size_t max_segments = 32;
    bool allow_piecewise_auto = false;
    bool allow_torus_exact = false;
};
```

### C5. Applying Replacement

API:

```cpp
SurfaceReplacementProposal propose_surface_replacement(
    Session& session,
    const std::string& face_id,
    const SurfaceReplacementPolicy& policy = {},
    ReportIdMode id_mode = ReportIdMode::scoped);

CommandResult apply_surface_replacement_candidate(
    Session& session,
    const std::string& face_id,
    const std::string& candidate_id,
    ReportIdMode id_mode = ReportIdMode::scoped);
```

Existing APIs can remain as low-level shorthands:

```cpp
replace_face_with_fit(...)
auto_fit_splines(...)
```

But the preferred user/agent path should be proposal-based.

Apply sequence:

1. Resolve face and owner target.
2. Recompute or fetch proposal.
3. Validate candidate still matches current geometry.
4. Apply replacement to model.
5. Rebuild owner target shape.
6. Check OCCT validity.
7. Re-analyze owner target.
8. Compare volume.
9. Log operation.
10. Return report and diagnostics.

If the candidate is no longer valid because earlier operations changed the
target, fail clearly and ask for a new proposal.

### C6. Auto-Replacement Policy

Automatic replacement can be allowed only when:

- candidate is exact or deviation is below strict tolerance;
- owner volume delta is below tolerance;
- owner remains valid;
- replacement reduces unsupported surface count;
- no similar alternative candidate exists with ambiguous score.

Default:

- exact near-plane/cylinder/sphere/cone spline fits may auto-apply when strict
  tolerances pass;
- piecewise approximations require review;
- torus approximation requires review;
- unknown NURBS always requires review.

### C7. Decision Requests

When replacement cannot be automatic:

```json
{
  "kind": "surface_replacement",
  "target_id": "doc:1/face:7.4",
  "reason": "unsupported spline face",
  "recommended_action": "review_surface_replacement",
  "candidates": [
    {
      "id": "candidate:1",
      "action": "apply_surface_replacement_candidate",
      "confidence": "medium",
      "reason": "fit_cylinder",
      "metrics": [
        {"name": "max_deviation", "value": 2.0e-7, "unit": "model"},
        {"name": "rms_deviation", "value": 3.0e-8, "unit": "model"},
        {"name": "owner_relative_volume_delta", "value": 4.0e-10}
      ]
    }
  ]
}
```

Viewer command:

```sh
csgify view model.step --review-surface-replacement candidate:1 \
    --select-face doc:1/face:7.4
```

Expected viewer behavior:

- highlight owner solid/piece;
- highlight target face;
- draw replacement surface overlay;
- show sampled deviation markers for worst points later;
- `A` emits `command apply_surface_replacement_candidate ...`;
- `X` emits rejection.

### C8. Surface Replacement Milestones

Milestone C1: proposal types

- Add internal/public proposal structs.
- Add JSON report output.
- Add Lua and C ABI access after C++ session API is stable.

Milestone C2: spline analytic proposal layer

- Wrap existing plane/cylinder/sphere/cone fits as candidates.
- Add owner volume delta metrics.
- Add decision requests for non-auto fits.

Milestone C3: apply candidate

- Apply a selected candidate through the new proposal API.
- Preserve existing `replace_face_with_fit` as shorthand.
- Log operation with candidate metrics.

Milestone C4: viewer replacement review

- Add `--review-surface-replacement`.
- Highlight face and overlay analytic surface.
- Emit replayable accept/reject commands.

Milestone C5: piecewise approximations

- Implement piecewise planar/extruded approximation for controlled cases.
- Segment count and deviation are explicit.
- No default auto-apply.

Milestone C6: torus and revolution strategy

- Detect torus and revolution surfaces.
- Use exact primitive only when Alea/libalea emitter support exists.
- Otherwise propose split-before-replace or reviewed approximation.

Milestone C7: unknown surface stop condition

- Unknown NURBS produces clear decision request.
- No silent fallback.

## Phase D: Combined Pipeline

Once B and C exist, the preprocessing engine should become a loop:

```text
for each active target:
    analyze
    if target is removable by accepted cleanup policy:
        remove
    else if exact CSG supported:
        done
    else if non-convex:
        decompose or request decision
    else if unsupported surfaces exist:
        replace/split or request decision
    else:
        report unsupported final state
```

Important: decomposition and replacement can alternate.

Example:

```text
solid:7 non-convex and has spline blend
-> decompose by concave edge
-> piece:3 exact
-> piece:4 convex but has spline
-> replace spline with cylinder
-> piece:4 exact
```

### Pipeline Report

Add a concise preprocessing report:

```json
{
  "targets_seen": 42,
  "targets_exact": 37,
  "targets_decomposed": 8,
  "surface_replacements_applied": 5,
  "targets_needing_decision": 3,
  "targets_failed": 1,
  "total_volume_delta": 1.3e-9,
  "decisions": [...]
}
```

This report is more useful for agent workflows than a long list of raw
diagnostics.

## Phase E: Commands And APIs

### CLI

Add:

```sh
csgify surface-suggest input.step face:N.M [--report proposal.json]

csgify simplify input.step \
    --apply-surface-candidate face:N.M \
    --candidate candidate:N \
    --out output.i

csgify preprocess input.step \
    --auto-decompose-bsp \
    --auto-replace-surfaces \
    --max-deviation 1e-6 \
    --out-step simplified.step \
    --report report.json \
    --log decisions.json
```

Viewer:

```sh
csgify view input.step \
    --review-surface-replacement candidate:N \
    --select-face doc:1/face:7.4
```

### Lua

Add:

```lua
local p = s:propose_surface_replacement("doc:1/face:7.4", {
    max_deviation = 1e-6,
    max_owner_relative_volume_delta = 1e-8
})

local report = s:apply_surface_replacement_candidate(
    "doc:1/face:7.4",
    p.best_candidate_id
)
```

The agent can script the same loop as the CLI:

```lua
while true do
    local r = s:analyze(doc)
    local d = next_decision(r)
    if not d then break end
    -- accept obvious high-confidence candidates, otherwise ask user/viewer
end
```

### C ABI

Expose only stable string/report surfaces:

- propose replacement -> JSON string or opaque report handle;
- apply candidate -> command result;
- no OCCT handles.

## Phase F: Viewer Work

Decomposition viewer improvements:

- scale cut planes from target bbox, not fixed size;
- draw non-plane cutter surfaces when candidate is cylinder/cone/sphere;
- optionally isolate target automatically in review mode;
- show candidate metrics in stderr.

Surface replacement viewer:

- highlight target face;
- draw replacement surface overlay;
- show original target owner transparent;
- later: draw worst-deviation sample points.

This is enough before Qt. Qt becomes justified when:

- tree and viewer must be integrated;
- filtering large assemblies is necessary;
- user needs persistent decision panels.

## Phase G: Performance Plan

Performance concerns:

- OCCT splitter calls are expensive.
- Full analysis after every candidate is expensive.
- Large assemblies may have thousands of solids.
- Viewer display of all solids is expensive.

Rules:

- Analyze only active targets when possible.
- Cache face classification and sampling.
- Use bbox filters before expensive geometry operations.
- Limit candidates per target.
- Limit total pieces.
- Record timing per phase.
- Keep OpenMP out initially unless profiling shows a clean independent loop.

Useful metrics:

```json
{
  "load_step_ms": 0,
  "analyze_ms": 0,
  "proposal_ms": 0,
  "splitter_calls": 0,
  "surface_fit_calls": 0,
  "surface_sample_points": 0,
  "pieces_produced": 0,
  "cache_hits": 0
}
```

Performance test files:

- small fixtures in `tests/fixtures`;
- `rails.stp`;
- `bolt_hole.step`;
- `SHIELDING_ASSY_5638_assembly.stp`;
- a real 100 MB class model when available.

## Phase H: Testing

### Unit/Smoke Fixtures

Need fixtures for:

- convex box;
- L-shape planar decomposition;
- slot/rail multi-plane decomposition;
- torus face;
- tilted torus;
- simple spline plane;
- spline cylinder;
- spline cone;
- spline surface of revolution;
- extruded spline;
- tiny fillet/blend;
- unknown NURBS that must stop.

### Contract Tests

For each operation:

- proposal JSON contains stable IDs;
- candidate has metrics;
- invalid candidate fails cleanly;
- apply logs the operation;
- replay reproduces result;
- volume delta is within tolerance;
- resulting target is re-analyzed.

### Viewer Tests

Automated GUI testing can be limited:

- `viewer-check` confirms OCCT viewer availability.
- review commands validate proposals before opening the display.
- manual smoke for overlays on selected fixtures.

### GEOUNED Comparison

Use GEOUNED as a reference, not as an oracle:

- compare number of generated cells/surfaces;
- compare major decomposition choices;
- compare runtime phase by phase;
- compare volume preservation;
- ignore void generation for now because libalea may handle it.

## Phase I: Suggested Implementation Order

1. Add target analysis cache and surface classification fields.
2. Add formal face adjacency graph.
3. Add concave-edge decomposition candidates.
4. Add candidate no-progress detection and stronger scoring.
5. Add multi-plane grouping.
6. Add target/piece decomposition contract tests.
7. Add `SurfaceReplacementProposal` and candidate structs.
8. Wrap current spline fits as replacement proposals.
9. Add owner volume delta for replacement candidates.
10. Add apply-surface-candidate command.
11. Add decision requests for surface replacement.
12. Add viewer overlay for surface replacement.
13. Add piecewise replacement for simple extrusion/revolution cases.
14. Add torus strategy using libalea/emitter capability detection.
15. Run large-model profiling and optimize the slowest measured path.

## Definition Of Done

Decomposition is good enough for early real use when:

- non-convex fixtures decompose into exact CSG-supported pieces;
- generated pieces can be recursively decomposed;
- ambiguous cases produce reviewable candidates;
- volume preservation is measured and guarded;
- no-progress and piece explosion are prevented.

Surface replacement is good enough for early real use when:

- spline plane/cylinder/sphere/cone replacements are proposal-based;
- max/RMS deviation and owner volume delta are reported;
- exact or near-exact replacements can auto-apply under strict policy;
- approximate replacements require review;
- unknown surfaces stop with clear diagnostics.

The complete preprocessing engine is good enough for serious testing when:

- the combined loop can process a real assembly without hidden edits;
- every accepted operation is logged and replayable;
- unresolved geometry is reported with decision packets/viewer commands;
- emitted Alea cells only come from validated CSG-convertible targets.

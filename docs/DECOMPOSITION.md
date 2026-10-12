# Decomposition Plan

The first safe rule is now enforced in code: a CAD solid is emitted as one Alea
cell only when it passes convex halfspace validation. If it fails, `csgify`
reports `nonconvex_solid_requires_decomposition` and emits no wrong CSG.

The first decomposition paths are now available both as internal machinery and
as explicit preprocessing operations.

## Automatic structural routing

`inspect`, `decompose-apply`, `preprocess`, and `shatter` now classify topology
before choosing a decomposition strategy. Reports expose per-solid V/E/F and
Euler counts, face inner wires, shell/cavity counts, manifold/boundary edges,
concave edges, unique planes and normal directions, bounding-box fill ratio,
ranked `recognition_candidates`, a selected recognition kind, and a
`recommended_strategy`.

- A multi-shell solid whose outer and cavity shells are individually convex is
  emitted exactly as `outer - union(cavities)`. The original materialized
  B-rep is retained for STEP export. This is available to Lua as
  `s:decompose_shell_difference(id)`.
- An all-planar, all-linear-edge prismatic solid uses a bounded sequential
  support-plane arrangement. Profile planes are applied before axial levels;
  only residual cells receive extra planes through reflex vertices. The
  `prismatic_max_pieces` and `prismatic_max_splitter_calls` budgets prevent a
  Boolean splitter from constructing an unbounded global arrangement. The
  extrusion axis is derived from mutually parallel/perpendicular CAD support
  planes rather than a world-aligned bounding box, so rotated constant and
  piecewise extrusions follow the same path. Reports include axial end-face
  count, profile-hole count, and constant-cross-section evidence.
- A straight full-annulus pipe with two coaxial full cylindrical supports and
  two annular end faces is recognized in any orientation. Its axis, inner and
  outer radii, length, and two outward-facing lumen ports are reported. The
  wall is preserved as one exact OCCT-certified Boolean body, `outer cylinder
  - inner cylinder`, before BSP or shatter is considered when decomposition is
  required. A pipe already supported as one analytic CSG cell is left
  unchanged. Reports, project
  spatial indexes, replay, and Lua recipes retain the recognition evidence and
  port topology; Lua exposes `s:decompose_pipe_annulus(id)`.
- Cylindrical, conical, toroidal, spherical, and compatible planar faces are
  also checked for a shared revolution axis. Reports distinguish full from
  partial angular sweeps, record radial seam faces and axial topology levels,
  and retain the ranked evidence. A non-convex full revolution such as a
  stepped shaft is split sequentially only at internal axial meridian-event
  planes; an exact result short-circuits BSP/shatter. The replayable Lua
  primitive is `s:decompose_revolution(id)`. Partial revolutions are reported
  for review but are not silently treated as full revolutions.
- Other solids use the scored progressive BSP path. Low policy now genuinely
  admits low-confidence candidates, and candidates report a halfspace-
  violation `progress_ratio` rather than rewarding a cut only because it peels
  a small piece.

Emission is pure by default: it validates and emits the prepared model but does
not launch hidden decomposition. A partial strategy writes its report and
optional STEP result before returning failure; it is never fed into an
unbounded fallback during emission.

`decompose_solid_by_planar_faces` and the CLI command
`simplify --decompose-planar solid:N` extend the selected solid's planar faces
and ask OCCT's splitter to cut the solid.

`decompose_solid_by_face` and the CLI command
`simplify --decompose-by-face solid:N --by-face face:N.M` are the directed
manual primitive. The selected face must belong to the target solid. Plane,
cylinder, cone, and sphere faces are converted into bounded cutting surfaces.
Torus faces are split through a natural plane containing the torus axis. This
keeps user/agent decisions explicit and replayable while giving later BSP
heuristics a simple command to emit.

`propose_decomposition_candidates` and the CLI command
`decompose-suggest input.step solid:N --report proposals.json` dry-run the same
face-driven splits without mutating the model. Each candidate is scored from:

- splitter success and produced piece count
- volume preservation
- sliver-piece detection
- how many produced pieces are exact CSG-supported
- how many pieces still need decomposition, spline replacement, or user action
- volume balance
- preference for existing CAD support surfaces

The proposal stage is where agent/user interaction belongs. The engine produces
facts and candidate scores; an agent may auto-apply a high-confidence candidate
or show the candidate list to the user. The split itself is still applied later
with the explicit `decompose_by_face` command so the decision is replayable.

## Exhaustive subdivision: `shatter`

The confidence-scored proposal path above searches for *one good cut* and stops
at `*_needs_decision` when no high-confidence candidate exists. The `shatter` op
takes the complementary approach and never searches. For each piece that fails
convex halfspace validation it applies, in order:

1. **a targeted cut by the convexity-failure face** — the validator already
   identifies a vertex on the far side of one face's surface, so that surface's
   extension is guaranteed to separate the piece (`find_convexity_failure_face`
   + `split_solid_by_face`). One surface at a time yields the minimal, cleanest
   pieces (an L-shape becomes 2 cells, not 3);
2. **bounded prismatic subdivision for planar/linear solids** — unique profile
   and axial planes are applied sequentially under explicit piece/splitter
   budgets, so the legacy all-plane batch arrangement is never constructed;
   non-prismatic curved solids may still use the bounded all-surface fallback;
3. **closure planes** — for a *partial* cylinder/cone arc with no planar CAD
   face bounding it, synthesize the axis/chord plane through the arc's
   angular-extreme generators (`split_solid_by_closure_planes`).

Each produced piece is re-validated and re-fed up to `--max-depth`. There is no
confidence gate: it either reduces the solid to simple pieces or fails honestly
with `shatter_residual_nonconvex` (e.g. a torus or spline-bounded region that
needs surface replacement, not decomposition), never emitting wrong CSG.

Two robustness details matter at real-world scale (models far from the origin,
e.g. coordinates in the 10^4 range):

- **Cutters are re-anchored on the target solid.** A bounded cutter face is
  built centered on the projection of the solid's bbox center onto the surface
  (`plane_anchored_at`, and the cylinder/cone V range centered on the anchor's
  axial coordinate), not on the surface's intrinsic `Location()`. Otherwise the
  +/- half_size cutter can sit entirely off the part and the split silently
  produces nothing -- the dominant cause of stalled decomposition on large-
  coordinate assemblies.
- **Convexity is tested in geometric distance.** The validator compares
  `AnalyticSurface::signed_distance_at` (the algebraic value divided by the
  gradient magnitude) against `linear_tolerance`, because the raw implicit
  value is length^2 for quadrics/spheres and length^4 for tori -- comparing
  those to a length tolerance mis-classifies vertices on large-radius faces.
- **`find_interior_point` insets from face centroids** along the inward normal
  (not just a coarse bbox grid), so thin and awkward fragments still yield an
  interior point; a final piece with none is dropped
  (`shatter_dropped_degenerate_piece`), volume-guarded, rather than emitted.

```sh
# one solid, as a simplify action:
csgify simplify input.step --shatter solid:N --out output.i --log d.json

# whole assembly, as a standalone verb (shatter every non-convex solid):
csgify shatter input.step --out output.i --all --log d.json
#   --target solid:N   shatter only the named solid(s) instead of --all
#   --max-depth N      recursion bound (default 8)
```

`shatter --all` snapshots every solid that fails convex validation and shatters
each; `solid:N` IDs are stable across shatters (a shattered solid is suppressed,
not renumbered), and each op is recorded so the decision log replays.

Geometry lives in `split_solid_by_all_surfaces` (`src/decompose.cpp`); the
recursion, volume guard, and piece bookkeeping live in `shatter_solid`
(`src/shatter.cpp`). It is volume-guarded by default: if the summed piece volume
drifts past the dimensionless `shatter_relative_volume_tolerance`, the op refuses to mutate unless
`--allow-shatter-volume-drift` is passed. Shatter on an already-simple solid is a
no-op (`shatter_not_needed`) that leaves `solid:N` intact.

After decomposition, csgify first performs an OCCT fixed-point coalescing pass.
Candidate pieces first match exact bounded planar interfaces, then a broader
pass considers overlapping face regions on the same support plane (covering
interfaces that OCCT partitioned differently on each side). Their OCCT fuse is
accepted only when it returns one valid solid, preserves the summed volume
within `decomposition_relative_volume_tolerance`, and passes the ordinary convex
half-space certificate. This produces the same maximal convex pieces in both
the review STEP and the emitted CSG; genuine non-convex unions remain separate.

The remaining pieces from the same source solid are then emitted as one Alea
**union** (using `alea_union_n`). `alea_simplify_and_prune_cells` provides a
secondary Boolean normalization and surface-deduplication layer. Library and
Lua callers can set `union_decomposed_pieces=false` to preserve separate Alea
cells, but this does not disable the earlier OCCT geometry coalescing.

The op records through the decision log, compacts into `s:shatter(id, { ... })`
in the Lua recipe, and replays through both `csgify lua` and the JSON `replay`
verb (covered by `tests/shatter_smoke.sh`).

## Bounded fixed-point coordination

`decompose-apply` uses the fixed-point coordinator by default. The explicit
`--fixed-point` spelling is retained for scripts and clarity. It keeps one
in-memory document. Ordinary BSP candidates are immutable, target-local OCCT
partitions: rejected candidates never copy or mutate the assembly. Parallel
families of existing planar supports are tried before individual face planes,
which handles repeated rails, channels, and thin walls without immediately
falling into exhaustive `shatter`.

Before commit, each local partition must prove valid children, volume
conservation, child containment in the parent, complete parent coverage,
sibling disjointness, and no new overlap with the target's affected spatial
neighbours. The exact geometric proof uses two Boolean calls independent of
the child count: `compound(children) - parent` proves containment and
`parent - compound(children)` proves coverage. Given those results and the
conserved sum of child volumes, positive-volume sibling overlap is impossible.

A body-certificate ledger projects the document-wide progress score
algebraically. Candidate child analysis is also used to avoid certifying a
candidate that cannot beat an already exact-certified candidate; only
record-breaking projected candidates spend the two Boolean proof calls. This
changes proof cost, not selection semantics. Source-local OCCT coalescing is
applied only to the new children. Public IDs are allocated only after
acceptance, so rejected trials cannot perturb replay.

Once exact containment is established, external overlap safety follows by set
inclusion: for every neighbour `N`, `child ∩ N ⊆ parent ∩ N`. The local
certificate therefore does not rebuild every neighbour bbox or repeat OCCT
Commons. It records a containment-overlap proof, while the mandatory terminal
global reconciliation freshly enumerates the committed child pairs and remains
the publication authority.

The initial and terminal global overlap passes share an in-memory immutable
pair-evidence cache. The terminal pass always rebuilds the complete active-body
bbox sweep. An exact intersection result is reused only when both stable body
IDs survived unchanged; every pair involving a newly committed piece is
evaluated with a fresh OCCT Common. Unknown Boolean results are never reused.
This preserves the terminal global gate while avoiding duplicate expensive
intersections between untouched assembly components.

Acceptance requires:

- no new overlap or unsupported geometry;
- per-action and cumulative volume limits;
- no candidate slivers;
- active-piece, residual, candidate, splitter, action, and elapsed-time bounds;
- a strict document-wide lexicographic improvement: unsupported faces,
  residual-volume bucket, halfspace violations, residual count, then active
  piece count.

High- and medium-confidence candidates may therefore run autonomously when the
geometry certificate and global score pass. Low-confidence and bbox fallback
remain explicit policy choices. A locally successful split that increases the
global residual state is discarded without changing the session revision or
decision log.

The incremental proof optimizes candidate search; it does not weaken
publication. The coordinator always performs a fresh authoritative global
analysis and overlap reconciliation at its terminal boundary, and STEP/MCNP
publication remains subject to the normal final safety gate. Use
`--verify-incremental-state` to add the expensive reconciliation after every
accepted action while debugging. `--legacy-global-trials` restores copied
whole-model BSP trials for differential testing. Plane batching and its hard
bounds are controlled by `--no-parallel-plane-batches`,
`--max-parallel-families`, `--max-planes-per-batch`, and
`--max-local-boolean-calls`. `--max-candidates-per-target` (default 64) bounds
the work spent on one difficult residual before yielding to another body.
Candidate and splitter limits are enforced before an OCCT proposal call, not
after it.

Example:

```sh
csgify decompose-apply input.step --out output.i --out-step output.step \
  --fixed-point --max-actions 64 --max-active-pieces 512 \
  --max-action-volume-delta 1e-6 \
  --max-cumulative-volume-delta 1e-8 \
  --max-candidates-per-target 64 \
  --max-parallel-families 8 --max-planes-per-batch 64 \
  --log decisions.json --progress-json progress.jsonl --report report.json
```

`--progress-json` writes one JSON object per safe operation boundary, including
the current target, counters, residual volume, accepted actions, and terminal
stop reason. The equivalent C++ callback can request cancellation by returning
false; an OCCT Boolean already in flight finishes before the last certified
state is preserved.

JSON reports expose local proof cost and remaining global work under
`performance`, including `local_certificate_ms`, `local_overlap_ms`,
`local_partition_boolean_calls`, `parallel_plane_batches`,
`local_overlap_containment_proofs`,
`overlap_pair_cache_hits`, `overlap_pair_cache_misses`, `full_analysis_ms`,
`full_overlap_ms`, and `full_ledger_ms`,
`full_reconciliations`, and `whole_model_trial_copies`. Ordinary incremental
BSP discovery should report zero whole-model trial copies. Recognized
structural strategies run their older mutation adapters in isolated one-body
sandboxes, so they also avoid copying the complete assembly.

`decompose-apply` always writes a final STEP alongside MCNP. If `--out-step` is
omitted, the safe default is `<out-stem>_decomposed.step`; the explicit option
overrides that path.

Use `--single-pass` only for compatibility testing of the former per-target
transaction behavior. It does not provide document-wide progress scoring or
certified partial commits. The target-filtered form (`--target`) also retains
single-pass behavior unless `--fixed-point` is explicitly supplied, in which
case the unsupported local filter is rejected rather than weakening the global
score invariant.

The separate action and cumulative tolerances accommodate local OCCT splitter
noise without relaxing the final document-volume guarantee. Residual pieces
created by decomposition that fall below the minimum piece fraction may be
suppressed automatically only when the cumulative volume guard still passes;
the removal is logged and reported.

Every accepted iteration is recorded as its resolved primitive operation
(`decompose_by_face`, `decompose_by_plane`, `decompose_by_planes`, prismatic or
shell-difference decomposition, pipe-annulus recognition, or a residual-piece
removal). Rejected trials
are not mutations and do not enter the decision log. Consequently:

```sh
csgify compact decisions.json --out reproduce.lua --preserve-step-exports
csgify lua reproduce.lua
```

replays the accepted process without rerunning candidate search. Replay is
normally much faster than discovery and remains stable if candidate scoring is
changed later. Without `--preserve-step-exports`, compaction omits recorded STEP
side outputs and keeps only the geometry operations plus final MCNP emission.

The first automatic layer is safe BSP auto-decomposition:

```sh
csgify simplify input.step --auto-decompose-bsp solid:N \
    --max-depth 4 --out output.i
```

It repeatedly requests proposals and applies only high-confidence candidates.
If a generated piece still needs decomposition, it is queued as `piece:N` and
processed recursively up to `max_depth`. If no high-confidence candidate exists,
the command stops with `auto_decompose_bsp_needs_decision` instead of guessing.
This is the boundary between deterministic engine work and agent/user
interaction.

The auto-decomposition decision is now routed through an explicit policy:

- high-confidence face-driven candidates are allowed by default
- bbox-axis fallback candidates are not allowed by default
- medium-confidence candidates are not allowed by default
- candidate `relative_delta` must be below the policy limit
- candidate minimum piece volume fraction must be above the policy limit

CLI opt-ins exist for the two non-default automatic behaviors:

```sh
--allow-bbox-fallback-auto
--allow-medium-confidence-auto
```

The defaults remain conservative for neutronics: exact, CAD-face-derived,
high-confidence splits may proceed; invented fallback planes require review.

Stopped BSP states should be exported with `--proposal-report`. A candidate can
then be applied explicitly:

```sh
csgify simplify input.step --apply-decomposition-candidate solid:N \
    --candidate candidate:K --out output.i \
    --proposal-report proposals.json
```

Candidate IDs are intentionally proposal-local. The durable operation remains
the resolved target plus source face, which is what the decision log and recipe
carry forward.

In both cases the original solid is suppressed, generated pieces are stored in
the model, and the summed piece volume is compared with the original volume.

Any resulting piece is still emitted only if it passes the same convex validator
and surface-support checks. If pieces remain non-convex or contain unsupported
surfaces, conversion fails honestly after reporting the decomposition result.

The next layer should grow this into a fuller pipeline:

1. Analyze

   Collect solids, faces, surface equations, adjacency, bounding boxes, and
   unsupported surfaces. Do not create Alea objects in this stage.

2. Split

   Use candidate cutting surfaces from existing CAD faces first. For a
   non-convex solid, concave face/edge evidence should propose planes or
   analytic surfaces that split the shape into simpler pieces.

3. Validate

   Every produced piece must pass the same convex halfspace validation used by
   the current single-cell path.

   For an all-planar polyhedron, the signed distance to each support plane is
   affine, so checking every B-rep vertex is a complete halfspace-containment
   certificate. The OCCT Boolean halfspace check is reserved for curved
   surfaces, where an edge or face interior can contain the extremum. This also
   avoids wrong-side `MakeHalfSpace` results observed for very small planar
   partition faces in OCCT 8.

4. Emit

   Emit each validated piece as an Alea cell. The original CAD solid becomes a
   union of those cells at the model level, not a guessed single intersection.

5. Ask

   If the split requires approximating a spline or choosing between several
   simplification cuts, report a diagnostic that an agent or user can answer.

Current practical target:

- support planar polyhedra
- identify concave planar solids
- split them with existing planar face surfaces
- allow user/agent-directed splits by selected analytic faces
- reject pieces that still fail convex validation

That is enough to handle many engineering CAD parts without introducing silent
approximations.

Next target:

- broaden BSP candidate generation beyond bbox axis planes, starting with OBB
  and convex-hull concavity planes
- add quality metrics for candidate splits, primarily produced-piece count,
  volume balance, and whether every piece passes convex validation
- add type-specific unsupported-surface splitters, starting with torus and
  surfaces of revolution

When no face-driven candidate is usable, the proposal engine now adds three
bbox-axis fallback planes through the target bbox center:

- `bbox_plane_x`
- `bbox_plane_y`
- `bbox_plane_z`

These candidates use `reason = "bbox_axis_fallback"` and are intentionally not
high confidence. They give an agent or user a concrete fallback option without
letting safe BSP silently invent non-CAD splits.

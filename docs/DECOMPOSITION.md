# Decomposition Plan

The first safe rule is now enforced in code: a CAD solid is emitted as one Alea
cell only when it passes convex halfspace validation. If it fails, `csgify`
reports `nonconvex_solid_requires_decomposition` and emits no wrong CSG.

The first decomposition paths are now available both as internal machinery and
as explicit preprocessing operations.

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
2. **the full surface arrangement** — cut by the bounded cutter of every face
   at once (planes stay planes; cylinders/cones/spheres become their full
   quadric; tori become their axis plane), as a fallback when the targeted cut
   does not split;
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

By default each piece is emitted as its own Alea cell;
`alea_simplify_and_prune_cells` normalizes the resulting Boolean expressions.
Passing
`--union-decomposed-pieces` instead emits one Alea **union** cell per source
solid (the pieces combined with `alea_union_n`), so the original CAD solid stays
one logical body and Alea's union-branch simplification can apply; with the
default `split_union_cells` on, that union is then re-split into simple cells.

The op records through the decision log, compacts into `s:shatter(id, { ... })`
in the Lua recipe, and replays through both `csgify lua` and the JSON `replay`
verb (covered by `tests/shatter_smoke.sh`).

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

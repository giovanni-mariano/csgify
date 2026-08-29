# Exhaustive Decomposition — Implementation Plan

## Motivation

Today's decomposition (`src/cleanup.cpp` `propose_decomposition_candidates_impl`,
`src/decompose.cpp`) treats a split as a *search for one good face cut*, scores it,
and gates auto-apply behind a high/medium/low confidence policy
(`score_decomposition_candidate`, cleanup.cpp:879). For genuinely non-convex parts
no single face cut yields all-convex pieces, so the policy stalls at
`*_needs_decision` and we "don't really solve" the part.

The alternative — used by mature CAD→CSG converters — never searches and never
needs convex pieces. It **subdivides each solid against the full set of its own
surfaces** until every piece lies on one definite side of every surface, emits each
piece as a signed half-space intersection, takes the **union** of the pieces as the
original solid, and lets a boolean simplifier collapse the result.

The decisive enabler on our side: **Alea already provides the boolean layer.**

- `alea_union_n` / `alea_intersection_n` (alea.h:426-427)
- `alea_simplify_and_prune_cells` — NNF, associative normalization, absorption, idempotent reduction,
  subtree dedup, contradiction/tautology removal, union-branch subsumption
  (alea.h:494-521)
- `alea_split_union_cells` — explode a union root into k cells (alea.h:535)
- surface dedup via `cfg.dedup` + `alea_renumber_surfaces` (alea.h:128, 1069)

So the new work is **the exhaustive subdivision** plus **union reconstruction at
emit**. Everything downstream already exists.

## Invariant the subdivision must guarantee

A piece is *simple* iff it lies entirely on one side of every analytic surface that
bounds it. Our existing per-cell path already requires and checks exactly this:
`validate_convex_halfspace_cell` (engine_internal.cpp:317) picks each face's
half-space sense from an interior point and verifies all vertices are inside, and
`convert_solid` builds the cell as `alea_intersection_n` of those half-spaces
(emit.cpp:204). **A fully subdivided piece passes this validator by construction.**
That is why the subdivision is the whole game: produce simple pieces and the
existing emit path converts each one with no policy and no guessing.

## Naming

All new code uses our own vocabulary. Proposed names (final names at the author's
discretion):

- `shatter_solid` — top-level entry, runs the subdivision pipeline.
- `subdivide_by_planar_faces` — fixed-point planar pass.
- `subdivide_by_quadric_faces` — full cylinder/cone/sphere/torus pass.
- `subdivide_by_closure_planes` — synthesized bounding planes for partial
  curved faces.
- `bounding_plane_for_partial_face` — the synthesized-plane helper.
- `face_orientation_is_inward` — local convex/concave sense test.
- `drop_degenerate_pieces` — sliver/zero-volume filter.

## Phasing

Each phase is independently shippable and testable. Phases 1-2 deliver the
"always terminates, no confidence policy" win; phase 3 makes the output faithful
(one CAD solid = one cell); phase 4 is the agent/CLI/replay surface; phase 5 is the
non-planar frontier already flagged in `docs/DECOMPOSITION.md`.

---

### Phase 1 — Exhaustive surface subdivision (engine core)

**New file: `src/shatter.cpp` / `src/shatter.hpp`** (sibling of `decompose.cpp`;
reuses its `run_splitter`, `solid_box_span`, `make_supported_cutter` helpers — lift
those to `decompose.hpp` or include internally).

A fixed pipeline returning `std::vector<TopoDS_Solid>`:

1. **Closed-cylinder explode.** Detect closed 360° cylindrical faces and split them
   off first, bounding them with the perpendicular planes built from their circular
   edges. (Curved faces that wrap fully need an explicit cap plane to be a finite
   cell.)
2. `subdivide_by_planar_faces(solid)` — cut with the **infinite extension of every
   planar face**, looping until no plane produces a further split. We already build
   planar cutters in `split_solid_by_planar_faces` (decompose.cpp:139); the new part
   is the **fixed-point loop** (`while (any base still split) re-split`), not a
   single pass.
3. `subdivide_by_quadric_faces(solid)` — for each cylinder/cone/sphere/torus face,
   cut with the full quadric surface, looping per surface kind. Accept a cut only if
   total volume is preserved — reuse the volume math already in
   `decompose_solid_by_planar_faces` (cleanup.cpp:445-459).
4. `subdivide_by_closure_planes(solid)` — this is the **only genuinely new
   geometry**: synthesize the bounding plane at the angular extremes of a *partial*
   cylinder/cone (via `bounding_plane_for_partial_face`) so a wedge piece is closed
   even when no CAD face supplies that plane (rounded corners). Gate on a local
   convex/concave orientation test (`face_orientation_is_inward`).

Top-level `shatter_solid(solid, options, report)` runs 1→2→3→4,
`drop_degenerate_pieces`, and emits a `decomposition_volume_changed` warning if
summed piece volume drifts past `options.linear_tolerance`. We keep our
exact-by-default stance: treat drift beyond tolerance as failure unless an explicit
`--allow-shatter-volume-drift` opt-in is set, paralleling the existing
`--allow-bbox-fallback-auto` policy switch.

**Model mutation** follows the existing template exactly
(`decompose_solid_by_planar_faces`, cleanup.cpp:413-487): `remove_solid(model, id)`
then push each piece to `model.decomposed_pieces` with
`next_decomposed_piece_id`, `source_solid_id = id`. Add to engine_model.hpp:

```cpp
bool shatter_solid(CadModel& model, const std::string& solid_id,
                   const ConversionOptions& options = {});
```

**Acceptance:** on a non-convex test part (e.g. an L-bracket, a slotted plate),
`shatter_solid` produces N pieces, each of which passes
`validate_convex_halfspace_cell`, with summed volume within tolerance.

---

### Phase 2 — Recursion + termination guard

Wrap phase 1 so any piece that *still* fails `validate_convex_halfspace_cell` is
re-fed to `shatter_solid` up to a depth bound (reuse the `max_depth` plumbing from
`auto_decompose_bsp`). Unlike `auto_decompose_bsp`, there is **no confidence gate
and no `*_needs_decision` stop** — the subdivision is deterministic and complete, so
it either reduces to all-simple pieces or reports the specific residual piece +
surface that defeated it (`shatter_residual_nonconvex` diagnostic carrying the
`piece:N` id and offending `face:N.M`). That diagnostic is the new, far rarer,
human-escalation point.

Each pass must make progress: bound iterations and treat a no-progress pass (a cut
that only re-emits the same piece, or produces a sliver each time) as termination,
not an infinite loop.

**Acceptance:** parts that previously returned `auto_decompose_bsp_needs_decision`
now fully decompose; a contrived pathological surface still fails *honestly* with a
precise residual diagnostic rather than a silent wrong cell.

---

### Phase 3 — Union reconstruction at emit (faithful output)

Currently each piece is emitted as an independent Alea cell (emit.cpp:495-510 loops
`model.decomposed_pieces` and calls `convert_solid` per piece). To represent "one
CAD solid = one body," group pieces by `source_solid_id` and emit **one cell whose
root is `alea_union_n` of the per-piece intersection nodes**:

- Refactor the per-solid body of `convert_solid` (emit.cpp:154-210) so the
  half-space-intersection construction is callable as
  `build_simple_cell_node(sys, solid, ...) -> alea_node_id_t` (no `alea_add_cell`).
- In `convert_model` (emit.cpp:495-510), for each `source_solid_id` collect its
  active pieces, build each node, `alea_union_n` them, and `alea_add_cell` once.
- Let Alea collapse the algebra: the existing `alea_simplify_and_prune_cells` call
  (emit.cpp:529) already runs NNF + dedup + absorption. Enable surface dedup
  (`cfg.dedup`) so the shared cut surfaces between adjacent pieces are merged into
  one MCNP surface.
- Keep `options.split_union_cells` (emit.cpp:526) as the opt-out: callers who want
  the pieces as separate MCNP cells get `alea_split_union_cells` for free.

This realizes the intent already written in `docs/DECOMPOSITION.md:111` ("the
original CAD solid becomes a union of those cells … not a guessed single
intersection"), which the code does not yet do.

**Acceptance:** the decomposed part emits one cell per original solid; a
`volume` cross-check (`csgify volume in.step --compare`) matches the original;
surface count after dedup is materially lower than naive per-piece emission.

---

### Phase 4 — Session / Lua / CLI / recipe wiring

Follow the six-point spine (the path `decompose_planar` already takes):

1. **Session** — add `CommandResult shatter(Session&, const std::string& shape_id,
   IdMode, …)` to `include/csgify/session.hpp` (next to `decompose_planar`,
   session.hpp:119) and implement in `session.cpp` via the `mutate_solid` wrapper
   used at session.cpp:1142.
2. **Lua** — add `l_shatter` in `src/lua_runner.cpp` and register it in the
   metatable block (alongside `l_decompose_planar` / `l_auto_decompose_bsp`,
   around lua_runner.cpp:1161-1223). Expose an optional `{ max_depth = N,
   allow_volume_drift = bool }` table.
3. **CLI** — add `csgify simplify in.step --shatter solid:N` and a standalone
   `csgify shatter in.step [solid:N|--all]` verb in `src/main.cpp`'s dispatch.
   `--all` shatters every solid that fails convex validation — the
   "just convert the whole assembly" entry point.
4. **Recipe** — in `src/recipe.cpp`: add `if (op == "shatter") return "solid";`
   to `category_for` (recipe.cpp:51) and a `shatter` branch in `emit_solid_op`
   (recipe.cpp:285) emitting `s:shatter(<id>, { max_depth = … })`. **An op not
   wired here works once then silently drops from compacted recipes.**
5. **Decision log** — record from the engine entry via
   `DecisionLog::record(target, "shatter", params, result, produced)` with the
   produced `piece:N` ids, params at 17-digit precision (`param_double`).
6. **Tests** — extend `tests/*.sh` with a record→compact→replay case for
   `shatter`.

**Acceptance:** `csgify simplify part.step --shatter solid:7 --log d.json`,
then `csgify compact d.json --out recipe.lua`, then
`csgify replay recipe.lua --step part.step` reproduces the identical MCNP output.

---

### Phase 5 — Non-planar cutters (the active frontier)

Phase-1 step 4 (`subdivide_by_closure_planes`) is where the priorities in
`docs/DECOMPOSITION.md` land: cylinder/cone/sphere cutters and **torus splitting**
for surfaces that are not CSG-representable whole but split into representable
pieces. `make_supported_cutter` (decompose.cpp:44) already builds
cylinder/cone/sphere/torus-axis-plane cutters; the new work is the UV-extreme
closure-plane synthesis (`bounding_plane_for_partial_face`) and the torus
natural-plane split. Treat surfaces-of-revolution and arbitrary tori as the explicit
deliverable here.

---

## What we deliberately keep our own way

- **Exact-by-default.** Any volume drift is an explicit, recorded policy opt-in —
  consistent with `docs/PHILOSOPHY.md` and the existing `--allow-bbox-fallback-auto`
  precedent. We do not silently accept approximate cuts.
- **Reuse Alea's simplifier.** The boolean simplification maps onto
  `alea_simplify_and_prune_cells` + surface dedup. Do not hand-roll a boolean reducer; call
  Alea.
- **Keep the proposal/confidence path.** `propose_decomposition_candidates` stays
  for the "give me a *minimal, named* decomposition to review" workflow. The
  subdivision becomes the default "just make it convertible" engine; the scored
  proposals become the curated alternative. They share `split_solid_by_*` and the
  piece-model plumbing.

## Risk register

- **Cell/surface explosion.** A bracket can subdivide into dozens of pieces.
  Mitigated by phase-3 union + `alea_simplify_and_prune_cells` + surface dedup; verify on a
  real assembly that final surface count is acceptable, not just that it's correct.
- **OCCT splitter robustness** on near-tangent quadric cuts. `run_splitter`
  (decompose.cpp:80) already degrades to a warning; the fixed-point loops must not
  spin on a cut that produces a sliver each pass — bound iterations and treat
  no-progress as termination.
- **Orientation sign correctness.** Wrong half-space sense silently inverts a cell.
  Implement `face_orientation_is_inward` with a unit test on a known
  inside-of-cylinder piece before trusting `subdivide_by_closure_planes`.

## First milestone

Phases 1-2 behind `csgify shatter in.step --all`, validated by: every produced
piece passes `validate_convex_halfspace_cell`, summed volume within
`options.linear_tolerance`, and a part that previously hit
`auto_decompose_bsp_needs_decision` now converts end-to-end. That alone replaces the
stalling confidence policy for the common non-convex polyhedron case.

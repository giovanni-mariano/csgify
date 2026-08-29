# csgify Philosophy

`csgify` should follow an antirez-style engineering bias:

- keep the core small enough that one person can understand it
- prefer plain data and explicit functions over framework structure
- make failure states visible and useful
- build working increments, then simplify them
- avoid clever abstractions until repetition proves they are needed
- keep command-line workflows first-class
- optimize for debuggability before automation

The CAD-to-CSG problem is hard, so the code should expose intermediate decisions
rather than hiding them. For example, a spline face should produce a diagnostic
that an agent or user can act on. It should not silently mesh, approximate, or
discard geometry.

OCCT is a C++ API, so the implementation uses C++ at that boundary. The project
should still keep a C-like shape internally: simple structs, ownership made
obvious, no deep class hierarchy, and no template-heavy generic machinery.

Engine direction:

- STEP import creates a clear OCCT shape model.
- Analysis records solids, shells, faces, materials, unsupported surfaces, and
  suggested simplification actions.
- Decomposition creates explicit CSG-convertible pieces.
- Emission creates Alea surfaces, nodes, and cells.
- Export is a separate final step.

The public API should remain boring: input path, options, result, diagnostics.

## Stage model

The implementation plan names six conceptual stages (Import, Analyze, Semantic
Cleanup, Geometric Repair, Emit CSG, Export). The current code implements
those stages but does not enforce them as a typed pipeline — semantic cleanup
and geometric repair mutate `CadModel` in place, and conversion reads from
the mutated model.

This is a deliberate choice for the current scope:

- **State lives in `CadModel`.** Loaded geometry, removed solid IDs, synthetic
  bodies, decomposed pieces, and face replacements all hang off the same
  value. Commands mutate it; conversion is a final read.
- **Refusal is enforced at the emit boundary.** `convert_solid` rejects
  geometry it cannot turn into a valid CSG cell. The "geometry must be ready"
  rule is a runtime check, not a type-system check.
- **Why this over a pipeline of immutable values:** a typed pipeline
  (`RawModel → AnalyzedModel → CleanedModel → RepairedModel → AleaSystem`)
  is more rigorous and would surface mistakes earlier, but with one frontend
  (the CLI) the extra plumbing is currently overhead. The trade is "one
  mutation in flight, refusal at the gate" rather than "one type per stage,
  refusal as a constructor postcondition."

This choice is **revisitable** when any of the following becomes true:

- A second frontend exists (Lua, HTTP, Python binding) and needs to drive the
  stages independently.
- Cleanup operations start interacting in ways that are hard to read
  ("did this --replace-face-with-* see the effect of the prior
  --decompose-planar?"). At that point an immutable pipeline pays for itself.
- A stage needs to be re-run cheaply on a subset of solids without redoing
  the whole import.

Until then, keep `CadModel` as the state, document each command's mutation
contract on the function that performs it, and write tests that pin the
post-mutation report.

## Internal code layout

The engine is split into translation units along the natural stage seams:

- `src/engine_internal.cpp` — shared helpers under `csgify::detail`. ID
  formatting, BBox math, interior-point search, convex validator,
  diagnostic helpers, Vec3 math, synthetic body lookup, and other
  cross-stage utilities. Every other module pulls these in via
  `engine_internal.hpp`.
- `src/analytic_surface.cpp` — leaf, OCCT face → CSG analytic surface
  translation. Used by analyze, emit, and decompose. Owns
  `analytic_surface_from_face`, `cylinder_quadric`, `cone_quadric`, and
  the FaceReplacement override path.
- `src/decompose.cpp` — leaf, planar decomposition via `BOPAlgo_Splitter`.
  Shared between the emit stage's automatic fallback and the cleanup
  stage's explicit `--decompose-planar`.
- `src/fitted_candidates.cpp` — leaf, computes
  `fitted_{cylinder,sphere,cone}_candidate` from a solid's analytic faces.
- `src/import.cpp` — leaf, STEP reader + ShapeFix.
- `src/report.cpp` — leaf, JSON serialization of `ConversionReport`.
- `src/spline_fit.cpp` — leaf, B-spline/Bezier face → analytic fit
  candidates (plane, sphere, cylinder, cone). Exposed as
  `csgify::detail::fit_spline_face`.
- `src/analyze.cpp` — analyze stage. `inspect_solid`,
  `analyze_decomposed_piece`, `finalize_cleanup_metrics`, `analyze_model`,
  `inspect_step_file`. Pure read from `CadModel` to `AnalysisReport`.
- `src/cleanup.cpp` — cleanup / geometric-repair stage. All the
  `CadModel` mutators: `replace_solid_with_*`, `add_synthetic_*`,
  `remove_solid`, `decompose_solid_by_planar_faces`,
  `replace_face_with_fit`, `auto_fit_splines`, and the
  `apply_replacement_operation` dispatchers. This TU is the one where the
  "mutate in place vs. typed pipeline" decision is load-bearing — see the
  "Stage model" section above.
- `src/emit.cpp` — emit stage. `convert_solid`, `add_face_halfspace`,
  the `emit_synthetic_*` helpers, `convert_model`, `convert_step_file`.
  Strict: refuses geometry that isn't ready.
- `src/engine.cpp` — public-API entry points that don't fit a stage TU:
  `AleaSystem` RAII, `occt_version`, `ConversionReport::ok`,
  `compare_step_volumes`, `export_mcnp`. Under 100 lines.

The plan is to peel further leaf modules off `engine.cpp` as they prove
self-contained, following the same recipe as `spline_fit`:

- a header in `src/foo.hpp` that declares only what the rest of the engine
  needs, under `csgify::detail`;
- a `src/foo.cpp` that implements helpers in an anonymous namespace;
- one `#include "foo.hpp"` at the top of `engine.cpp`.

All stage extractions are done. Future leaf opportunities (when the code
makes them worth doing):

- A `csg/` sub-namespace if the Alea-specific emit code ever needs to
  abstract over backends (OpenMC, Serpent, etc.).
- A small `id_helpers` leaf if other namespaces start producing solid/face
  IDs.

Neither is needed today.

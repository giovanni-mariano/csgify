# csgify Review

*Reviewer persona: antirez. Original date: 2026-05-19. Updated:
2026-05-20.*

## Status update, 2026-05-20

This review started as a snapshot of the early Phase 1 codebase. Several
items below have since been addressed: the command line has a small parser,
the engine was split into stage-oriented translation units, contract fixtures
exist, spline-fit diagnostics are part of the agent protocol, and face
replacement commands now record stable shape IDs separately from human labels.

The main remaining architectural caveat is intentional for the current
phase: `include/csgify/engine.hpp` is still the native C++ engine boundary
and therefore exposes OCCT types in `CadModel` and decomposition records. That
is acceptable while the engine is still being shaped around OCCT 8. The later
scripting/agent API should be a smaller opaque boundary, with stable IDs and
commands rather than `TopoDS_*` objects.

I read the README, the philosophy doc, the implementation plan, the public
header, `main.cpp`, and most of `engine.cpp`. This is the kind of project I
like to look at: a single binary, an explicit problem, no framework on top.
Below is what I think after one pass. Take it as a set of opinions, not a
checklist of bugs.

## What is good

The shape of the project is right.

- The project is still small enough for one person to understand, but it is
  no longer one huge engine file. The current split follows the real stages:
  import, analyze, cleanup, decomposition, emit, and report. That keeps the
  antirez-style constraint that the code should be direct, while making the
  engine easier to navigate.
- The public API in `include/csgify/engine.hpp` is plain data: `BBox`,
  `Vec3`, `AnalyzedFace`, `AnalyzedSolid`, `ConversionReport`, plus free
  functions. No deep class hierarchy, no templates, no PIMPL. The only
  RAII type is `AleaSystem`, which exists because `alea_system_t*` needs
  an owner. Good restraint.
- The diagnostic model is the right idea for this problem. Severity + code +
  message + shape label, accumulated into the report, surfaced by both the
  CLI and the JSON output. Refusing spline faces *by default*, with a
  structured diagnostic the user or an agent can act on, is the right
  discipline. The point is "no silent approximation," not "no approximation
  ever." See the spline section below.
- Stable IDs (`solid:1`, `face:1.4`, `synthetic:1`, `piece:1`) are a small
  thing that pays off forever once an agent or a human is making decisions
  on top of the report. Cheap to add now, very expensive to retrofit later.
- The "inspect → simplify → convert" split, with `--report` writing a
  machine-readable JSON of the analyzed model, is the correct shape for
  the "agent in the loop" workflow the plan describes. The CLI is the
  protocol. That is a good place to keep it.
- `apply_checked_replacement_operation` putting the limit check and the
  mutation on the same path as the CLI is the right call. Future Lua,
  Python, or agent bindings will not drift from the CLI semantics, which
  is the usual way these tools rot.

## What I would push back on

None of these are emergencies. They are the things that will start to hurt
around 6–12 months from now if left alone.

### 1. `main.cpp` argument parsing is fragile

Status: addressed for the current command set.

`run_volume_command` dispatches by `argc != 3 && argc != 5 && argc != 7
&& argc != 9`. This works today and will silently misbehave the first time
someone adds an option that takes zero arguments, or a flag that can repeat.
The top-level `main` has the same flavor: hand-written shape matching with
`argc == N && strcmp(argv[k], "...")`.

I would not pull in a CLI library for this. I would write ~80 lines of a
plain token-stream parser:

```c
// pseudo
parser p = parser_init(argc, argv);
while ((tok = parser_next(&p))) {
    if (tok_is(tok, "--compare")) compare = parser_value(&p);
    else if (tok_is(tok, "--abs-tol")) abs_tol = parser_double(&p);
    ...
}
```

Same spirit as the rest of the codebase, no dependency, and the bug surface
shrinks by an order of magnitude. Do this before the CLI grows another
subcommand; right now you have four (`<default>`, `inspect`, `simplify`,
`volume`) and the cracks are already visible.

### 2. The replacement operations are starting to repeat themselves

`replace_solid_with_bbox`, `..._bbox_cylinder`, `..._fitted_cylinder`,
`..._fitted_sphere`, `..._fitted_cone`, plus a parallel
`apply_replacement_operation` switch, plus a parallel `--replace-solid-*`
CLI flag set, plus a parallel set of `ReplacementMetrics` /
`Fitted*Candidate` structs, plus a parallel `operation_from_action` mapping.

Five primitives is the moment where the "no premature abstraction" rule
flips. Each new primitive currently requires touching ~5 places. The
philosophy doc says "avoid clever abstractions until repetition proves
they are needed" — the repetition has now proved itself. I would:

- Keep `ReplacementOperation` as the single source of truth.
- Collapse the per-shape `Fitted*Candidate` into one `FittedPrimitive`
  with a `kind` enum and the union of fields it actually needs. The
  alternative is fine too: keep them separate but stop duplicating
  `ReplacementMetrics metrics;` on each — pull it up.
- Drive the CLI from a table:
  `{ "--replace-solid-bbox", ReplacementOperation::bbox }`,
  iterated in one place. Adds a primitive in one spot, not five.

### 3. `engine.cpp` is one big file

Status: addressed. The engine is now split by stage.

3k lines in one translation unit is fine *as long as it stays readable
top-to-bottom*. It is on the edge right now. The natural seams are already
written into the philosophy doc — Import, Analyze, Semantic Cleanup,
Geometric Repair, Emit, Export. Splitting along those seams (six files of
~500 lines, or four if Import/Export are tiny) would let a reader of the
code reproduce the architecture diagram without effort.

I would *not* split it into `engine_internal.hpp` plus headers per stage.
Keep the includes flat: `import.cpp`, `analyze.cpp`, `cleanup.cpp`,
`emit.cpp`, sharing a single `engine_internal.hpp` with the private
helpers. Forward declarations across the file (see lines 51–56) are a
symptom that the split wants to happen.

### 4. Some duplication of intent between analyze and convert

`inspect_solid` walks faces and classifies them. `convert_solid` walks the
same faces and re-emits them as halfspaces. `validate_convex_halfspace_cell`
is called from both paths with different `count_*` flags. The flags are a
mild smell: a function that has been parameterized to behave three
different ways for three different callers usually wants to be split.

Not urgent. But when you do the stage split above, I would make analyze
produce a concrete `ConvertibleSolid` value (interior point, ordered face
list with their analytic surfaces already extracted) and have emit
consume it. Then `validate_convex_halfspace_cell` lives once, in analyze,
and the booleans disappear.

### 5. `kEps = 1.0e-14` lives next to `options.linear_tolerance = 1e-7`

There are at least three tolerance regimes floating around: `kEps` for
near-zero algebraic checks, `linear_tolerance` for OCCT operations,
`small_volume_fraction` and friends for cleanup heuristics. They are not
the same thing and the code is mostly right, but the constants are spread
out enough that I had to grep to be sure. One short comment block at the
top of the file naming them and what each is for would save the next
reader (or yourself in a year) some time.

### 6. Tests

`tests/phase1_smoke.sh`, 114 lines, is the entire test suite. For a tool
that mutates CAD and emits MCNP this is thin. I do not believe in
heavyweight test infrastructure for projects this size, but I would want
at least:

- A handful of tiny canned STEP files checked into the repo (a box, a
  sphere, a cylinder, a non-convex L-shape, a bolt with a spline).
- A test that round-trips `volume` and `volume --compare` and checks the
  exit codes.
- A test that asserts `inspect` produces the diagnostic codes the README
  promises (`unsupported_spline_face`, `nonconvex_solid_requires_decomposition`,
  etc.) — those codes are part of the contract with agents now.

Shell + diff is fine. The point is to pin the contract, not to be fancy.

### 7. Splines need a treatment plan, not just a refusal

Today `unsupported_spline_face` is a hard stop with no second step. That
is the correct *default*, but it is not the end state. A real CAD model
from a vendor will have spline faces that are geometrically trivial
(filleted edges, draft surfaces, near-cylindrical revolutions) and the
tool needs an explicit path to handle them. Otherwise users will route
around `csgify` — pre-process the STEP in another tool, then feed in
something the engine accepts — and you lose the audit trail the
diagnostic model exists to provide.

The path I would build, in order:

1. **Classify the spline.** When a B-spline face is encountered, attempt
   to fit it against the analytic primitives the engine already supports
   (plane, sphere, cylinder, cone, torus) within an explicit tolerance.
   Report `spline_fits_cylinder` (or whatever) with the fitted parameters
   and the residual. Same shape as the existing `replacement_candidates`
   metrics on solids. No mutation yet — just a candidate.
2. **Offer an explicit replacement command.** `--replace-face-with-plane
   face:1.4 --max-deviation 0.01`, mirroring the existing
   `--replace-solid-*` family. The user (or the agent) accepts the fit
   with a tolerance budget; the report records the deviation. This is
   the same decision model that already exists for solid replacement,
   just one level finer.
3. **Last resort: tessellate under a recorded tolerance.** If no analytic
   fit works and the user accepts it, tessellate the spline into planar
   facets and emit a (probably large) CSG intersection or
   piecewise-convex decomposition. This is the "approximation allowed
   only when recorded with tolerances and accepted" clause the
   implementation plan already promises. The diagnostic should carry
   the chord/angle tolerance used and the resulting facet count, so the
   neutronics person can see the cost.

What I would *not* do: implicit meshing on the convert path, or a
"--allow-splines" global flag that turns off the diagnostic without
recording per-face decisions. Either of those defeats the audit model.

The fitting code in (1) is the cheap, high-value first step and is mostly
a sampling-and-least-squares exercise against surfaces the engine already
knows how to emit. I would do (1) before any of the refactors in §1–§5.

## What I would not change

- The OCCT 8.0.0 EXACT pin. Annoying for packagers, correct for this
  project's stage. Float it later when there is a real reason.
- C++ at the OCCT boundary, C-shaped data internally. This is the right
  call for a tool that may grow Lua or Python bindings later.
- The decision to put `synthetic:N` and `piece:N` in the same namespace
  family as `solid:N` and `face:N.M`. Consistent, greppable, sortable.

## One thing to decide soon

The implementation plan describes six stages; the code today fuses
Analyze and Emit, and Semantic Cleanup mutates `CadModel` in place.
That is fine for the current scope but it means the "refuse geometry
that is not ready" rule from the plan is enforced inside `convert_solid`
rather than at a stage boundary.

Pick one of these, explicitly:

1. **Mutate in place, single pass.** Accept that `CadModel` is the state,
   commands mutate it, conversion is the final read. Document this and
   stop pretending there are six stages.
2. **Pipeline.** Each stage produces an immutable value consumed by the
   next: `RawModel → AnalyzedModel → CleanedModel → RepairedModel →
   AleaSystem`. More code, but the refusal rule becomes a type, not a
   runtime check.

Either is defensible. Picking neither is the worst option, because every
new feature will get to choose for itself, and the answer will diverge.
My weak preference is (1) for now and (2) once a second frontend (Lua,
HTTP, whatever) appears.

## Summary

This is a small, honest, well-scoped project that does what it says.
The bones are good. The things I flagged are the kind of "second-system
hygiene" you do *before* the codebase doubles in size, not after.
Priority order if you want one:

1. Build the spline analytic-fit pass (§7 step 1): report fitted-primitive
   candidates for B-spline faces. High user value, doesn't touch the
   refusal contract, and reuses the existing replacement-metrics shape.
2. Add `--replace-face-with-*` commands (§7 step 2) once the candidates
   are in the report. Do this *before* the table-driven refactor in (4),
   so the table is designed against the real shape of the feature set.
3. Add canned STEP fixtures (including a spline case) and pin the
   diagnostic-code contract.
4. Collapse the replacement-operation code paths — now covering both
   solid-level and face-level replacements — into one table-driven path.
5. Replace the `argc == N` dispatch with a small argument parser.
6. Split `engine.cpp` along the stages the philosophy doc already names.
7. Decide explicitly between "mutate in place" and "pipeline" and write
   it down.

Tessellation fallback (§7 step 3) is later — it is the largest piece of
code and the least important to get right early.

Everything else can wait.

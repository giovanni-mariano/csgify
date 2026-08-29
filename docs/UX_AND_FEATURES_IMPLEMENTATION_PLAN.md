# UX and Missing-Features Implementation Plan

This plan turns the CAD-side review findings into concrete work. The scope is
**the CAD half of csgify**: STEP import, analysis, decomposition, surface
fitting, and the CLI/Lua workflow that drives them. MCNP deck completeness
(materials, graveyard cell, importances) is **libalea's responsibility** and is
explicitly out of scope here.

The governing rule stays the one in `docs/PHILOSOPHY.md`:

```text
Keep the core small, expose intermediate decisions, make failure visible,
keep the command line first-class, optimize for debuggability.
```

Several findings below are cases where the edges of the tool have drifted from
that rule. The plan is ordered by how much each item hurts a real user, not by
how hard it is to build. The cheap usability fixes come first on purpose: they
decide whether anyone enjoys driving the tool at all.

## Goals

- Make the common first commands (`inspect`, `--help`) behave the way a user
  expects, on the right stream, with the right exit code.
- Stop being silent about units. Report the STEP length unit everywhere and
  let the user scale explicitly.
- Treat instanced assemblies as the norm: decide once, apply to all instances.
- Give the user a single front door to "why won't this convert?" and "do
  everything safe, then tell me what's left."
- Converge the two CLI idioms (`*-suggest` verbs vs. `simplify --apply-*`) onto
  one consistent propose/apply shape.

## Non-Goals

- No MCNP deck semantics (materials, void, importances, source, tallies). That
  is libalea.
- No GUI work beyond what already exists in `viewer.cpp` / decision packets.
- No rewrite of the `CadModel`-mutates-in-place stage model. The
  `PHILOSOPHY.md` "Stage model" decision stands until a second frontend forces
  it.
- No new heavy dependencies. Unit handling and instancing use OCCT facilities
  already linked.

---

## Workstream 1 — CLI usability (the cheap wins, do first)

These are small, independent, and high-impact. None should take more than a day
together. Land them as one PR so the CLI "feels" fixed in a single step.

### 1.1 `inspect` must work without `--report`

**Problem.** `run_inspect_command` (`src/main.cpp:1908`) hard-fails when
`--report` is absent:

```c
if (report_path.empty()) { usage(argv[0]); return EXIT_FAILURE; }
```

The most natural first command — "let me look at this file" — dumps the full
usage and exits 1. The summary data is already computed
(`print_summary`/`print_diagnostics` run unconditionally).

**Design.** `--report` becomes optional. When absent, print a readable
human summary to **stdout** (not stderr) and exit on `report.ok()`. When
present, keep current behavior (write JSON, summary to stderr).

**Steps.**
- Drop the `report_path.empty()` guard.
- Add a `print_human_inspect(report, stdout)` helper: solids seen/converted,
  faces analytic/spline, per-solid one-line status (ready / decompose / spline
  / decision), total volume **with unit** (see WS2).
- Route the human summary to stdout; keep machine JSON behind `--report`.

**Tests.** Extend `tests/phase1_smoke.sh`: `inspect box.step` (no `--report`)
exits 0 and prints a non-empty summary to stdout.

### 1.2 Real `help` / `--help` / `-h`

**Problem.** `--help` falls through to `run_default_convert`
(`src/main.cpp:2028`), fails the `argc != 3` check, prints usage to **stderr**,
exits **1**. A user who asked for help got an error code and an empty stdout.

**Design.**
- In `main()` (`src/main.cpp:1997`), match `help`, `-h`, `--help` first → print
  usage to **stdout**, exit **0**.
- Per-command help: `csgify <verb> --help` prints only that verb's options.
  Implement by giving each `run_*_command` an early `--help` check that calls a
  per-verb usage string.

**Steps.**
- Split the monolithic `usage()` (`src/main.cpp:44`) into a top-level synopsis
  plus a `usage_for(verb)` lookup. Keep it plain data: a `{verb, synopsis,
  options[]}` table, no framework.
- Each `run_*_command` checks `argv` for `--help`/`-h` before parsing.

**Tests.** New `tests/help_smoke.sh`: `help` exits 0, stdout non-empty;
`simplify --help` mentions `--replace-solid-bbox`; unknown `--help` of a real
verb does not error.

### 1.3 Reject unknown verbs explicitly

**Problem.** Any unrecognized first argument is treated as `input.step` via the
`run_default_convert` fall-through (`src/main.cpp:2028`). `csgify inspct
model.step` tries to read a STEP file literally named `inspct` and reports
`step_read_failed` — blaming the file, not the typo.

**Design.** Keep the bare two-arg convert form (`input.step output.i`) but only
take it when `argv[1]` looks like a path (ends in `.step`/`.stp`, or exists on
disk). Otherwise, if `argv[1]` starts with a letter and is not a known verb,
emit `unknown command '<verb>'` plus a nearest-match suggestion and exit 2.

**Steps.**
- Build the verb list as a `static const char* kVerbs[]` (it already exists
  implicitly in the `main()` dispatch chain — lift it to data).
- Add a tiny Levenshtein (≤ 30 lines, no dep) for "did you mean".
- Gate `run_default_convert` on the path-shaped heuristic.

**Tests.** `tests/help_smoke.sh`: `csgify inspct x.step` prints
`unknown command` + `did you mean 'inspect'`, exits 2.

### 1.4 Document and bless stdout for reports

**Problem.** Everything is file-in/file-out. `--report /dev/stdout` already
works but is undiscoverable, and the primary documented consumer is agents
(`docs/AGENT_WORKFLOW.md`).

**Design.** Adopt the Unix convention: `--report -` means stdout. Same for
`--out-step -` where it makes sense. Document it in usage and README.

**Steps.**
- In the report-writing helpers (`write_report_json` call sites), map `-` to
  stdout before `fopen`.
- Update `usage_for` strings and README piping examples (`… --report - | jq`).

**Tests.** `inspect box.step --report - | head` yields JSON on stdout.

---

## Workstream 2 — STEP units (correctness, not polish)

**Problem.** `src/import.cpp` never reads the STEP length unit. `box.step`
declares `SI_UNIT(.MILLI.,.METRE.)`; csgify reports `volume: 100` (mm³) with no
unit label. Every volume, every bbox extent, and every `--max-abs-delta` guard
is in undeclared units. Wrong-by-1000× is the worst kind of silent failure and
directly violates "make failure states visible."

**Design.**
- On import, read the model's length unit. With `STEPControl_Reader` the unit
  is available via `Interface_Static` (`"xstep.cascade.unit"`) and the file's
  `SI_UNIT`; capture the source unit and the scale-to-mm factor.
- Record it in `ConversionReport`: add `std::string length_unit` and
  `double length_unit_to_mm` (so downstream/libalea can convert deterministically).
- Print the unit in every human summary and the JSON report.
- Add `--units <mm|cm|m|in>` and `--scale <factor>` to import-bearing commands
  to let the user override/normalize. Default: **report only, do not silently
  rescale** (consistent with "no hidden magic"). A normalize flag
  (`--normalize-units cm`) opts into actual scaling and logs it as a decision.

**Steps.**
1. `src/import.cpp`: capture unit + factor in the imported-model struct.
2. `include/csgify/engine.hpp`: add the two report fields.
3. `src/report.cpp`: serialize them under a new `units` block.
4. `src/analyze.cpp` / `print_summary`: show `volume: 100 mm^3`.
5. CLI: `--units` / `--scale` / `--normalize-units` parsed by `OptScanner`,
   threaded into `ConversionOptions`.
6. If `--normalize-units` is set, apply a `BRepBuilderAPI_Transform` scale at
   import and record a decision-log entry.

**Risks.** Assembly subtrees can carry per-component units in pathological STEP
files; for the first pass, read the top-level unit and emit a `warning
mixed_units` diagnostic if XCAF reports inconsistent units rather than guessing.

**Tests.** Add an mm and a cm fixture of the same nominal box;
`inspect` reports the right unit; `--normalize-units cm` makes their reported
volumes equal within tolerance; decision log records the scale.

---

## Workstream 3 — Assembly instancing (highest domain value)

**Problem.** The domain is dominated by instanced assemblies — fuel pins, bolts,
repeated modules (see memory note `project_domain_mcnp`). csgify parses the
XCAF assembly tree into `AssemblyNode` (names, paths, labels are in the report),
but **conversion flattens every occurrence** into a linear
`TopExp_Explorer(TopAbs_SOLID)` `solid:N` order (invariant in
`project_solid_id_invariant`). 200 identical pins become 200 independent
decisions and 200 independent cells, with no notion that they are one part at
different placements.

**Design (CAD side only).** Add an *instance grouping* analysis pass that
clusters solids that are the same part modulo a rigid transform. This is a
read-only enrichment of the report — it does not change `solid:N` identity, so
the invariant holds.

- **Geometric signature.** For each solid compute a transform-invariant hash:
  sorted face-type histogram + sorted area/volume multiset + bbox extents
  (sorted) + topology counts (faces/edges/vertices). Use the existing FNV-1a
  (see `project_hash_choice`). Same signature ⇒ instance-candidate group.
- **Transform recovery.** For solids in a group, recover the placement
  transform relative to a chosen representative (XCAF already stores location;
  prefer that, fall back to aligning principal axes / a coincident-vertex
  match). Store as a `Vec3` translation + a rotation (quaternion or 3×3).
- **Report shape.** Add an `instance_groups[]` section: `representative_solid_id`,
  `member_solid_ids[]`, `count`, per-member transform, and a confidence
  (`exact` if XCAF location-derived, `geometric` if hash+align-derived).

**Decide-once, apply-to-all.** A new flag on the mutating commands:
`--apply-to-instances` (CLI) / `apply_to_instances = true` (Lua). When the user
makes a decision on the representative, the engine replays the *same operation*
on every member, mapping face IDs through the recovered transform. The decision
log records the group, the representative decision, and the member expansion so
replay is deterministic.

**Steps.**
1. New leaf `src/instancing.cpp` / `.hpp` under `csgify::detail`:
   `compute_solid_signature`, `group_instances`, `recover_transform`.
2. `analyze_model` (`src/analyze.cpp`) calls it after solids are analyzed; fill
   `instance_groups[]`.
3. `src/report.cpp`: serialize the new section.
4. Cleanup dispatchers (`src/cleanup.cpp`): teach `apply_replacement_operation`
   and the decompose entry points to optionally fan out across a group.
5. Decision log / replay (`src/decision_log.cpp`, `src/replay.cpp`): record and
   replay group-scoped decisions by signature, not by transient `solid:N`.
6. Lua + CLI surfaces.

**Risks.** Transform recovery is the hard part. Ship the XCAF-location path
first (high confidence, covers clean exports); gate the geometric-align path
behind `geometric` confidence and never auto-apply it without
`--allow-medium-confidence-auto`-style opt-in. Mirror parts (handed instances)
need a reflection in the transform — detect negative determinant and flag.

**Tests.** A fixture with N copies of one part (grid of pins). Assert one
`instance_group` with `count == N`; a single `--replace-solid-fitted-cylinder
--apply-to-instances` converts all N; volume delta scales by N; replay
reproduces it.

---

## Workstream 4 — `explain` command (close the decision loop)

**Problem.** The data to answer "why won't `solid:7` convert?" exists
(`needs_spline_replacement` / `needs_surface_replacement` /
`needs_decomposition` / `needs_user_decision` per face + diagnostics), but the
human has to assemble it from JSON. The philosophy is "expose intermediate
decisions" — give it a front door.

**Design.** `csgify explain <shape-id> input.step` prints, in one shot:
- the solid's status and the reason it is not emit-ready,
- the exact blocking faces with surface type and the specific blocker,
- the suggested action per face (replace-with-fit / decompose / remove),
- the relevant proposals already available (decompose / surface candidates),
  with their confidence and volume delta.

It is a read-only view over the existing analysis + proposal calls — no new
geometry logic. Works on `solid:N`, `piece:N`, and `face:N.M`.

**Steps.**
1. `run_explain_command` in `src/main.cpp`: analyze, then for the target
   collect blocking faces and call the matching `propose_*` to surface options.
2. Human output to stdout; `--report -`/file for the JSON form (reuse existing
   proposal serializers).
3. Add to the verb table + per-verb help (WS1).

**Tests.** `explain solid:1 spline_face.step` names the spline face and suggests
`replace-face-with-*`; exit code reflects whether the target is already ready.

---

## Workstream 5 — `preprocess --safe-all` (one front door for the safe pass)

**Problem.** `auto-fit`, `decompose-apply`/`--auto-decompose-bsp`,
`remove-suggest`+apply, and `hole-fill-suggest`+apply are separate verbs the
user must sequence correctly. The high-value workflow — "apply every
high-confidence safe op, leave the rest for me, hand me the list" — is not
composed anywhere.

**Design.** Extend `run_preprocess_command` with `--safe-all`, which runs, in a
fixed documented order, on one load:
1. auto-fit splines that meet `--max-deviation`,
2. safe BSP decomposition (high-confidence face-driven only),
3. explicitly authorized removal candidates,
4. high-confidence hole fills (only when explicitly enabled — fills are modeling
   decisions, per `HOLE_FILL_IMPLEMENTATION_PLAN.md`; default off).

Every applied op is logged to the decision log; everything skipped is reported
as a remaining decision in a final "what's left for you" summary. Defaults stay
conservative — this composes the *existing* safe policies, it does not loosen
them.

**Steps.**
1. Compose existing session calls in `run_preprocess_command`; no new engine
   ops.
2. Emit a closing summary: applied N, skipped M with reasons, K decisions left.
3. `--safe-all --dry-run` prints the plan without mutating (reuses proposal
   metrics).

**Tests.** `preprocess --safe-all` on `rails.stp` (the private fixture, kept out
of published docs/skills per `project_rails_findings`) applies the safe splits
and leaves the known-hard pieces listed; `--dry-run` mutates nothing.

---

## Workstream 6 — Converge the propose/apply idioms

**Problem.** Two idioms for one concept: standalone `remove-suggest`,
`surface-suggest`, `hole-fill-suggest`, `decompose-suggest` **and** `simplify
--apply-decomposition-candidate` / `--apply-surface-candidate`. Propose lives in
verbs; apply lives in `simplify` flags. The user holds two mental models.

**Design.** Introduce a consistent noun-first pair and **alias** the old verbs
to it (do not break existing scripts/recipes):

```text
csgify propose <kind> <target> input.step [--report -]
csgify apply   <candidate-id> <target> input.step --out out.i [--log ...]
```

where `<kind>` ∈ `decompose | surface | removal | hole-fill`. The old verbs and
`simplify --apply-*` flags remain as thin shims that print a one-line
deprecation note and forward. Revisit removing the shims only once usage proves
the new shape (per philosophy: don't abstract until repetition proves it — here
it has).

**Steps.**
1. `run_propose_command` / `run_apply_command` dispatch on `<kind>` to the
   existing `propose_*` / `apply_*` session calls.
2. Keep `simplify` for the direct single-operation actions
   (`--replace-solid-*`, `--decompose-*`); move only the *candidate-apply* flags
   under `apply`.
3. Update `docs/AGENT_WORKFLOW.md` to lead with the new shape.

**Tests.** Old `decompose-suggest`/`simplify --apply-decomposition-candidate`
smoke tests still pass; new `propose decompose` / `apply` smoke covers the same
path.

---

## Workstream 7 — Document the ID / mutation contract

**Problem.** `solid:N` is positional; after decomposition the source is
suppressed and results become `piece:N`. The philosophy doc promises "document
each command's mutation contract on the function that performs it" — that
promise needs to reach the user, not just the source.

**Design.** No code behavior change. For each mutating verb, document in
`--help` (WS1) and `AGENT_WORKFLOW.md`:
- what it suppresses,
- what new IDs it creates (`synthetic:N`, `piece:N`),
- whether surviving `solid:N` IDs shift (they must not — `TopExp` order of
  surviving solids is stable; assert this with a test).

**Steps.**
1. Add a "Mutation contract" line to each per-verb help string.
2. Add `tests/id_stability_smoke.sh`: after `remove-solid solid:3`, the
   remaining `solid:N` IDs still resolve to the same geometry (pin volumes).

---

## Out of scope here, tracked elsewhere

- **Non-planar decomposition breadth** (general-quadric *fitting*,
  surfaces of revolution beyond torus/cone, fillet/chamfer handling). This is a
  large geometry track, not a UX fix; it belongs in
  `docs/DECOMPOSITION_AND_SURFACE_REPLACEMENT_PLAN.md`. Listed here only so the
  review's point #3 is not lost.
- **Generic synthetic-body creation from Lua** and **boolean synthetic
  combinations** — already tracked under "Missing Pieces" in
  `docs/AGENT_WORKFLOW.md`.

---

## Sequencing

| Milestone | Contents | Rationale |
|-----------|----------|-----------|
| M1 (≈1 day) | WS1 (all four), WS7 docs | Cheapest, highest first-impression impact; no engine risk. |
| M2 (≈1–2 days) | WS2 units | Correctness; unblocks meaningful guards and downstream libalea conversion. |
| M3 (≈1 day) | WS4 explain, WS5 safe-all | Pure composition over existing calls; closes the decision loop. |
| M4 (≈1 day) | WS6 propose/apply convergence | Consistency; aliased so nothing breaks. |
| M5 (multi-day) | WS3 instancing | Highest domain value, highest risk; ship XCAF-location path first, geometric-align path behind opt-in. |

M1–M4 are independent and can land in any order. M5 depends on WS2 (units) only
insofar as instance volume deltas should be unit-labeled.

## Testing strategy

- Every workstream adds a `tests/*_smoke.sh` following the existing pattern;
  keep `rails.stp` out of any published doc/skill (`project_rails_findings`).
- Pin post-mutation reports (per `PHILOSOPHY.md` stage-model guidance).
- Units and instancing get dedicated fixtures (mm/cm boxes; an N-copy grid).
- No change to the `OpenCASCADE 8.0.0 EXACT` requirement; all OCCT facilities
  used (unit query, `BRepBuilderAPI_Transform`, XCAF locations) are in 8.0.0.

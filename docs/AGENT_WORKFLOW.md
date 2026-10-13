# Agent Workflow

The default workflow uses Codex, Claude Code, Pi, or another external agent as
the conversation and planning interface. csgify does not need a separate LLM
API key for this path. The Qt workspace provides geometry inspection, preview,
and human approval; it does not open an embedded chat by default.

Ask the external agent to:

1. Read `csgify context` for the source model and project policy.
2. Choose an advertised action and submit it through
   `csgify autonomy --mode propose`.
3. Open the resulting decision queue in the Qt workspace for human review.
4. After approval, continue through the supported queue workflow and verify
   the resulting geometry and export.

The strict `context`/`autonomy`/`queue` contract is documented in
[CLI_AGENT_PROTOTYPE.md](CLI_AGENT_PROTOTYPE.md). It binds requests to source
and policy identity and does not give the planner general scripting authority.

## Embedded project conversation

The optional embedded chat requires explicit opt-in with
`CSGIFY_ENABLE_EMBEDDED_CHAT=1`. Without that setting, the workspace creates no
conversation dock or provider, even if API credentials are present.

When enabled, the Qt project workspace can answer questions about the loaded
model through an embedded OpenAI provider. The key is read from the process environment,
kept in memory, and never written to the project database. An explicit model
identifier is also required; csgify does not silently select or change models.

```sh
cmake -S . -B build -DCSGIFY_ENABLE_QT_VIEWER=ON
cmake --build build -j2

export CSGIFY_ENABLE_EMBEDDED_CHAT=1
export OPENAI_API_KEY='<your-api-key>'
export CSGIFY_OPENAI_MODEL='<model-id-available-to-your-project>'

./build/csgify project init demo-project
./build/csgify project review demo-project --source model.step
```

Load the model in the workspace, then use the **Project conversation** dock.
The header must say **OpenAI … · advisory** before a live request is sent. If
either environment variable is absent, the dock intentionally uses and labels
the built-in simulated provider, so the UI can still be demonstrated without
network access or API spend.

Each question first builds a bounded, revision-bound evidence snapshot. Only
that curated JSON and recent conversation messages are sent; STEP files,
screenshots, project files, API credentials, and database contents are not.
Responses are constrained to the local response schema and validated again by
csgify. The current live slice answers questions only: it cannot mutate
geometry, approve operations, or export. Stop cancels the in-flight context or
network request, and a response is discarded if its model revision became
stale while waiting. Questions and terminal answers are retained in schema-8
project storage and restored when the project is reopened.

The Lua workflow below is for batch scripts the user explicitly trusts, not
for untrusted model output. Its preferred interface is:

```sh
csgify lua script.lua [args...]
```

The agent writes or edits a Lua script, runs it, reads table or JSON reports,
adjusts the script, and repeats. A decision log plus compacted recipe is the
preferred durable audit trail for preprocessing decisions.

## CLI Quick Reference (post-UX update)

The CLI exposes a small set of verbs. All accept `--help`/`-h` for per-verb
detail (including a mutation contract). `help` or `help <verb>` prints the
same content to stdout with exit 0. A `--report -` writes JSON to stdout
instead of a file.

The propose/apply pair is the unified front door over candidate generation
and application:

```sh
# Generate candidates (read-only; same engine paths as the legacy *-suggest verbs).
csgify propose decompose solid:7 in.step --report propose.json
csgify propose surface   face:1.4 in.step --report propose.json
csgify propose removal              in.step --report propose.json
csgify propose hole-fill            in.step --host solid:4 --report propose.json

# Apply a chosen candidate.
csgify apply decompose solid:7 in.step --candidate candidate:3 --out out.i --log d.json
csgify apply surface   face:1.4 in.step --candidate candidate:1 --out out.i --log d.json
csgify apply removal   solid:9 in.step --out out.i --log d.json
csgify apply hole-fill solid:4 in.step --candidate hole-fill:2 --out out.i --log d.json
```

The legacy `decompose-suggest` / `remove-suggest` / `surface-suggest` /
`hole-fill-suggest` verbs still work but emit a `deprecated_verb` info
diagnostic; existing scripts/recipes do not need to change immediately.

The `inspect` verb is the first-look command. Without `--report` it prints
a human-readable summary to stdout; with `--report PATH` (or `--report -`)
it writes JSON. `inspect` now surfaces the STEP length unit:

```sh
csgify inspect in.step                          # human summary, unit on every line
csgify inspect in.step --units cm               # override the asserted unit
csgify inspect in.step --normalize-units mm     # actually rescale geometry to mm
```

The `explain <shape-id> input.step` verb prints a focused explanation of
why a target solid/piece/face is not emit-ready -- exit code 0 if the
target is already emit-ready, nonzero otherwise. It is read-only and
consumes the same `propose_*` calls under the hood.

The `preprocess --safe-all` flag composes conservative passes on one load:
explicitly bounded spline fitting (only when `--max-deviation` is supplied),
and high-confidence BSP decomposition. Semantic feature removal is opt-in via
`--apply-removal-candidates high|medium`. Missing `--max-deviation` means
"leave splines untouched", never "fit without a bound".
Use `--dry-run` to see the plan without mutating; the closing summary
reports what's left for the user to decide.

## Basic Loop

1. Load the STEP file.
2. Analyze the document.
3. Read diagnostics, assembly metadata, and candidate metrics from the report.
4. Apply explicit cleanup or repair commands with tolerances.
5. Compare volumes when simplification changed geometry.
6. Emit only when the report is acceptable.

Example:

```lua
local s = csgify.session()
local doc = s:load_step(arg[1])

local report, err = s:analyze_table(doc)
if not report then error(err) end

for _, solid in ipairs(report.solids) do
    print(solid.id, solid.name, solid.hierarchy_path, solid.volume)
end

report, err = s:replace_solid(doc .. "/solid:4", "fitted_cylinder", {
    max_rel_delta = 0.02
})
if not report then error(err) end

report, err = s:replace_face_with_fit(doc .. "/face:2.3", "cylinder", 1e-6)
if not report then error(err) end

report, err = s:emit(doc, arg[2])
if not report then error(err) end
print(report.json)
```

## Current Lua API

Session creation:

```lua
local s = csgify.session()

-- Optional: pass a ConversionOptions table. Recognized keys mirror the
-- CLI flags introduced in M2 (`units`, `scale`, `normalize_units`) plus
-- engine tuning knobs (`heal_shape`, `linear_tolerance`, etc.).
local s = csgify.session({
    units            = "cm",   -- assert source unit (no rescale)
    normalize_units  = "mm",   -- actually rescale geometry to mm
    heal_shape       = true,
})
```

Documents:

```lua
local doc = s:load_step("model.step")

-- s:load_step also accepts a per-document options override.
local doc2 = s:load_step("alt.step", { normalize_units = "mm" })

-- Return shape for analyze (and every mutating command):
--   * success: (report_table)
--   * failure: (nil, report_table)
-- `result.ok` means "the requested operation applied". To decide
-- whether the model is emit-ready, inspect `report.ok` (boolean) or
-- the per-solid `exact_csg_supported` / `needs_*` flags. A report can
-- have `ok = false` while the op that produced it succeeded -- that
-- just means there are still error-severity diagnostics elsewhere in
-- the model.
local report, err = s:analyze_table(doc)
if not report then error(err) end  -- only the op-failure path
if not report.ok then
    print("model still needs work; " .. #report.diagnostics .. " diagnostics")
end
```

Reports use schema `csgify.report` version 1 and include
`document_revision`, per-solid `geometry_certificate`, and `provenance`.
Mutations are atomic: a rejected operation returns `(nil, report)` without
advancing the revision. Use `s:document_revision(doc)` to check and
`s:undo_last_mutation(doc)` to restore the preceding committed state as a new
revision. `csgify.operations()` exposes the versioned operation registry used
by CLI parsing, Lua dispatch, decision logs, compaction, and replay.

Decision logs (matches the CLI `--log decisions.json`):

```lua
s:attach_decision_log("decisions.json")
-- Subsequent mutating ops record entries; compact later with the CLI:
--   csgify compact decisions.json --out recipe.lua
```

Cleanup and repair:

```lua
s:remove_solid("doc:1/solid:12")
s:replace_solid("doc:1/solid:4", "bbox", { max_rel_delta = 0.05 })
s:replace_solid("doc:1/solid:4", "bbox_cylinder", { max_rel_delta = 0.10 })
s:replace_solid("doc:1/solid:4", "fitted_cylinder", { max_rel_delta = 0.02 })
s:replace_solid("doc:1/solid:4", "fitted_sphere", { max_rel_delta = 0.02 })
s:replace_solid("doc:1/solid:4", "fitted_cone", { max_rel_delta = 0.02 })
s:replace_face_with_fit("doc:1/face:2.3", "cylinder", { max_deviation = 1e-6 })
s:auto_fit_splines("doc:1", { max_deviation = 1e-6 })
s:decompose_planar("doc:1/solid:7")
s:decompose_by_face("doc:1/solid:7", "doc:1/face:7.4")
s:decompose_by_plane("doc:1/solid:7",
                     0.0, 0.0, 0.0,   -- origin x, y, z
                     0.0, 0.0, 1.0)   -- normal x, y, z
s:decompose_by_planes("doc:1/solid:7", {
    { origin = {0, 0, 0}, normal = {1, 0, 0} },
    { origin = {0, 0, 0}, normal = {0, 1, 0} },
})
```

`decompose_by_plane` cuts with an explicit infinite plane (used by safe
BSP for synthetic-plane candidates). `decompose_by_planes` applies a
sequence of cuts in one call; each step operates on all pieces produced
so far.

Proposal-driven workflows. Each `propose_*` call returns a table with
`ok`, `target_id`, `best_candidate_id`, and a `candidates` list; the
matching `apply_*` / `fill_*` call commits one candidate by id. Soft
matches and rejections are reported in the candidate fields.

```lua
-- Removal suggestions (small / thin / sliver solids).
local rem = s:suggest_removals("doc:1")
for _, c in ipairs(rem.candidates) do
    if c.confidence == "high" then
        s:remove_solid(c.target_id)
    end
end

-- Hole filling: propose plug bodies for through-holes in a host.
local holes = s:propose_hole_fills("doc:1", {
    max_radius          = 5.0,
    max_volume_fraction = 0.05,
})
if holes.ok and holes.best_candidate_id then
    s:fill_hole("doc:1", holes.best_candidate_id)
end

-- Surface replacement: fit a primitive to a single problematic face.
local surf = s:propose_surface_replacement("doc:1/face:2.3")
if surf.ok and surf.best_candidate_id then
    s:apply_surface_replacement_candidate("doc:1/face:2.3",
                                          surf.best_candidate_id)
end
```

Volume validation:

```lua
local cmp = s:compare_volumes("doc:1", "doc:2", {
    rel_tol = 1e-6,
    abs_tol = 0.0
})
if not cmp.within_tolerance then
    error("volume changed too much")
end
```

Reports:

```lua
local report = s:report_table()
local json = s:report_json()
local ok, err = s:write_report("report.json")
```

`analyze_table()` and `report_table()` expose these top-level fields directly:

- `report.solids`: analyzed solids with scoped IDs, names, hierarchy paths,
  volumes, bounding boxes, cleanup flags, face IDs, AND
  `replacement_candidates` (bbox / bbox_cylinder / fitted_cylinder /
  fitted_sphere / fitted_cone, each with `available`, `volume`,
  `absolute_delta`, `relative_delta`, `fill_ratio`).
- `report.faces`: analyzed faces with scoped IDs, surface type, area,
  spline/emitter flags, and fit diagnostics.
- `report.decomposed_pieces`: pieces produced by decomposition ops
  (id, source_solid_id, volume, bbox, emit-ready flags).
- `report.diagnostics`: structured diagnostics with severity, code, message,
  scoped shape ID, document ID, and local shape ID.
- `report.assembly`: STEP/XCAF assembly nodes with node ID, parent ID, name,
  label entry, linked shape ID, kind, and path.
- `report.units`: `{ length_unit, length_unit_to_mm }` -- the STEP length
  unit captured at import (or asserted via `units`/`scale` options).
- `report.overlaps`: pairs of interpenetrating solids detected by the
  analyze pass. Each entry has `solid_a_id`, `solid_b_id`, `volume_a`,
  `volume_b`, `intersection_volume`, and `min_fraction` (intersection
  divided by the smaller solid's volume). MCNP cannot handle
  overlapping cells; **both kinds of overlap (pre-existing and
  mutation-introduced) must be resolved before emit**. Resolve each
  pair via decomposition, removal, or by fixing the source CAD.
  Disable detection via `ConversionOptions.check_overlaps = false` on
  very large assemblies (not recommended).

  **Overlaps are error-severity by default.** `report.ok()` becomes
  false when any pair is present, and `s:emit` / the default
  `csgify input.step output.i` convert refuses to write the MCNP
  deck. `preprocess --safe-all` aborts before running auto-fit / BSP
  / removal (those passes can introduce additional overlaps and
  compound the problem). Override with `ConversionOptions.allow_overlaps
  = true` (CLI: `--allow-overlaps`) only for iterative workflows
  where you plan to resolve the overlap in a later step; the
  diagnostic is downgraded to warning and emit proceeds.

  The full pairwise scan runs only on the explicit `s:analyze` /
  `s:analyze_table` entry; **every mutating op also runs a scoped
  scan** that tests only the new geometry (the synthetic body or
  decomposition pieces added by the op) against its neighbors,
  emitting a `mutation_introduced_overlap` diagnostic if
  `--replace-solid-bbox`, `--decompose-*`, or `--fill-hole` puts
  material where another solid already sits. So a Lua workflow can
  tell pre-existing overlap (in `report.overlaps[]` on the first
  analyze) apart from new overlap (the diagnostic surfaces on the
  operation that caused it).
- `report.performance`: per-pass timings + counters.
- `report.json`: full JSON report string for external tools.

`s:replace_solid(...)` with a guard table also returns a `check` field
on the report when a `ReplacementLimits` table was supplied:

```lua
local r, err = s:replace_solid("doc:1/solid:4", "bbox", { max_rel_delta = 0.05 })
if r and r.check and r.check.ok then
    -- volume guard satisfied; r.check.metrics has the numbers
end
```

Emission and STEP export:

```lua
local report, err = s:emit("doc:1", "model.i")
if not report then error(err) end

-- Write the as-simplified geometry as STEP for visual review
-- (same as the CLI --out-step).
local ok = s:export_step("doc:1", "simplified.step")
```

Explaining blockers (CLI: `explain`):

```lua
-- Returns a structured table: kind ("solid" | "piece" | "face" | "unknown"),
-- ready, status, blocking_faces, decomposition, surface_replacement, ...
local info = s:explain("doc:1/solid:7")
if not info.ready then
    for _, blocker in ipairs(info.blocking_faces or {}) do
        print(blocker.id, blocker.surface_type)
    end
end
```

Composed safe-all (CLI: `preprocess --safe-all`):

```lua
local summary = s:preprocess_safe_all("doc:1", {
    apply_removal_candidates = "high",       -- "high" | "medium"
    max_deviation            = 1e-6,          -- omit to leave splines untouched
    max_depth                = 4,
    allow_overlaps           = false,         -- abort on pre-existing
})
if summary.overlaps_aborted then
    error("resolve overlaps before --safe-all proceeds")
end
print("decomposed=" .. summary.decomposed .. ", removed=" .. summary.removed
      .. ", remaining=" .. summary.remaining)
```

Decision packets (CLI: `decision-pack`):

```lua
local result = s:write_decision_pack("doc:1", "decisions/", {
    target_id  = "solid:7",                  -- optional: limit to one target
    max_packets = 10,                        -- 0 = no limit
})
if not result.ok then error(result.error) end
print("packets written: " .. result.packets_written)
```

Compact + replay (CLI: `compact`, `replay`):

```lua
-- Compact an append-only decision log into a deterministic Lua recipe.
local ok, err = csgify.compact("decisions.json", "recipe.lua")
if not ok then error(err) end

-- Replay a JSON recipe against a (possibly revised) STEP file with
-- soft-match support. Returns true / (nil, errmsg).
local rok, rerr = csgify.replay(
    "recipe.json", "revised.step",
    "out.i", "replay_report.json")
if not rok then error(rerr) end
```

## Rules for Agents

- Always analyze before modifying geometry.
- Use scoped IDs from reports, such as `doc:1/solid:4`.
- Do not silently approximate. Every replacement must have an explicit
  tolerance or clear reason.
- Prefer checked solid replacement with `max_rel_delta` or `max_abs_delta`.
- After removing or replacing meaningful volume, compare against the original
  document when possible.
- Keep a decision log when decisions need to survive review or replay:
  `csgify simplify ... --log decisions.json`, then compact with
  `csgify compact decisions.json --out recipe.lua`.
- Emit only after diagnostics are understood.

Directed decomposition is the current manual primitive for non-convex solids:

```sh
csgify simplify model.step --decompose-by-face solid:7 \
    --by-face face:7.4 --out model.i --report report.json --log decisions.json
```

The selected face must belong to the target solid. Plane, cylinder, cone, and
sphere faces are used as bounded cutting surfaces. Torus faces currently split
through a natural plane containing the torus axis, which is useful for reducing
unsupported torus boundaries before further replacement.

Before asking a human to choose a face, an agent should request proposals:

```sh
csgify decompose-suggest model.step solid:7 --report proposals.json
```

or from Lua:

```lua
local p = s:propose_decomposition("doc:1/solid:7")
if p.ok and p.candidates[1].confidence == "high" then
    s:decompose_by_face(p.candidates[1].target_id,
                        p.candidates[1].source_face_id)
else
    -- show p.candidates to the user
end
```

The proposal report is intentionally decision-oriented: each candidate includes
the source face, surface/tool kind, piece count, volume delta, convex/CSG
status of the produced pieces, score, confidence, and rejection reason. A human
should only be asked when no candidate is high confidence or when several
candidates are equivalent from the current metrics.

When obvious decomposition is acceptable, use the safe BSP command instead of
asking the agent to choose:

```sh
csgify simplify model.step --auto-decompose-bsp solid:7 \
    --max-depth 4 --out model.i --report report.json \
    --proposal-report proposals.json --log decisions.json
```

or from Lua:

```lua
local report, err = s:auto_decompose_bsp("doc:1/solid:7", { max_depth = 4 })
if not report then error(err) end
```

Safe BSP only applies high-confidence candidates. If it reaches a target where
no high-confidence candidate is available, it stops with
`auto_decompose_bsp_needs_decision`, leaving the remaining proposal step to the
agent or user. Generated pieces can be targeted as `piece:N` with faces like
`piece:N/face:M` inside the same session.

When a user chooses a proposal, prefer applying the candidate rather than
copying the face ID by hand:

```sh
csgify simplify model.step --apply-decomposition-candidate solid:7 \
    --out model.i --candidate candidate:3 \
    --report report.json --proposal-report proposals.json
```

In Lua:

```lua
local p = s:propose_decomposition("doc:1/solid:7")
local report, err = s:apply_decomposition_candidate("doc:1/solid:7",
                                                    p.best_candidate_id)
if not report then error(err) end
```

Candidate IDs are local to the current proposal. The decision log records the
resolved target, face ID, score, and confidence so replay does not depend on
the transient candidate number.

If no face-driven candidate works, proposals may include bbox-axis fallback
planes:

```json
{
  "reason": "bbox_axis_fallback",
  "tool_kind": "bbox_plane_x",
  "confidence": "medium"
}
```

These are review candidates, not automatic safe-BSP choices. The agent should
show them to the user unless there is an explicit policy allowing fallback
splits.

The safe BSP policy defaults are conservative:

- auto-apply high-confidence face-driven splits
- stop on bbox fallback candidates
- stop on medium-confidence candidates

The CLI can opt into broader behavior with:

```sh
--allow-bbox-fallback-auto
--allow-medium-confidence-auto
```

Agents should treat these as explicit user/project policy choices, not routine
defaults.

## Missing Pieces

Still missing:

- general-purpose synthetic body creation from Lua. Hole-fill already
  creates `synthetic:N` bodies via `s:fill_hole(...)`, but there is no
  generic Lua primitive to add an arbitrary cylinder/sphere/etc.
- boolean synthetic combinations from Lua. The engine and STEP exporter now
  materialize existing Boolean nodes recursively and exactly, but Lua cannot
  create arbitrary Boolean trees yet.
- material assignment workflow. Synthetic bodies inherit
  `default_material_id`; decision-pack UI asks about host material vs.
  void, but there is no Lua-level material assignment surface.
- stronger assembly-instance matching for complex revised STEP files.

Recently landed (no longer missing):

- preview generation for semantic cleanup — decision-pack packets emit
  `preview.html`, and the viewer renders hole-fill previews. See
  `src/decision_pack.cpp` and `src/viewer.cpp`.

Note: hole-fill (`s:propose_hole_fills`, `s:fill_hole`), surface
replacement proposals (`s:propose_surface_replacement`,
`s:apply_surface_replacement_candidate`), removal suggestions
(`s:suggest_removals`), and decision-pack export are recent additions
not yet covered in the "Current Lua API" section above.

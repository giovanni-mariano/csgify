# Decision Packet Implementation Plan

This plan describes how to implement the Alea boundary and human-feedback
model. The goal is to give agents structured data and give humans visual
evidence when a geometry decision is not obvious.

The main rule is:

```text
csgify prepares and explains CAD geometry.
Alea owns CSG identity, cells, voids, simplification, and export.
Humans see pictures when judgment is required.
Agents get structured data plus links to those pictures.
```

This must stay in the antirez style: small moving parts, plain data, command
line first, explicit failure, useful logs, no large framework, and no hidden
magic.

## Scope

Implement decision packets for:

- decomposition choices
- removal of useless components
- solid replacement with simplified bodies
- spline or unsupported-surface replacement
- torus/blend treatment when the automatic choice is not high confidence

Do not implement:

- a GUI application
- a CAD editor
- a second permanent surface registry
- void generation in `csgify`
- a second CSG boolean engine

## Architecture

The implementation should add three small layers:

```text
Proposal layer
  produces facts, candidate actions, and confidence

Decision packet layer
  writes manifest.json, decision.md, preview.html, and images

Renderer layer
  draws OCCT shapes and simple overlays
```

The proposal layer must not know about HTML or image encoding. The renderer
must not decide geometry. The packet writer only assembles artifacts.

## Data Model

Add plain structs in a small leaf module, for example `src/decision_packet.hpp`
and `src/decision_packet.cpp`.

Suggested structs:

```cpp
struct DecisionImage {
    std::string role;        // overview, target, overlay, after, difference
    std::string path;        // relative to packet directory
    std::string description;
};

struct DecisionMetric {
    std::string name;
    double value;
    std::string unit;
};

struct DecisionCandidateView {
    std::string id;
    std::string action;
    std::string confidence;
    std::string reason;
    std::vector<DecisionMetric> metrics;
    std::vector<DecisionImage> images;
};

struct DecisionRequest {
    std::string id;
    std::string kind;        // removal, replacement, decomposition, spline
    std::string target_id;
    std::string reason;
    std::string recommended_candidate_id;
    std::vector<DecisionMetric> metrics;
    std::vector<DecisionImage> images;
    std::vector<DecisionCandidateView> candidates;
};
```

Keep this model independent from OCCT and Alea. It should be serializable to
JSON and table-like APIs without carrying heavy geometry handles.

Geometry needed for rendering should live in a separate view model:

```cpp
struct ViewShape {
    std::string id;
    TopoDS_Shape shape;
    Color color;
    double opacity;
    bool draw_edges;
};

struct ViewPlane {
    gp_Pln plane;
    Bnd_Box bounds;
    Color color;
    double opacity;
};

struct ViewRequest {
    std::string title;
    std::vector<ViewShape> shapes;
    std::vector<ViewPlane> planes;
    ViewCamera camera;
};
```

This split matters: decision data is durable; view data is a temporary drawing
request.

## Phase 1: Stable Decision Requests

First, teach the engine to name unresolved choices explicitly.

Tasks:

1. Add `DecisionRequest` and JSON serialization.
2. Add `decision_requests` to the report model.
3. Populate requests from existing proposal producers:
   - decomposition proposal has no high-confidence candidate
   - several decomposition candidates have similar medium confidence
   - removal candidate is medium confidence
   - replacement volume delta exceeds default tolerance
   - spline fit is possible but approximate
4. Keep existing diagnostics. A diagnostic says what is wrong; a decision
   request says what the user can choose.
5. Sort requests by stable target ID so output is deterministic.

Acceptance:

- `csgify inspect model.step --report report.json` can report unresolved
  decisions without writing images.
- Reports remain useful in a terminal.
- Existing tests keep passing.

## Phase 2: Minimal Headless Renderer

The first renderer should be simple and reliable. Do not start with Qt, VTK, or
an OCCT interactive viewer.

Preferred first implementation:

```text
OCCT shape -> BRepMesh_IncrementalMesh -> triangles -> orthographic projection
-> SVG
```

Why SVG first:

- no new dependency
- works headlessly
- easy to inspect in a browser
- easy to embed in `preview.html`
- easy to draw overlays, labels, edges, and split planes
- deterministic enough for tests

The renderer only needs to be good enough for decisions. It is not a CAD
viewport.

Views to support first:

- isometric
- front
- right
- top
- automatic fit-to-bounds

Drawing features:

- colored filled triangles
- transparent target overlays
- darker silhouette/edge lines when possible
- highlighted faces in a different color
- split planes as translucent rectangles
- replacement bodies over original bodies
- simple scale axis marker

Avoid text-heavy images. Put metrics in `decision.md` and `manifest.json`.

Acceptance:

- A box/cylinder/sphere fixture can generate an SVG.
- A highlighted target is visually distinct.
- A split plane overlay is visible.
- Renderer works without an X server.

## Phase 3: Decision Packet Writer

Add a packet writer that creates:

```text
decision-solid-7/
  manifest.json
  decision.md
  overview.svg
  target.svg
  candidate-001.json
  candidate-001-overlay.svg
  candidate-001-after.svg
  preview.html
```

`manifest.json` is for agents. `decision.md` is for humans. `preview.html` is
just static HTML that links or embeds the images.

The packet writer should take:

- one `DecisionRequest`
- one or more `ViewRequest` objects
- the output directory
- limits such as max candidates and max image size

It should not import STEP by itself and should not mutate geometry.

Acceptance:

- The packet directory is deterministic.
- Missing optional images do not break the manifest.
- The generated HTML opens locally in a browser.
- The manifest uses relative paths only.

## Phase 4: CLI

Add a command:

```sh
csgify decision-pack model.step \
    --target solid:7 \
    --kind decomposition \
    --out decisions/solid-7
```

Add a report-driven form:

```sh
csgify decision-pack model.step \
    --from-report report.json \
    --out decisions/
```

Useful options:

```text
--max-packets N
--max-candidates-per-packet N
--image-format svg
--image-size 1200x900
--view iso|front|right|top
--jobs N
```

Keep the CLI boring. Avoid an interactive prompt. The human can inspect the
packet, then the agent or user writes a recipe.

Acceptance:

- The command can generate a packet for one decomposition proposal.
- The command can generate packets for all unresolved requests in a report.
- It refuses if the report does not match the input model enough to trust it.

## Phase 5: Decomposition Packets First

Start with decomposition because the current engine already has candidate data.

For a decomposition decision, each candidate should show:

- target solid isolated
- source face or split plane highlighted
- before/after pieces
- piece count
- volume delta
- exact CSG status for each produced piece
- confidence and rejection reason

The view generator can reuse the candidate geometry already produced during
scoring when available. If candidate scoring currently discards that geometry,
store just enough temporary shape data in the decision-pack path. Do not put
heavy shapes into JSON.

Acceptance:

- A medium-confidence split produces a packet.
- The packet shows why the candidate is not automatic.
- Applying the candidate still uses the existing explicit operation path.

## Phase 6: Removal Packets

Removal decisions are semantic, so they need context.

For each removal candidate, show:

- full or local assembly context with candidate highlighted
- isolated candidate
- nearest neighboring solids if cheap to compute
- name, hierarchy path, volume fraction, bbox fraction
- reason flags: hardware name, repeated group, small volume, protected name,
  unknown semantics

Do not automatically remove medium-confidence objects. If a candidate is high
confidence under a user-approved policy, no packet is required.

Acceptance:

- `remove-suggest` can point to a packet directory.
- The packet makes it visually obvious what would be removed.
- Batch removal can still run without images.

## Phase 7: Replacement and Spline Packets

Replacement packets should compare original and simplified geometry.

For solid replacement:

- original body
- replacement body overlay
- volume delta
- bbox delta if useful
- fitted primitive parameters
- accepted tolerance

For spline/unsupported surface replacement:

- target face
- fitted analytic surface
- max deviation
- RMS deviation if available
- effect on parent solid volume if known

Acceptance:

- A fitted cylinder/sphere/cone candidate can be visualized.
- A rejected candidate explains whether rejection was due to volume, deviation,
  topology, or unsupported primitive.

## Phase 8: Session and Lua API

Expose this without forcing agents to parse CLI output.

Lua shape:

```lua
local requests = s:decision_requests("doc:1")
s:write_decision_pack("doc:1", requests[1].id, "decisions/solid-7", {
    max_candidates = 3,
    image_format = "svg"
})
```

Session C++ API shape:

```cpp
std::vector<DecisionRequest> decision_requests(DocumentId doc);
CommandResult write_decision_pack(DocumentId doc,
                                  const std::string& decision_id,
                                  const DecisionPackOptions& options);
```

C ABI can come after the C++ and Lua API settle. Keep the ABI flat:

- count requests
- get request JSON
- write packet by request ID

Acceptance:

- A Pi/Claude/Codex-style agent can run Lua, receive request tables, generate a
  packet, show image paths to the user, and write a replayable recipe.

## Phase 9: Alea Emission Boundary

While implementing packets, keep the Alea boundary clean.

Emission should do this:

1. Convert validated CAD pieces into Alea primitive calls.
2. Use Alea surface creation APIs, such as plane, sphere, RCC, TRC, box, or
   axis-aligned primitives where appropriate.
3. Use `alea_halfspace()` to get signed halfspace nodes.
4. Build cell roots with `alea_intersection()` and `alea_union()`.
5. Add cells and materials through Alea.
6. Let Alea deduplicate, simplify, generate voids, and export.

`csgify` may keep this temporary map only during emission:

```text
CAD face id -> Alea surface index -> sense
```

Do not build another global registry in `csgify`.

Acceptance:

- Emission reports can explain which CAD faces became which Alea surfaces.
- Surface identity remains owned by Alea.

## OpenMP and Parallelism

OpenMP can help, but only if used in a bounded and boring way. It should not be
sprinkled through the codebase.

Add a build option:

```cmake
option(CSGIFY_ENABLE_OPENMP "Enable OpenMP acceleration" OFF)
if(CSGIFY_ENABLE_OPENMP)
    find_package(OpenMP REQUIRED)
    target_link_libraries(csgify_engine PRIVATE OpenMP::OpenMP_CXX)
    target_compile_definitions(csgify_engine PRIVATE CSGIFY_ENABLE_OPENMP=1)
endif()
```

Add one tiny wrapper, for example `src/parallel.hpp`:

```cpp
template <typename Fn>
void parallel_for(std::size_t count, int jobs, Fn fn);
```

Rules:

- If OpenMP is disabled, `parallel_for` is serial.
- If `jobs <= 1`, `parallel_for` is serial.
- Do not mutate `CadModel`, `Session`, Alea systems, or shared vectors inside
  parallel loops.
- Preallocate output arrays and write by index.
- Sort final results by stable ID after parallel work.
- Keep default jobs conservative.
- Avoid nested parallelism.
- Measure before making a loop parallel.

Good OpenMP targets:

- rendering independent decision packets
- rendering independent images inside one packet
- per-solid read-only analysis after import, if OCCT access proves safe
- per-face fit metric computation after face samples are collected
- candidate metric evaluation when each candidate uses isolated local data
- volume/bbox/statistics over independent solids

Bad OpenMP targets:

- STEP import
- shape healing
- session mutation
- decision log writing
- report JSON writing
- Alea system mutation
- repeated OCCT boolean splitting on the same model
- anything that pushes into a shared vector without fixed indexing

OCCT-specific rule:

```text
Prefer OCCT's own parallel option for one heavy boolean operation when
available. Prefer csgify OpenMP for many independent lightweight tasks.
Do not do both at the same time without a benchmark.
```

This matters because large STEP files are often memory-bound. Parallel booleans
can easily consume all RAM. The CLI should have a visible budget:

```sh
csgify decision-pack model.step --from-report report.json --out decisions --jobs 4
csgify preprocess model.step --max-targets 20 --max-splits 20 --jobs 2
```

For a laptop, default behavior should favor not exhausting memory. `--jobs 1`
must always be valid and deterministic.

## Performance Plan

Add performance counters before adding parallelism:

- decision request build time
- candidate generation time
- render mesh time
- image write time
- packet write time
- peak candidate count
- packet count
- images written

Then benchmark:

- small fixtures in normal tests
- `rails.stp` as a medium local benchmark
- `SHIELDING_ASSY_5638_assembly.stp` as an opt-in large benchmark, not a
  default test

Performance improvements should come in this order:

1. Avoid repeated full analysis after batch operations.
2. Cache imported documents inside the session.
3. Cap packet and candidate generation.
4. Reuse tessellation for multiple views.
5. Add OpenMP to image generation.
6. Add OpenMP to read-only analysis only after thread-safety checks.
7. Consider OCCT boolean parallel options only with strict job and memory
   limits.

## Testing

Add tests in small increments:

- JSON serialization for a hand-built `DecisionRequest`.
- SVG renderer smoke test for a box fixture.
- Decomposition packet smoke test.
- Removal packet smoke test.
- HTML preview contains expected image paths.
- Lua API can write one packet.
- OpenMP disabled build keeps working.
- OpenMP enabled build, if available, produces the same request IDs and same
  candidate ordering.

Do not make large STEP files part of default `ctest`. Add an explicit script:

```sh
tests/large_benchmark.sh tests/fixtures/SHIELDING_ASSY_5638_assembly.stp
```

## Milestones

Milestone 1: decision request model

- report exposes unresolved decisions
- no images yet
- tests for JSON/table shape

Milestone 2: SVG renderer

- isolated target view
- overlay view
- no GUI dependency

Milestone 3: decomposition decision packets

- one CLI command
- manifest, markdown, SVG, HTML
- bounded candidate count

Milestone 4: removal decision packets

- context images
- semantic metrics
- agent-readable manifest

Milestone 5: replacement and spline packets

- original/replacement overlays
- deviation and volume metrics

Milestone 6: Lua/session integration

- agents can request packets from scripts
- recipes remain the replay mechanism

Milestone 7: optional OpenMP

- build option
- `--jobs`
- image generation parallelized first
- deterministic output order

## Antirez Checks

Before merging each milestone, ask:

- Can one person read the code path in one sitting?
- Does the CLI still work without a service or GUI?
- Does every automatic action have a visible reason?
- Does every non-obvious choice produce useful evidence?
- Can the user replay the decision later?
- Can the feature run serially with `--jobs 1`?
- Did we avoid building something Alea already owns?

If the answer is no, simplify the design before adding more machinery.

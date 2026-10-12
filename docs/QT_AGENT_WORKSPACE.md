# Qt Agent Workspace

## Goal

The GUI should become a CAD conversion workspace where the user, the agent,
and the engine share one visible state. The agent should not be a chatbot
bolted onto the viewer, and it should not mutate CAD state secretly. It should
produce structured operation proposals that the Qt app can preview, validate,
queue, approve, execute, and audit.

The core loop is:

```text
user intent -> context bundle -> agent proposal -> action queue
    -> user/policy approval -> Session mutation -> report refresh
```

`csgify::Session` remains the source of truth. The Qt app orchestrates the
workflow. The agent proposes; the session mutates.

## Implemented Decision Viewer

The project workspace now includes the first embedded advisory conversation
slice. It builds an immutable, revision-bound engine context on the geometry
worker, answers through either a clearly labelled simulated provider or an
asynchronous OpenAI Responses provider, validates structured output and
evidence references, supports cancellation, and persists the conversation in
the project database. Live setup is documented in
[AGENT_WORKFLOW.md](AGENT_WORKFLOW.md#embedded-project-conversation). This
slice is question/answer only: it does not turn text into geometry mutations.

The first milestone is available directly through the existing CLI binary.
Qt 6 Widgets is preferred automatically when it was found at configure time;
the original X11 frontend remains available as a fallback:

```sh
./build/csgify viewer-check
./build/csgify view model.step --select solid:1 --candidate best \
  --log decisions.json --out reviewed.i
./build/csgify view model.step --viewer-backend x11
```

The Qt window embeds OCCT in a native widget and provides an assembly/status
tree, selectable 3D geometry, proposal overlays, a decomposition-candidate
table, measured decision evidence, Accept/Reject/Undo, isolate/show-all, and a
decision-history dock. Accepting an approximate or lower-confidence proposal
requires an explicit confirmation; the measured warning is advisory and the
human makes the final policy decision. All accepted changes and undo operations
go through `csgify::Session`, so `--log` remains replayable.

The decision details and autonomy queue summary also show the immutable
approximation limit, cumulative ledger use, added/removed volume, overlap and
unknown counts, modeled-domain coverage status, and the MCNP blocking code.
These are evidence displays; the viewer does not allow an agent proposal to
edit the human-supplied budget.

All eight advertised autonomy operations now have a Qt review route.
Decomposition, surface replacement, removal, and contained-medium changes use
the geometry decision window. Fixed-point resolution, budgeted shatter,
residual removal, and overlap ownership use a transactional review that shows
the target, owner or depth, cumulative ledger use, and hard project limits.
Approval authorizes a certified attempt: the Session still measures the result
and rolls it back when budget or overlap gates fail. When authority is absent,
the same button is labelled **View recovery steps…** and identifies the exact
project-policy change or missing owner; it never offers an override.

The project policy editor exposes those approximation authorities together
with topology and containment preservation. Saving policy still unloads the
active geometry, so the queue must be evaluated again against the new policy.

The geometry workspace uses a searchable assembly tree, a central viewport
with adjacent navigation and comparison controls, and a scrolling inspector
with Alternatives/Evidence tabs. Search filters hierarchy rows while retaining
the selected shape and its actions. Status remains visible beside long names;
the header shows the current revision and decomposition count without claiming
export readiness. Decision history starts collapsed and is opened from the
header; active jobs open the existing progress dock. The visual and interaction
specification is in [GEOMETRY_WORKSPACE_DESIGN.md](GEOMETRY_WORKSPACE_DESIGN.md).

Decomposition review also provides `Before`, `Preview`, and `Applied` display
modes. `Preview` regenerates only the selected candidate against a shallow
model copy and retains no geometry for other candidates. The parent remains
visible beneath the proposed children so coverage or outside-parent failures
can be inspected directly. Proposed exact pieces are bright green, residual
pieces orange, and slivers magenta; support planes are translucent blue.

The preview runs the same exact local partition and neighbour-overlap
certificates used by the coordinator. The evidence panel reports child
validity, containment, coverage, sibling-disjointness, relative volume delta,
outside/uncovered/overlap volumes, OCCT proof calls and failure code, neighbour
candidate/test counts, inherited overlaps, conflicting body and intersection
volume. It then recommends an alternate batch, targeted shatter, approximation
review, overlap ownership, or source-CAD repair. This is a non-mutating
certificate: the live session revision remains unchanged.

Build controls:

```sh
cmake -S . -B build -DCSGIFY_ENABLE_QT_VIEWER=ON
cmake --build build
```

When Qt 6 Widgets is absent, configuration succeeds and `view` falls back to
the X11 frontend. `--viewer-backend qt|x11|auto` makes the choice explicit.
On Linux, the Qt frontend selects the `xcb` platform when X11/XWayland is
available because OCCT TKOpenGL embeds an X11 native window.

The `csgify_qt_decision_widget` regression constructs the production decision
window offscreen and drives its actual widgets without initializing the native
OCCT surface. It covers unresolved selection guidance, Grid/Axes toggles,
candidate generation, guarded acceptance, asynchronous preview cancellation,
close blocking while work is active, and safe closing afterward:

```sh
ctest --test-dir build -R csgify_qt_decision_widget --output-on-failure
```

## Product Shape

The longer-term workspace can grow from the implemented decision viewer into a
dedicated application beside the existing CLI:

```text
csgify      CLI/headless workflow
csgify-gui  Qt human-agent workspace
```

The GUI should use Qt Widgets with an embedded OCCT viewer first. QtQuick can
come later if needed, but the first application needs dockable panels, trees,
forms, logs, and a native OpenGL/OCCT viewport more than it needs animated UI.

Suggested layout:

```text
+---------------------------------------------------------------+
| Toolbar: Analyze | Propose | Apply | Emit | Export STEP       |
+----------------------+----------------------------+-----------+
| Assembly Tree        | 3D OCCT Viewer             | Inspector |
| - status badges      | - selection                | - target  |
| - names/hierarchy    | - overlays                 | - metrics |
| - removed/synthetic  | - before/after preview     | - actions |
+----------------------+----------------------------+-----------+
| Agent Console + Action Queue + Transcript                    |
+---------------------------------------------------------------+
```

The user should be able to work without typing for common tasks, but should
also have a natural-language console for intent and refinement.

## Agent Console

The console is not only a chat transcript. It has three parts:

1. Prompt box: user writes intent.
2. Structured action queue: agent output becomes proposed operations.
3. Transcript: explanations, user refinements, command results.

Example prompt:

```text
Make this model emit-ready. Ask before removing anything above 0.5% volume.
```

Example action queue:

```text
[Approve] auto_fit_splines(doc:1, max_deviation=1e-6)
          resolves: unsupported_spline_face
          confidence: high

[Review ] apply_decomposition_candidate(doc:1/solid:16, candidate:2)
          resolves: nonconvex_solid_requires_decomposition
          confidence: high

[Review ] remove_solid(doc:1/solid:42)
          reason: tiny hardware
          volume_fraction: 0.00003
```

The user can reference GUI context naturally:

```text
Fix the selected solid.
Remove tiny bolts, but leave anything named shield or detector.
Why can't this emit?
Apply all high-confidence actions.
Show me what changed since the last export.
```

The console should also expose quick commands:

```text
[Explain selected] [Make emit-ready] [Propose removals]
[Auto-fit splines] [Decompose selected] [Check overlaps]
[Emit MCNP] [Export STEP]
```

## Context Bundle

The agent should receive a curated context bundle every turn. Do not dump the
whole model or rely on hidden viewer state. The Qt app should explicitly build
the context from session/report/view state.

Minimum context:

```text
Document:
  source path
  units
  total solids
  emit-ready summary

Current UI:
  selected shape id
  selected face/solid diagnostics
  active mode
  pending queue item, if any

Report:
  blocking diagnostics
  overlaps
  spline faces
  non-convex solids
  decision requests
  candidate proposals

History:
  decision log summary
  operations already applied
  pending action queue

Policy:
  max volume delta
  max spline deviation
  allow removals
  allow overlaps
  auto-apply threshold
```

Example machine context:

```json
{
  "selected": "doc:1/solid:16",
  "task": "make selected solid emit-ready",
  "blockers": [
    {
      "code": "nonconvex_solid_requires_decomposition",
      "target": "doc:1/solid:16"
    }
  ],
  "available_tools": [
    "analyze",
    "propose_decomposition",
    "apply_decomposition_candidate",
    "emit",
    "export_step"
  ],
  "policy": {
    "allow_overlaps": false,
    "allow_removal_without_approval": false,
    "max_spline_deviation": 1e-6
  }
}
```

## Agent Output

The agent should return structured proposals, not just prose.

```json
{
  "message": "solid:16 needs decomposition. candidate:2 is high confidence.",
  "actions": [
    {
      "op": "apply_decomposition_candidate",
      "target": "doc:1/solid:16",
      "params": {
        "candidate": "candidate:2"
      },
      "confidence": "high",
      "reason": "nonconvex_solid_requires_decomposition",
      "requires_user_approval": true
    }
  ]
}
```

The GUI renders this into the action queue and shows evidence:

- target highlighted in the viewer
- diagnostics resolved
- candidate preview or overlay
- volume delta
- overlap result
- confidence and reason codes
- exact recipe/decision-log operation

## Modes

Support three agent modes.

### Explain Mode

No mutations and no queued operations. The agent only explains:

- why a target is not emit-ready
- what diagnostics mean
- what operation classes may resolve blockers
- what risks remain

### Propose Mode

The agent creates queue items. Nothing mutates until the user approves or the
active policy auto-approves the item.

### Auto Mode

The agent may apply operations that match policy:

- high-confidence spline fits
- safe decomposition
- removals under explicit volume/name thresholds
- no overlap override unless explicitly allowed

Auto mode still routes through the same queue and session machinery. It just
allows some queue items to pass approval automatically.

## Boundaries

Hard rule:

```text
Agent proposes.
Qt validates.
User or policy approves.
Session mutates.
Report verifies.
Decision log records.
```

Avoid:

```text
Agent secretly mutates CAD state.
Viewer owns geometry state.
GUI has a parallel model of decisions.
Agent controls mouse/keyboard instead of session tools.
```

The Qt app may run the agent in a worker thread or a separate process, but all
mutations must go through the same session operations used by CLI, Lua, replay,
and C API.

## Suggested Components

```text
Qt GUI
  MainWindow
  OcctViewerWidget
  AssemblyTreeModel
  DiagnosticsPanel
  AgentConsole
  ActionQueuePanel
  OperationInspector

Agent Layer
  AgentController
  ContextBuilder
  PolicyBuilder
  OperationParser
  ToolDispatcher

Engine Layer
  csgify::Session
  analyze / propose / apply / emit / export
  decision log / recipe
```

## First Milestone (implemented)

The implemented GUI milestone covers:

1. Qt main window with embedded OCCT viewer.
2. Load STEP into `csgify::Session`.
3. Run analyze and populate assembly tree / diagnostics.
4. Select shape in tree or viewer.
5. Show target diagnostics and candidate actions.
6. Apply one approved action through the session.
7. Refresh report and viewer.
8. Export MCNP on commit. STEP export remains available through the existing
   CLI session workflow.

Then add the agent as a producer of queue items. This keeps the workflow
testable before introducing language-model variability.

## Fixed-point progress and residual review

The Qt 6 decision window can run the bounded fixed-point coordinator on a
worker thread. It displays accepted actions, residual count/volume, active
pieces, current target, and elapsed time. Cancel requests are checked between
OCCT operations, preserve the last certified state, and do not close the
viewer.

Worker progress is milestone-preserving and rate-limited before it enters the
Qt event queue. The progress model still retains terminal, phase, target,
accepted-action, diagnostic, failure, and cancellation events, while viewport
highlight changes are coalesced to at most one OCCT presentation update per
100 ms. The review artifact log appends each delivered JSONL event once rather
than rewriting its complete history for every update. These bounds keep input,
window management, and Cancel responsive during candidate-heavy searches.

If the run stops partial, unresolved bodies remain orange. Select one and use
**Review selected residual** to generate a fresh proposal in the background;
the ordinary before/preview/applied views and safety evidence then govern the
explicit human override. Certified automatic actions are reported in history
and never open authorization dialogs.

## Component-removal review

`view --review-removals` uses a component-aware plan in the Qt frontend.
Removal candidates are grouped by stable assembly occurrence (with ungrouped
solids identified explicitly), and the table shows exact target count,
confidence, combined volume, reasons, and blockers. Protected candidates
cannot be selected. **Select eligible high-confidence** selects only
unblocked high-confidence candidates; manual candidates remain an explicit
human choice.

The removal action includes the selected component and exact-solid counts in
its label and confirmation against the current document revision, then runs
the complete plan asynchronously as one
certified batch and one undo step. Proposed targets are red and the active
certification target is yellow. Cancellation or failure retains the pre-job
revision; a successful terminal certificate rebuilds the scene once at the
new committed revision. One-item removal reviews opened from the autonomy
queue use the same worker path rather than synchronously mutating the UI
thread.

When that one-item review is opened from the project workspace, Qt creates and
claims a dedicated `qt_batch_remove_v1` project job. Its first durable event is
linked to the confirmed component/solid plan; subsequent engine events are
normalized under the project job ID. Project cancellation requests propagate
to the Qt worker at the next safe boundary. On termination, the plan, event
log, diagnostics, summary, and available milestone images are published into
the content-addressed project artifact store. A committed terminal event and
the primary summary artifact complete the job atomically, and the workspace
refreshes its Jobs and Artifacts tabs when the review closes. If artifact or
SQLite publication fails after certification, Qt writes a compensating undo
before restoring the live geometry. If that undo cannot be journaled, all
further mutation controls are locked and the workspace remains open with a
**Recovery required** state until the operator retries recovery.

## Phase 8 coverage review

The Phase 9 workspace can consume the versioned coverage certificate directly;
the reviewer does not need to inspect JSON:

```sh
csgify coverage status coverage.json
csgify coverage review coverage.json
```

`coverage review` validates the certificate digest before opening Qt. The
panel shows the modeled domain, material/void/graveyard inventory, exact query
counts, worker and scratch-memory facts, hierarchy reuse, and explicit evidence
for overlaps, gaps, undefined fills, unresolved owners, and truncation. Findings
are searchable and filterable as blocking or informational.

Unsafe ownership findings are never represented as approval buttons. The panel
instead states the corrective operation that a human or authorized LLM must
perform before regenerating the certificate. This prevents a UI acknowledgement
from weakening MCNP's one-component-per-space invariant. Numeric shortcuts are
not bound in this panel and closing it performs no mutation.

Blocking findings can also enter the ordinary durable autonomy queue:

```sh
csgify coverage queue coverage.json \
  --source final.step --out coverage-queue.json

csgify autonomy final.step \
  --resume coverage-queue.json \
  --queue-report coverage-queue.json \
  --review
```

The queue retains the certificate path and digest. Selecting a coverage item
opens that validated certificate in the coverage panel. Automatic mode cannot
execute or dismiss these items. After corrective geometry work, reconcile the
same queue against the replacement certificate:

```sh
csgify coverage queue replacement-coverage.json \
  --resume coverage-queue.json --out coverage-queue.json
```

## Unified project workspace

Large projects can be inspected without parsing report JSON or loading the
source STEP:

```sh
csgify project review

# Or open a known project directly:
csgify project init project-directory
csgify project status project-directory
csgify project review project-directory \
  --source final.step --coverage coverage.json --queue coverage-queue.json
```

Running `project review` without a directory opens the project launcher. New
Project initializes the selected folder and its conservative default policy;
Open Project validates an existing project before entering the workspace. In
the Overview tab, choose a source STEP, inspect the effective project policy,
edit its supported preparation/authority settings, load/analyze it, then
explicitly export reviewed STEP or gated MCNP output. Saving policy changes
unloads geometry evaluated under the previous policy and requires fresh
evidence. Advanced contained-medium and protection settings are retained by
the editor even though they are not exposed as routine controls.

After accepted geometry edits have been journaled, reopen the project and use
**Restore saved decisions**. The workspace compacts the log, reconstructs the
session with the shared typed replay dispatcher on a worker, checks the exact
source digest, healing/unit preparation, csgify build version, and OCCT version,
then regenerates analysis before enabling review or export. Historical export
commands are not rerun during restoration. Older logs without explicit
preparation binding or build identity are reported as incompatible and must be
reimported; they are never silently replayed under the current project policy.
The compact recipe also restores the latest rejection for each exact
target/proposal identity. Matching removal alternatives remain visible for
audit, are labelled **Previously rejected**, and are not selected
automatically; selecting one is an explicit reconsideration.

Opening a supported geometry proposal writes a pending-review checkpoint with
its typed action, candidate identity, rationale, and base revision. After
project reconstruction, **Resume pending review** regenerates the current
decomposition, surface-replacement, hole-fill, removal, or contained-medium
evidence before opening the decision window. Contained-medium checkpoints bind
to the staged typed request ID and produce a fresh geometry certificate. A
missing regenerated candidate or staged request remains pending and directs the
operator to regenerate options or restage the request; saved evidence is never
treated as a fresh preview. Legacy contained-medium checkpoints without a
request ID cannot be resumed automatically.

Initialization is explicit. `project status` and `project events` open SQLite
read-only and never create a missing project, artifact directory, journal, or
schema migration as a side effect.

The Qt workspace queries the project SQLite index in bounded pages. It exposes
component definitions and occurrences separately so repeated ITER components
stay compact, plus job and immutable-artifact views. Search executes against
the complete SQLite index rather than only rows already visible in the table.
The spatial-focus tab pages through R-tree entities by namespace and kind and
can filter by entity or owner ID.

The geometry review uses an opaque overview by default; whole-model
transparency is deliberately avoided because it makes rear faces and internal
boundaries unreadable on dense assemblies. Selecting a body switches to local
context: the selected body is opaque cyan, only bodies intersecting a
10%-expanded selection bounding box remain as gray depth-peeled ghosts, and
distant geometry is hidden. Certified overlap participants are red. An
adaptive XY reference floor below the model and a color-coded corner XYZ
trihedron provide the familiar spatial references used by robotics and CAD
tools. The floor uses background-depth rendering so it cannot cross visible
solids; **Grid** and **Axes** toggle the aids independently. **Isolate** hides
even the local context,
while **Show all** restores the opaque overview.
These are presentation modes only and never mutate the CAD model.

Inspection is also the corrective starting point, not a read-only dead end.
Selecting an unresolved solid or piece fills the inspector with its stable ID,
status, and recommended next action, then enables **Generate decomposition
options**. The inspector shows the document-wide unresolved count and offers
**Resolve all certifiable shapes** for repetitive assemblies. That bulk action
uses the same bounded fixed-point worker, progress, cancellation, transaction,
and verification path as CLI decomposition. It does not apply uncertain
candidates: those remain residuals that the operator or LLM must review
explicitly using Before/Preview evidence.

Geometry is demand-driven: opening the workspace performs no OCCT import;
**Load geometry review** and **Open decision queue** are the operations that
load the source STEP. Opening a selected assembly occurrence or spatial entity
uses focused LOD. The session owns the imported CAD, but the AIS viewer creates
presentations only for the selected solid/piece at first. **Show all** is the
explicit promotion to full-model rendering. This controls the expensive GPU
and interactive-object side without pretending that STEP import itself is
random-access.

Operator mutations are explicit and audited by their durable state. A failed
job can be granted exactly one further attempt, running jobs with heartbeats
older than ten minutes can be recovered after confirmation, and a decision log
can be compacted into a replayable recipe with optional intermediate STEP
exports. Coverage ownership blockers remain corrective findings, not approval
buttons.

Claimed conversion workers use `ProjectJobProgressSink` as the single bridge
from engine progress to project state. It replaces engine-local job IDs and
sequences with the durable project identity, continues sequence numbers after
a retry, throttles heartbeat writes, polls the database cancellation flag at
safe event boundaries, and rejects decreasing or post-terminal progress. Final
artifact publication and the committed terminal event are one SQLite
transaction, so a racing cancellation cannot leave contradictory audit state.
The Alea void-generation worker also maintains a separate five-second
heartbeat while its solver call is opaque and discards the result if a project
cancellation arrived during that call.

The CI-only `project review --smoke-test` option constructs the launcher and
checks its New/Open actions. `project review DIR --smoke-test` constructs the
workspace, checks its project-lifecycle controls, validates attached coverage
and autonomy documents through the typed readers, enters the Qt event loop,
and closes immediately. Neither path loads geometry or approves a decision.
The Phase 9 qualification combines the workspace check with a real
blocked-overlap certificate, the immutable coverage queue policy, evidence
reconciliation, an autonomous certified geometry action, final STEP/MCNP
outputs, and compact recipe generation.

Only disappearance of the finding from a valid replacement certificate marks
it `verified resolved`; rejecting the corrective suggestion cannot clear the
blocking item. Source-overlap records retain both competing component IDs,
intersection volume, and smaller-component fraction so a human or LLM can make
an explicit one-owner decision.

## Contained-medium request editor

Human-review queue items for `apply_contained_medium` open a typed editor
before geometry review. The editor:

- lists fluid materials configured by the immutable user/project policy;
- lists current component occurrences recognized as straight cylindrical or
  conical annular pipes;
- rebuilds lumen ports from fresh topology rather than trusting the LLM
  payload;
- requires an explicit disposition for every port;
- continuously regenerates the OCCT validity, closure, leak, and material
  overlap certificate;
- disables **Review certified geometry** until the certificate passes.

Continuing stages the corrected typed request in the session and decision log.
The ordinary geometry-review window then shows the proposed fluid cylinder or
frustum as a translucent cyan overlay and performs the final apply through the
session API. Cancelling the editor leaves the previous request unchanged.
Unsupported network decisions such as connected/open ports remain visibly
selectable but blocking until the Phase 6 network generator is available.

# Qt, LLM Progress, and CAD Visual Feedback Implementation Plan

## Status

- **Document status:** implementation in progress.
- **Created:** 2026-09-02.
- **Primary objective:** keep a human continuously informed during long CAD
  operations through truthful progress, live Qt/OCCT highlighting, cancellable
  jobs, and durable visual review artifacts that can also be consumed by an
  LLM harness.
- **Primary UI:** the existing Qt 6 `DecisionWindow` and OCCT
  `AIS_InteractiveContext` viewport.
- **LLM integration:** model-agnostic structured job API first; DeepSeek
  Harness may consume it through an MCP adapter, but is not a geometry or
  certification authority.
- **Safety invariant:** only csgify engine certificates may mark geometry as
  committed, exact, overlap-safe, or emission-ready. Qt and an LLM may present
  evidence and request operations; neither may manufacture safety state.

### Implementation checkpoint — 2026-09-02

Implemented:

- general progress contract, JSONL round trip, fixed-point adapter, and
  per-solid analysis/batch-removal certification events;
- callback-exception cancellation and rollback at safe mutation boundaries;
- reusable Qt worker/controller, throttled progress model/dock, live elapsed
  display, conservative ETA, and conflicting-control exclusion;
- in-place OCCT job highlighting with selection/overlap precedence;
- standalone review packets with event log, diagnostics, summary digests, and
  initial/plan/terminal images;
- project schema v5 append-only `job_events`, resumable event queries, latest
  job progress in the Qt workspace, and CLI status/events/cancel access.
- preview, STEP export, and CSG emission progress with safe-boundary
  cancellation; STEP body totals and per-body events; and asynchronous Qt
  preview execution with live review artifacts.
- durable `ProjectJobProgressSink` normalization across retries, throttled
  heartbeats and cancellation polling, atomic terminal-event/job completion,
  and artifact-linked progress for the project-owned Alea void worker.
- first-class Qt component-removal planning: candidates carry stable component
  instance/definition identity, multi-solid occurrences are grouped into exact
  target sets, protected rows are disabled, eligible high-confidence rows can
  be selected together, and confirmation is bound to the source revision;
- asynchronous atomic Qt removal for both whole-set and one-item autonomy
  reviews, with locked mutation controls, cancellation, live target
  highlighting, one committed revision/undo step, and terminal review images.
- project-owned Qt removal jobs with exact targeted claiming, normalized
  append-only progress, project cancellation polling, component/target plan
  artifacts, terminal review-packet publication, and artifact-linked atomic
  completion.

Remaining:

- implement the asynchronous MCP adapter, revision-bound approvals, and the
  optional DeepSeek Harness sidecar;
- complete large-assembly performance and display-capable capture qualification.

## 1. Existing foundation

The implementation should extend, not replace, the current architecture:

- `src/viewer_qt.cpp` already owns a native Qt/OCCT viewport, stable-ID
  selection, local-context/isolation modes, review overlays, before/preview/
  applied states, a history dock, fixed-point progress widgets, cancellation,
  and worker-thread execution.
- `include/csgify/fixed_point.hpp` already defines immutable safe-boundary
  progress events and callback cancellation semantics.
- `src/session.cpp` publishes fixed-point events before and after expensive
  phases and preserves the last certified state when cancellation is requested.
- `src/main.cpp` already serializes fixed-point progress as JSON Lines.
- `include/csgify/project_store.hpp` already provides durable conversion jobs,
  worker ownership, heartbeats, cancellation, retry, abandoned-job recovery,
  and immutable artifact records.
- `src/session_view.hpp` already separates displayable `TopoDS_Shape` entries
  from the public session API.

The missing pieces are a general progress contract shared by operations other
than fixed-point decomposition, a reusable Qt job controller/dock, incremental
highlight roles, milestone image capture, and an LLM-facing adapter.

## 2. Target user experience

### 2.1 During a long operation

The Qt window remains interactive for camera movement and inspection. A
`QDockWidget` named **Job progress** shows:

- operation and current phase;
- determinate progress when a real total is known, otherwise an indeterminate
  activity bar;
- completed/total counts, current target, elapsed time, and a conservative ETA;
- geometry revision at job start and the latest committed revision;
- warnings and diagnostics as soon as they become available;
- `Cancel`, `Show current`, `Show plan`, `Fit`, and `Open artifact` actions;
- an explicit provisional banner until final certification commits.

The viewport uses the following job highlight roles independently of ordinary
geometry status colors:

| Role | Appearance | Meaning |
|---|---|---|
| removal candidate | red | proposed for suppression, not yet removed |
| active target | yellow | currently being processed |
| processed provisional | muted/transparent | staged but not yet committed |
| certified result | normal status color | committed engine result |
| warning/review | orange | requires human attention |
| overlap blocker | bright red | certified overlap evidence |
| selected | cyan | explicit human selection; highest display priority |

For a 305-solid batch, Qt initially colors all candidates red. It should not
rebuild or repaint the complete scene after every removal. The UI coalesces
events and applies display updates at most 4-10 times per second. Full scene
reconstruction occurs only at a committed revision boundary.

### 2.2 Terminal states

The UI distinguishes these outcomes:

- **Certified and committed** — normal model colors, committed revision shown.
- **Stopped safely with partial certified progress** — committed pieces shown;
  remaining blockers selected for review.
- **Cancelled** — last certified revision retained.
- **Failed and rolled back** — pre-job revision restored and the cause shown.
- **Needs human review** — provisional proposal remains visible, but mutation
  controls require an explicit revision-bound approval.

No provisional scene may use the same label or styling as a certified result.

## 3. Architecture

```text
                         immutable JobProgressEvent
  csgify session/engine  ------------------------------+
          |                                             |
          | final certified result                      v
          |                                      CadJobController
          |                                      /       |       \
          v                                     /        |        \
  committed document revision            Qt JobDock   JSONL     MCP adapter
          |                                  |           |           |
          v                                  v           v           v
  session_view entries --------------> OCCT viewport  audit log  LLM harness
```

The engine publishes facts. `CadJobController` fans them out. Qt presentation,
JSON persistence, and MCP serialization are consumers of the same immutable
event stream.

### 3.1 Core progress contract

Add `include/csgify/progress.hpp` and `src/progress.cpp`:

```cpp
enum class JobOperation {
    analyze,
    batch_remove,
    fixed_point_decomposition,
    decomposition_preview,
    export_step,
    emit_csg,
};

enum class JobPhase {
    queued,
    resolving_targets,
    loading_geometry,
    analyzing_solids,
    staging_mutation,
    certifying_solids,
    checking_overlaps,
    terminal_verification,
    committing,
    awaiting_review,
    finished,
};

enum class JobOutcome {
    none,
    committed,
    certified_partial,
    cancelled,
    rolled_back,
    failed,
};

struct JobProgressEvent {
    unsigned schema_version = 1;
    std::string job_id;
    std::uint64_t sequence = 0;
    JobOperation operation = JobOperation::analyze;
    JobPhase phase = JobPhase::queued;
    JobOutcome outcome = JobOutcome::none;
    std::string message_code;
    std::string target_id;
    std::string candidate_id;
    std::size_t completed = 0;
    std::size_t total = 0;
    bool total_known = false;
    double elapsed_ms = 0.0;
    std::size_t base_revision = 0;
    std::size_t committed_revision = 0;
    std::vector<Diagnostic> diagnostics;
    std::vector<std::string> changed_shape_ids;
    std::string artifact_id;
};

using JobProgressCallback =
    std::function<bool(const JobProgressEvent&)>;
```

Rules:

1. Events have monotonically increasing sequence numbers per job.
2. Exactly one terminal event is emitted.
3. `completed` never decreases within the same phase.
4. A percentage is shown only when `total_known && total > 0`.
5. Returning `false` requests cancellation at the next safe boundary.
6. A callback exception is treated as cancellation, never as authorization to
   skip validation.
7. Events are emitted outside internal locks and contain copied immutable data.
8. The core uses stable message codes; Qt owns localized human text.
9. Heartbeats are emitted at least every 15 seconds during operations that
   cannot provide finer-grained progress.

Keep `FixedPointProgressEvent` temporarily and implement a lossless adapter to
`JobProgressEvent`. Remove the specialized Qt path only after CLI, Pi, replay,
and regression consumers have migrated.

### 3.2 Instrumentation order

Instrument operations in this order:

1. `remove_solids`:
   `resolving_targets -> staging_mutation -> certifying_solids ->
   checking_overlaps -> committing -> finished`.
2. `analyze`/`analyze_model`: publish per-solid certification progress and the
   overlap phase without weakening analysis caching.
3. fixed-point decomposition: adapt existing events and retain candidate,
   splitter, residual, and accepted-action metrics.
4. preview generation, STEP export, and CSG emission.

Add overloads/options rather than breaking existing callers:

```cpp
CommandResult remove_solids(
    Session&,
    const std::vector<std::string>&,
    ReportIdMode = ReportIdMode::scoped,
    JobProgressCallback = {});
```

The final `finish_document_mutation` analysis must receive the same progress
sink, so the expensive certification work is visible rather than only the fast
suppression stage.

### 3.3 Qt job execution

Add dedicated Qt files rather than growing `viewer_qt.cpp` indefinitely:

```text
src/qt/cad_job_controller.hpp/.cpp
src/qt/cad_job_worker.hpp/.cpp
src/qt/job_progress_dock.hpp/.cpp
src/qt/job_progress_model.hpp/.cpp
src/qt/view_artifact_capture.hpp/.cpp
```

Enable `AUTOMOC` for the `csgify` executable when the Qt viewer is enabled.

`CadJobWorker` is a `QObject` moved to a `QThread`. It owns one operation call
and emits value-type signals:

```cpp
signals:
    void progress(csgify::JobProgressEvent event);
    void completed(csgify::CommandResult result);
    void failed(QString code, QString message);
```

Register required metatypes before queued delivery. `CadJobController` owns
the thread, cancellation token, state machine, elapsed timer, and terminal
cleanup. It replaces direct `QThread::create([this, ...])` usage incrementally.

Threading invariants:

- `Session` has at most one mutating job at a time.
- All session calls capable of mutation are disabled while the worker owns it.
- Qt widgets, `AIS_InteractiveContext`, `V3d_View`, and presentation objects
  are touched only on the GUI thread.
- Worker events carry stable IDs and metrics, never live AIS handles.
- Camera movement, selection of already displayed shapes, and opening stored
  artifacts remain available during work.
- Closing a window requests cancellation and waits for a safe boundary; it
  does not terminate an in-flight OCCT Boolean.

### 3.3.1 Qt component-removal review

The presence of `CadJobController::startBatchRemoval` is not sufficient by
itself: the Qt UI must expose a deliberate removal-plan workflow rather than
calling synchronous `remove_solid` once per accepted row.

Add a component-removal panel that:

- groups `RemovalCandidate` rows by stable assembly occurrence/component ID
  when imported metadata provides one, while clearly labeling ungrouped
  solid-only candidates;
- resolves every selected component to an immutable, deduplicated set of
  scoped solid/piece IDs at the current document revision;
- disables apply when ownership is ambiguous, a target is inactive, the
  revision changed, or a protected component/material is included;
- shows confidence, reasons, blockers, individual and combined volume
  fractions, and the exact target count before confirmation;
- offers individual checkboxes and `Select eligible high-confidence`, without
  silently selecting manual or blocked candidates;
- colors the complete proposed target set red before mutation and retains the
  before/removal-plan/applied comparison until the job terminates;
- launches one `startBatchRemoval` operation for the confirmed target set,
  producing one certification pass, at most one revision increment, and
  one-step undo for the complete component batch;
- keeps camera movement, inspection, cancellation, and artifact access
  responsive while mutation controls are locked;
- records component IDs, resolved solid IDs, base revision, human
  confirmation, progress events, and review artifacts in project mode.

Single-item autonomy reviews should use this same asynchronous path with a
one-component/one-solid plan. The synchronous `ViewerController::remove_solid`
path may remain for headless compatibility, but must not be the normal Qt
execution path for a potentially expensive certified removal.

### 3.4 Job progress dock

`JobProgressDock` contains:

- phase title and provisional/certified state badge;
- primary `QProgressBar`;
- counts for targets, certificates, residuals, candidates, and splitter calls;
- current target and strategy;
- `QElapsedTimer`-based elapsed time;
- ETA computed only after at least five comparable completions, using an
  exponentially weighted moving average and displayed with `~`;
- bounded diagnostic/event list;
- thumbnail strip for plan, current target, post-mutation, and final views;
- buttons for cancellation and viewport navigation.

Use a `QTimer` to coalesce event bursts. The progress model keeps every event
needed for audit, while the widget refreshes no faster than 100-250 ms.

The existing fixed-point controls in the decision panel become job launch
controls; their detailed status moves into this dock. The bottom decision
history remains an audit-oriented human narrative.

### 3.5 Incremental viewport highlighting

Add a display concern separate from `SolidViewStatus`:

```cpp
enum class JobHighlightRole {
    none,
    removal_candidate,
    active_target,
    provisional_processed,
    warning,
};
```

Extend `OcctViewWidget` with:

```cpp
void setJobHighlights(
    const std::unordered_map<std::string, JobHighlightRole>&);
void setActiveJobTarget(const std::string& id);
void clearJobHighlights();
```

Highlight precedence is:

```text
human selection > overlap blocker > active target > warning
> removal/provisional role > ordinary SolidViewStatus
```

Update AIS color/transparency in place. Do not call `rebuildScene()` for normal
progress events. At commit, call `collect_view_entries`, rebuild once, clear
provisional highlights, and retain any terminal warning selection.

For batch removal, preserve `before_entries_` until the job reaches a terminal
state so the user can switch between removal plan and committed result.

## 4. Visual artifacts

### 4.1 Milestone captures

Capture, at minimum:

1. `initial-isometric.png`;
2. `removal-plan.png` with candidates red;
3. `current-target.png` only for warnings or human-review escalation;
4. `post-mutation-provisional.png` when useful;
5. `final-certified.png` or `rolled-back.png`.

Capture must run on the GUI thread after pending presentation updates. Prefer
the OCCT `V3d_View` pixmap/dump API supported by the pinned OCCT version; use a
Qt framebuffer capture only as a documented fallback. Save and restore the
user camera when generating canonical isometric/front/top/side views.

Do not capture every solid. Default limits:

- one plan image;
- no more than one current-target capture every 30 seconds;
- captures on warnings, review requests, and terminal state;
- maximum dimensions configurable, with 1280x720 as the initial default.

### 4.2 Artifact packet

Store a job review packet:

```text
jobs/<job-id>/
  events.jsonl
  summary.json
  initial-isometric.png
  removal-plan.png
  final-certified.png
  diagnostics.json
```

Write images and JSON to temporary sibling files and atomically rename them
after successful completion. Record them through `ArtifactStore` when a
project is open. Standalone viewer sessions use a user-selected or temporary
job-artifact directory and expose `Open artifact` from Qt.

`summary.json` records source digest, base/final revisions, operation,
requested targets, committed targets, volume change, outcome, certificate
summary, timestamps, and artifact digests. It must not claim certification
unless the terminal engine result says so.

## 5. Project job persistence

Support two modes:

- **Standalone viewer:** in-memory `CadJobController` plus JSONL/artifact files.
- **Project workspace:** existing `ProjectStore` conversion job is the durable
  job identity and cancellation authority.

Add an append-only `job_events` table in a schema migration:

```text
job_id, sequence, emitted_at, phase, completed, total, total_known,
target_id, message_code, payload_json, artifact_digest
```

Do not rewrite old events. Update the existing job heartbeat from the event
consumer at a bounded cadence, not for every event. The project Jobs tab gains:

- live phase and progress columns;
- last heartbeat/elapsed time;
- cancel action for queued/running jobs;
- double-click to open job detail and associated images;
- filtering by state and operation;
- stale-job recovery retaining its current explicit human confirmation.

## 6. LLM and DeepSeek Harness adapter

Implement this only after the engine and Qt consume the same stable event
contract. The first adapter should be a local MCP server or equivalent narrow
service exposing:

```text
cad_analyze
cad_propose_removals
cad_start_batch_removal
cad_start_decomposition
cad_job_status
cad_job_events
cad_job_preview
cad_cancel_job
cad_submit_review_decision
cad_export
```

Long operations return a job ID immediately. They never remain inside a single
long-lived tool call. Status responses include latest sequence, phase,
completed/total, elapsed time, current target, base/current revision, outcome,
diagnostics, and available image artifacts.

DeepSeek Harness is an optional sidecar launched and monitored with `QProcess`.
It connects to the local adapter through stdio or loopback transport. Do not
embed Harness-specific types in the engine, session API, project database, or
Qt widgets.

Mutation authorization uses an approval object bound to:

- source digest;
- document ID and revision;
- exact stable target-ID set;
- operation and policy digest;
- expiry/nonce;
- human decision identity when applicable.

The engine rejects stale approvals. The harness may explain, plan, poll, and
request an action; it cannot bypass session mutation postconditions or final
emission gates.

The initial agent profile should expose CAD tools only. Raw shell, unrestricted
filesystem, and network tools are outside the CAD workflow and disabled by
default.

## 7. Delivery phases

### Phase 1 — General progress contract

- Add `progress.hpp/.cpp`, JSON serialization, and invariant tests.
- Adapt fixed-point progress without breaking existing consumers.
- Instrument batch removal and its final analysis/certification.
- Add truthful 15-second heartbeat events around opaque OCCT operations.

**Exit:** CLI JSONL and a test callback observe ordered phase events, safe
cancellation, and exactly one terminal outcome for batch removal and
fixed-point decomposition.

### Phase 2 — Reusable Qt job layer

- Enable Qt `AUTOMOC` for the viewer target.
- Add worker/controller/progress-model classes.
- Move fixed-point execution from direct `QThread::create` into the job layer.
- Launch batch removal through the same layer.
- Replace normal Qt `remove_solid` acceptance with the asynchronous batch path,
  including the one-target case.
- Enforce one session writer and disable conflicting controls.

**Exit:** the GUI never freezes during the instrumented operations; cancel
preserves the last certified revision; closing during work behaves safely.

### Phase 3 — Job progress dock

- Add state badge, phase text, progress, counts, elapsed time, conservative
  ETA, diagnostics, and actions.
- Add event throttling and human-readable message mapping.
- Keep the camera and safe selection responsive.

**Exit:** no instrumented job is visually silent for more than 15 seconds and
unknown progress is never presented as a fabricated percentage.

### Phase 4 — CAD highlighting and review views

- Add independent job highlight roles and in-place AIS styling.
- Show removal plans, active target, provisional processed targets, and
  terminal state.
- Preserve before/plan/applied comparison until completion.
- Add `Show current`, `Show plan`, and standard-view actions.
- Add the component-removal table, eligibility filters, combined-volume
  summary, revision-bound confirmation, and exact resolved-target preview.

**Exit:** a user can visually identify proposed, active, provisional, retained,
and blocking geometry throughout batch removal and decomposition.

### Phase 5 — Screenshots and artifact packets

- Implement GUI-thread OCCT captures with camera save/restore.
- Write JSONL, summary, diagnostics, and milestone images atomically.
- Register immutable artifacts in project mode.
- Add thumbnails and `Open artifact` actions.

**Exit:** every long job produces a bounded review packet suitable for a human
or multimodal LLM.

### Phase 6 — Durable project job progress

- Add the `job_events` schema migration and APIs.
- Connect heartbeat, cancellation, recovery, and artifact publication.
- Extend the Project Workspace Jobs tab with live detail and previews.

**Exit:** Qt can close and reopen without losing the audit trail or confusing
a stale running job with active work.

### Phase 7 — LLM/MCP adapter and optional DeepSeek sidecar

- Expose asynchronous job tools and image artifacts.
- Add revision-bound approval handling.
- Add `QProcess` lifecycle/health display for an optional Harness sidecar.
- Keep the adapter usable by other LLM harnesses.

**Exit:** an LLM starts a job, Qt shows the same live state, the human can
inspect/cancel/approve it, and both observe the identical terminal result.

### Phase 8 — Qualification and rollout

- Run small deterministic fixtures and representative large assemblies.
- Measure engine overhead, UI event volume, render latency, memory, cancellation
  latency, artifact sizes, and recovery behavior.
- Document quiet/normal/verbose policies and production defaults.

**Exit:** all acceptance criteria below pass without weakening existing
geometry, overlap, approximation, replay, or emission tests.

## 8. Testing strategy

### 8.1 Core tests

- monotonic event sequence and phase-local completed counts;
- exactly one terminal event on success, failure, rollback, and cancellation;
- callback exceptions become safe cancellation;
- batch removal increments the revision once and one undo restores the batch;
- invalid/mixed-document batches emit failure without partial mutation;
- final event revision matches the returned report;
- fixed-point specialized/general event adapters preserve metrics;
- JSONL round trip and schema-version rejection.

### 8.2 Qt tests

Keep job state transitions and ETA calculations in a testable model separate
from widgets. Add Qt tests for:

- queued/running/cancelling/terminal transitions;
- determinate versus indeterminate progress;
- coalescing without loss of terminal/warning events;
- conflicting controls disabled while the session writer is active;
- cancellation signal delivery;
- highlight precedence and clearing at commit/rollback;
- stale event rejection by job ID and sequence;
- window close while a job is active;
- component grouping resolves to the expected deduplicated scoped target set;
- blocked, manual, and protected candidates are never selected automatically;
- changing the document revision invalidates the pending removal plan;
- accepting one or many components calls `startBatchRemoval` exactly once;
- success rebuilds the scene once and one undo restores the complete batch;
- cancellation keeps the before view and committed revision unchanged.

Use `QT_QPA_PLATFORM=offscreen` for widget tests where supported. Keep a
display-capable OCCT smoke test for actual viewport capture; skip it with an
explicit reason when no OpenGL display is available.

### 8.3 Persistence and adapter tests

- crash after event append but before artifact publication;
- heartbeat expiry and explicit stale-job recovery;
- cancellation from Qt reflected in the adapter and vice versa;
- approval rejected after revision or target-set change;
- artifact digest validation and missing-image diagnostics;
- MCP calls return promptly with a job ID;
- no model or network dependency in protocol contract tests.

### 8.4 Performance gates

- core event publishing overhead below 1% with no consumer;
- bounded event memory independent of job duration;
- UI refresh no faster than configured throttle;
- no complete AIS scene rebuild per processed solid;
- default artifact packet remains bounded by configured count and dimensions;
- cancellation acknowledged immediately in Qt and honored at the next safe
  engine boundary.

## 9. Acceptance criteria

The feature is ready when:

1. A long batch removal shows its plan before mutation and real phase progress
   during final certification.
2. A user can select component occurrences in Qt, inspect the exact solids and
   combined volume to be removed, and confirm one revision-bound atomic job.
3. The Qt event loop remains responsive for camera movement, selection, and
   cancellation.
4. No active instrumented job is silent for more than 15 seconds.
5. Percentages and ETAs are shown only from real totals and observed samples.
6. Provisional and certified geometry are visually and textually distinct.
7. Cancelling or failing never leaves an uncertified document revision active.
8. A final or rollback image and machine-readable summary are available.
9. Qt, CLI JSONL, project persistence, and an LLM adapter agree on job ID,
   sequence, phase, document revision, and outcome.
10. Existing session, viewer, replay, approximation, overlap, and MCNP safety
   tests continue to pass.
11. The implementation remains fully useful without DeepSeek Harness running.

## 10. First implementation slice

The first mergeable slice should be deliberately narrow:

1. introduce `JobProgressEvent` and adapt fixed-point events;
2. add progress instrumentation to `remove_solids` and final analysis;
3. add `CadJobController` plus a generic `JobProgressDock`;
4. show batch removal candidates red and the active certification target
   yellow with throttled updates;
5. support safe cancellation;
6. capture `removal-plan.png` and one terminal image;
7. retain JSONL output for external/LLM polling;
8. qualify with `four_solids.step` and one representative large assembly.

This slice directly solves the human-silence problem without making the Qt UI
dependent on an LLM harness or waiting for the full project-job/MCP rollout.

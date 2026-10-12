# First product prototype implementation plan

Status: in progress — first end-to-end increment implemented 2026-09-28.
Created: 2026-09-28.

Delivery direction updated 2026-09-28: implement the assistant inside the Qt
application, including user questions, answers and follow-up conversation.
[Embedded agent implementation plan](EMBEDDED_AGENT_IMPLEMENTATION_PLAN.md)
defines the architecture, sequencing and acceptance gates for Phase 5. The
external CLI agent remains supported, but is no longer the primary assisted
prototype experience.

## Implementation checkpoint — 2026-09-28

The first vertical slice is implemented in the current working tree:

- `csgify project review` opens a New/Open project launcher; New initializes
  the SQLite store and conservative policy without replacing existing files;
- the workspace can select or replace its source STEP, shows the effective
  preparation/authority policy before import, and can reload policy changes;
- asynchronous document loading now receives the project conversion options,
  then persists the analysis snapshot, spatial index, and recognition cache;
- explicit STEP and MCNP exports use the existing worker/controller path,
  retain the open project on success or failure, show progress/cancellation,
  and archive successful outputs in the immutable project artifact store;
- an in-app typed policy editor covers purpose, preparation, authority, and
  primary approximation budgets. Its atomic complete-policy writer preserves
  contained-medium materials, protection lists, and advanced fields the dialog
  does not edit. Saving unloads stale geometry and requires fresh evidence;
- **Restore saved decisions** compacts the append-only journal and reconstructs
  a live session through the existing typed CLI replay dispatcher on the Qt
  worker. Project restore requires the exact source digest, recorded healing
  and unit preparation, and recorded csgify and OCCT versions; historical
  outputs are not rerun and analysis evidence is regenerated before review;
- manifests now carry the compiled csgify project version, and opening an
  existing journal for append fails closed when its source digest, csgify
  version, or OCCT version does not match the active session;
- project-backed component removal now compensates a failed artifact/SQLite
  publication with a journal-first undo and restored live geometry. Failed
  journal appends return to the last complete NDJSON boundary; if compensation
  still cannot be recorded, the workspace locks mutation and remains visibly
  in recovery-required state;
- recipe compaction and strict project reconstruction preserve the latest
  review decision and rationale for each target/proposal identity. Restored
  removal alternatives are initialized as rejected and remain visible but
  unchecked unless the operator explicitly reconsiders them;
- supported review windows journal pending checkpoints with their typed action,
  candidate identity, rationale, and base revision. The project Overview can
  resume decomposition, removal, surface-replacement, hole-fill, and
  contained-medium checkpoints only after regenerating current evidence; stale
  or missing candidates and requests remain pending with a recovery action;
- all eight autonomy operations now have a Qt route. Fixed-point, budgeted
  shatter, residual removal, and overlap ownership run as explicit worker-backed
  certified attempts when policy allows, or identify the exact policy/owner
  recovery step when it does not. The project editor exposes the corresponding
  approximation and topology authority;
- focused face context now includes the selected face, its owning solid, and
  owner identity on regenerated surface candidates instead of returning an
  empty solid context;
- focused semantic context now carries the resolved occurrence and definition,
  same-definition occurrence page, material source and available definitions,
  protection lists, and bounded pipe-port interface decisions. Analyzed solids
  retain their resolved occurrence identity, including multi-solid components;
- solids, candidate families, same-definition occurrences, and local overlaps
  expose deterministic offset-page metadata under a hard serialized-size cap;
- offscreen workspace smoke coverage now requires the source, policy, and both
  export controls; the controller regression verifies policy-sensitive import
  plus STEP and MCNP export; a launcher smoke test verifies both entry actions.
  The full suite passes 75/75 after this increment.

This is not milestone A. The first exact-source human-edit reconstruction path
is working, but recoverable multi-file publication, legacy checkpoint recovery,
the broader restart acceptance matrix, and native-display inspection remain.

This is the delivery slice of the broader
[human/LLM workspace roadmap](UX_FIRST_HUMAN_LLM_WORKSPACE_IMPLEMENTATION_PLAN.md).
It reuses the existing engine, Session, project database, artifacts, job layer,
operation registry, CLI, and Qt review controls.

## Product outcome

An engineer can create or reopen a project, import a STEP assembly, establish
conversion constraints, obtain certified exact progress, review unresolved
geometry, export the supported result, and resume after closing the application.
An external agent can assist through the same project and operation contract.

Two independently demonstrable milestones:

- **A — Human prototype:** the complete workflow works from the GUI, including
  durable partial results and restart. Routine use needs no shell commands or
  hand-edited JSON.
- **B — Assisted prototype:** the embedded assistant answers user questions,
  retrieves focused context, proposes supported actions, handles review or
  refusal, and resumes the same project. At least one actual model run is
  recorded and evaluated through the desktop application.

Initial supported envelope: one source STEP assembly per project, analytic or
mostly analytic geometry, existing certified operations, and explicit residuals
for unsupported geometry. Use the current Linux/Qt/OCCT environment first.
STEP output may preserve a partial result; MCNP output remains subject to the
engine's export gates. Geometry export is distinct from simulation qualification.

## Starting evidence

- The engine already imports, analyzes, decomposes, previews, applies bounded
  approximation, handles overlaps, and emits geometry through Alea.
- Project persistence, source/policy-bound autonomy requests, idempotent retries,
  a shared autonomy apply/reconstruction dispatcher, and worker jobs exist.
- The redesigned geometry workspace has search, Alternatives/Evidence tabs,
  geometry controls, and guarded actions. See
  [its design specification](../docs/GEOMETRY_WORKSPACE_DESIGN.md).
- The existing CLI agent contract is documented in
  [CLI_AGENT_PROTOTYPE.md](../docs/CLI_AGENT_PROTOTYPE.md).
- The full suite passed 74/74 before the workspace redesign. The final UI
  change passed three focused Qt/workspace tests. This is not a new full-suite
  result or evidence of real-model decision quality.

Current integration gaps are concrete: legacy contained-medium checkpoints
without a stable staged-request ID need manual restaging; a repeated-occurrence
end-to-end context fixture remains incomplete. The broader interruption and
restart acceptance matrix is not yet complete.
Inspect current source before implementation because the working tree contains
substantial uncommitted development.

## Sequence and dependencies

| Phase | Deliverable | Depends on | Exit gate |
|---|---|---|---|
| 0 | Bounded scope and acceptance cases | Existing tree | Supported actions and representative cases are recorded |
| 1 | Project entry, import, policy, export | 0 | One exact case completes from the GUI |
| 2 | Durable edits and reopen | 1 | Human edits and partial results survive process restart |
| 3 | Complete review and recovery | 2 | Every exposed action completes or stops usefully; milestone A |
| 4 | Focused context | 0; integrate against 2–3 | Face, solid, and residual contexts are bounded and actionable |
| 5 | Embedded conversation and agent workflow | 3–4 | User Q&A, scripted protocol checks and a real assisted desktop run pass |
| 6 | Prototype qualification and handoff | 5 | Acceptance corpus, installation instructions, and known limits; milestone B |

Phase 4 can be developed alongside the human workflow once identities and
policy fields are agreed. Neither milestone depends on BRepGraph adoption.

## Phase 0 — Lock the prototype contract

- Inventory GUI-exposed mutations against the registry: arguments, policy
  enforcement, preview, commit, undo, persistence, reconstruction, and recovery.
  Human commands outside the autonomy subset still need durable coverage.
- Select four to six representative assemblies at the start. Use fixtures for
  fast regressions and include at least one user-representative assembly for
  milestone qualification. Record source identity and expected outcome.
- Record the actual source/dependency versions and development build settings.
  Keep the user's existing changes intact; do not treat HEAD as the entire
  implemented baseline.
- Adopt the existing UI design skills and workspace specification for further
  screens. Add a small state sketch for project entry, export, and reopen.

Acceptance: a short capability matrix and scenario list distinguish supported
operations, actions requiring review, and useful unsupported outcomes.

## Phase 1 — Complete project entry and export

Implement a New/Open project entry point in the existing Qt executable. Let the
user select a project directory and source STEP. Reuse project initialization;
opening an existing project must not replace its policy or database.

Provide a small policy form for existing supported settings: intended analysis,
unit assertion versus normalization, healing, exact-only default, supported
approximation budgets, and authority mode. Show effective values before loading.
Preserve existing policy fields when editing a subset; do not silently drop
material definitions or restrictions. Controls must only promise protections
that every exposed mutation path enforces. Defer unsupported policy controls
or explicitly disable the corresponding operation.

Pass the effective preparation and mutation policy through the document-loading
job and subsequent human/agent commands. A changed preparation policy requires
reimport. Changed authority or budgets invalidate affected pending approvals;
neither edit silently changes the meaning of already accepted history.

Add explicit Export STEP and Export MCNP actions with destination selection,
progress, cancellation at supported boundaries, and a result summary. Reuse
worker/session export paths. Exporting should leave the project available for
continued work. A failure leaves the project usable and the previous valid
output intact; publish new output only after successful completion. Report
which revision and policy produced the artifact, and any unresolved geometry.

Likely touchpoints: `src/main.cpp`, `src/viewer_qt.cpp`, `src/qt/cad_job_*`,
`src/autonomy.cpp`, and `src/project_store.cpp`. Extract shared policy adapters
where CLI and GUI would otherwise diverge.

Acceptance: starting from the application, create a project, choose a known
exact STEP, inspect units, analyze, and export both formats. Test invalid
policy, failed import, cancellation, and an unwritable export destination.

## Phase 2 — Make human work durable

Use project-owned immutable source content, prepared-state identity, and an
ordered operation journal as the reconstruction authority. Reuse existing
artifact and replay machinery. A STEP snapshot is a useful artifact, but must
not be the sole record of target identity, metadata, approximation usage, or
approval history.

Persist enough information to restore:

- source digest, preparation policy, effective authority policy, and an
  engine/kernel compatibility identifier;
- accepted human and agent operations with stable request/operation IDs,
  effective parameters, base revision, target/candidate evidence identity,
  and approximation-ledger effects;
- undo operations, pending/rejected decisions and rationale, unresolved
  targets, and references to successful export artifacts.

Inventory existing replay coverage before adding a new format. Extend a shared
typed operation path for missing GUI operations; do not add a second parallel
mutation switch in Qt. Make unavailable reconstruction an explicit unsupported
operation before it is offered as durable project work.

Define a recoverable publication protocol across the in-memory Session, journal,
artifact files, and SQLite. Stage immutable results, atomically publish their
references and operation outcome, and expose saved state only after durable
publication. If geometry changes but publication fails, restore the last
durable state or visibly block further edits pending recovery. Do not assume
a SQLite transaction alone makes files and an in-memory mutation atomic.

Reopen from the latest durable checkpoint/journal, restore pending review, and
verify identity and effective policy before accepting new mutations. Bind
reconstruction to pinned engine compatibility; incompatible versions require
fresh validation. Merely moving an identical source file should not imply
changed geometry; changed contents at the same path must be detected.

Initially retain unreferenced artifacts and report them as incomplete work.
Automatic garbage collection is deferred. Never infer a committed operation
from an orphaned artifact.

Acceptance: apply an edit, reject an alternative, undo another edit, close the
process, reopen, and compare geometry, metadata, revision semantics, ledger,
decisions, and export outcome. Inject interruptions before/after artifact and
database publication. Retry must not duplicate a mutation, log entry, or charge.

## Phase 3 — Finish human review and useful stopping

Map every exposed operation to either a working review adapter or a concrete
recovery action. Fixed-point resolution, budgeted shatter, residual removal,
and overlap ownership now use worker-backed transactional review with explicit
policy recovery when execution is not authorized. Retain the residual,
select an existing alternative, suspend, or repair/reimport the source when
proof remains unavailable. Do not leave an enabled Review button that throws.

Show the target, change, regenerated evidence, cumulative budget impact, and
available decision. Separate missing engineering intent from failed geometric
proof. Human acknowledgment cannot turn unknown evidence into certification.
Preserve rejection reasons and do not resubmit the same rejected proposal
without changed evidence or an explicit change of intent.

Use clear durable task states: complete, partial, awaiting decision, failed,
and cancelled. Keep task outcome, conversion readiness, and export result
separate. Review stale items only after regeneration. Run expensive queue
evaluation/execution through the existing job layer so the GUI stays responsive.

Acceptance: exact conversion, a human-approved permitted change, blocked overlap
ownership, a rejected action, and an unsupported residual each have a usable
next step. A partial project closes and reopens with the same unresolved work.
Inspect the native viewport and keyboard/focus behavior on a real display.
Passing this phase together with phases 1–2 establishes **milestone A**.

## Phase 4 — Supply focused agent context

Extend the current context writer instead of creating a second analysis model.
Resolve selected faces and residual pieces to their owners and occurrences.
Include applicable material/protection data, relevant neighbours, blockers,
remaining budgets, engine-generated candidates, and relevant rejection history.
Label unknown semantics explicitly; unavailable information must not look like
an absent constraint.

Expose only supported actions with argument schemas, current applicability,
evidence limitations, and frontend recovery support. Add solid/face/history/
candidate limits, deterministic pagination, and explicit truncation metadata.
Reference large artifacts instead of embedding them. Reuse current topology
and metadata APIs; BRepGraph is optional future implementation work.

Acceptance: face/solid/residual selections return the right owner context;
repeated occurrences remain distinguishable; a large assembly obeys the output
budget and reveals omissions; rejected actions and protection constraints are
visible to the planner. Read-only queries leave the revision unchanged.

## Phase 5 — Deliver the embedded conversational agent

Follow [EMBEDDED_AGENT_IMPLEMENTATION_PLAN.md](EMBEDDED_AGENT_IMPLEMENTATION_PLAN.md).
Embed conversation and orchestration in Qt, with a scripted provider first and
one live provider afterward. Support user questions and follow-ups independently
of running geometry tasks. Reuse the existing context/autonomy contract through
internal APIs. A new MCP server and model downloads are outside this scope.

The embedded loop performs:

1. Open the project, answer user questions, and capture an explicit task objective.
2. Request focused context for remaining work.
3. Submit a typed proposal, ask a precise clarification question, or stop.
4. Let the engine evaluate it and route every Assisted mutation through Qt review.
5. Refresh context after a commit or rejection; checkpoint partial work on stop.

Keep the planner's transport constrained to read/query and typed proposal
submission. Policy, approval, and project-store writes belong to the application
or authorized human workflow. Enforce this boundary in the application tool
router and shared execution coordinator; prompting alone is insufficient.

Set explicit per-run action/retry/time limits and detect repeated requests or
stagnation. Treat lost inference, malformed requests, stale revisions, and user
cancellation as useful persisted outcomes. Hard job deadlines require process
supervision; operation-boundary cancellation must not be advertised as a hard
kernel deadline.

Record provider/model identity, supplied context, conversation, proposal, tool
result, human decision, and final outcome. Use a scripted planner to verify the protocol,
then run an actual model on the same decision cases. Reuse an available model
configuration; provider/account selection is needed only if none is available.

Acceptance: user questions receive evidence-linked answers without mutation;
a real assisted run completes or yields a justified partial result,
resumes after interruption, honors rejection/protection/budget constraints,
and never bypasses an unavailable operation through scripting.

## Phase 6 — Qualify and hand off the prototype

| Scenario | Required observable result |
|---|---|
| Exact analytic assembly | GUI import, deterministic progress, successful export, consistent reopen |
| Non-convex assembly | Candidate inspection, preview, accepted edit, durable history |
| Explicit approximation policy | Permitted change measured and charged once; retry preserves ledger |
| Protected feature or unresolved ownership | Mutation prevented or clarification requested through a usable recovery path |
| Unsupported geometry | Partial STEP/project retained; MCNP blocked where required; restart retains residuals |
| Source/policy change or interrupted commit | Incompatibility detected or last durable state recovered; no silent approval reuse |
| Misleading CAD text or repeated agent proposal | Authority unchanged; invalid/repeated work rejected or stopped |

Keep fixture checks fast and automated. Run the full suite at the milestone
gate, native GUI walkthroughs for the user journey, STEP reimport checks, and
an independent geometry check on representative MCNP output using an available
consumer. If the consumer is unavailable, record downstream validation as an
open qualification item rather than claiming it passed.

Compare assisted runs with deterministic/human use of the same cases. Record
accepted completion or residual outcome, human interventions, rejected actions,
geometry/ledger changes, elapsed time, and restart consistency. Do not choose
a model or claim time savings from one successful demonstration.

Provide a reproducible launcher/build recipe for the target machine, example
projects, a short import-to-export walkthrough, dependency identities, and a
known-limitations list. Record remaining unsupported operations. Passing the
assisted acceptance scenarios establishes **milestone B**.

## First implementation increment

Completed: **New/Open project → choose STEP → inspect/edit effective policy →
load and analyze → export STEP/MCNP**. The Qt loading worker uses the effective
project preparation policy, and focused regressions cover policy round-tripping,
policy-sensitive loading, and both export commands. The next reviewable
increment is durable reconstruction of accepted edits across restart.

The initial durable-reopen increment is implemented for newly bound decision
histories. Histories created before healing/unit preparation was recorded fail
closed and require reimport. Changed source contents, changed preparation, or
a changed csgify or OCCT version block restoration instead of applying stale
decisions. Existing journals also reject incompatible appends, preventing two
sources or engine identities from being mixed into one history. Full Phase 2
durability still requires extending the recoverable publication boundary from
project-backed component removal to every mutation adapter, adding pending
resume adapters for surface replacement, hole fill, and contained medium, and
the broader restart matrix described above.

## Deferred work

Delegated agent execution, additional model providers, broad model comparisons,
BRepGraph migration, arbitrary free-form conversion, new geometry algorithms, ITER-scale rollout,
automatic artifact collection, multi-source project editing, and cross-platform
installers are outside this slice. They must not delay proving the bounded
human and assisted workflows above.

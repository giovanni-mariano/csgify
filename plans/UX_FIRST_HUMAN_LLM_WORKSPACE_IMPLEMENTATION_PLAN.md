# UX-First Human and LLM Workspace Implementation Plan

## Status

- **Document status:** proposed implementation roadmap.
- **Created:** 2026-09-03.
- **Scope:** Qt workspace, decision review, project jobs and artifacts, CLI
  coherence, and bounded LLM integration.
- **Depends on:** the
  [Qt/LLM progress and visual-feedback plan](QT_LLM_PROGRESS_AND_VISUAL_FEEDBACK_IMPLEMENTATION_PLAN.md)
  and the existing `Session`, proposal, certification, autonomy, project-store,
  and progress contracts.
- **Related specifications:**
  [Qt Agent Workspace](../docs/QT_AGENT_WORKSPACE.md) and
  [Certified LLM Autonomy](../docs/AUTONOMY.md).
- **Primary objective:** make csgify understandable, responsive, and difficult
  to misuse for a human, whether the work is initiated directly or with an
  LLM.

This plan is the product-level umbrella over the existing technical plans. It
does not replace their geometry, progress, project persistence, or safety work.
It connects those capabilities into one coherent experience and adds the
missing human-agent interaction contract.

The bounded first delivery is sequenced in
[First product prototype implementation plan](FIRST_PRODUCT_PROTOTYPE_PLAN.md):
complete the human project lifecycle, then deliver an embedded conversational agent
through the same policy, review, persistence, and export paths.

The delivery choice was updated on 2026-09-28 to an embedded Qt assistant.
See [Embedded agent implementation plan](EMBEDDED_AGENT_IMPLEMENTATION_PLAN.md)
for conversation (user questions, grounded answers and follow-ups), shared
execution coordination, durable runs, one provider, and Assisted-mode delivery.
This supersedes the earlier external-only delivery choice; the CLI stays supported.

### Verification checkpoint - 2026-09-28

The current working tree builds successfully with
`cmake --build build -j2`. The full suite passes **75/75** with
`ctest --test-dir build --output-on-failure --parallel 2 --timeout 120`
in 56.50 seconds. `git diff --check` passes. This uses the existing development
build with an empty `CMAKE_BUILD_TYPE`; timings are not an optimized performance
baseline. The tree remains substantially uncommitted relative to `8037748`.

Reconciliation against the September 24 product review:

- **Fit exactness:** sampled spline residuals cannot grant exact authority;
  the narrow-ridge evidence regression passes. A globally certified spline
  deviation algorithm is still follow-up work.
- **Source-bound restart:** queue schema 4 binds source contents, preparation,
  authority/conversion policy, and normalized request identity. Changed-source,
  conflicting preparation, legacy-queue, and edited-payload regressions pass.
  Explicit engine/kernel version and candidate-geometry identity binding remain
  follow-up work; these checks do not establish compatibility across upgrades.
- **Execution/reconstruction:** both routes use the typed autonomy dispatcher.
  Budgeted shatter and overlap restart regressions pass, and reconstruction
  selects the budgeted surface path and effective fixed-point policy.
- **Focused context:** operation argument schemas, review/restart capability,
  and recovery names are advertised. Focused face context now returns the face,
  its owning solid, owner-bound surface candidates, bounded face IDs, and local
  overlap neighbours. Rejection history retains target/candidate identity,
  actor, rationale, and policy rule. Solids, candidate families, and local
  overlaps expose bounded offset pages. The selected owner now includes stable
  occurrence/definition identity, same-definition occurrence neighbours,
  material provenance and protection constraints, and bounded pipe-port
  interface decisions. A hard serialized-size limit rejects oversized output
  before publishing a partial context file. Focused residual pieces resolve to
  their source owner and carry bounded geometry/status evidence. A
  repeated-occurrence end-to-end fixture remains.
- **Human recovery:** all eight advertised autonomy operations have Qt routes.
  Fixed-point, budgeted shatter, residual removal, and overlap ownership use
  worker-backed transactional review when policy permits and actionable policy
  recovery when it does not; a human acknowledgement cannot bypass proof or
  budget gates.
- **Planner boundary and kernel ownership:** documentation distinguishes strict
  planner proposals from trusted Lua. Binary Booleans use non-destructive
  execution, splitters set the same policy, and import/export use XCAF document
  guards; the lifetime regression passes. This is not a complete audit of all
  healing/meshing mutators or isolation of a deployed planner.

Stage 1's implemented regressions are green. Stage 2 has substantial contract,
identity, retry, and persistence coverage but remains in progress. Its next
recorded slice is artifact-orphan retention/reporting and operator recovery.
The restart acceptance matrix remains a prerequisite for the full human/LLM
product milestone. No real-model planner evaluation, industrial
qualification, or downstream transport validation was performed here.

### Implementation checkpoint - 2026-09-03

Implemented from the first slice:

- explicit `project init` plus a SQLite read-only open path for project status
  and events; missing-project inspection no longer creates directories or a
  database;
- corrected project/apply/view help contracts and current Qt removal wording;
- deliberate `Ctrl+Enter` acceptance for non-removal proposals, removal of the
  unmodified mutation shortcuts, exact-count removal labels, and confirmation
  before applying certified autonomy changes;
- context-sensitive decision controls: inspection has no mutation controls and
  operation-specific reviews hide unrelated fixed-point controls;
- project readiness guidance, disabled unattached review actions, and automatic
  attachment of the displayed decision log to the loaded session;
- project STEP loading and initial analysis now run as one worker-owned job,
  hand the completed session back to Qt, expose progress/cancellation, and
  defer the requested review until analysis completes;
- final viewer emission now uses the same asynchronous job layer, keeps the
  review open on failure, and closes only after the worker reports successful
  verified completion;
- an adaptive world XY grid, color-coded corner XYZ trihedron, and independent
  Grid/Axes viewport toggles inspired by familiar robotics/CAD workspaces; the
  grid uses background-depth rendering on a plane below the model so it cannot
  cross or overpower visible solids;
- actionable standalone inspection: selecting an unresolved shape now shows
  its status and recommendation, generates decomposition candidates in place,
  and exposes document-wide certified resolution with an exact unresolved
  count; uncertain residuals remain unchanged and escalate into the same
  explicit preview/review controls;
- bounded progress delivery for Qt geometry jobs: non-milestone worker updates
  are rate-limited, OCCT highlight/redraw work is coalesced, and the JSONL
  review artifact is appended incrementally instead of being rewritten on
  every event, preventing candidate-heavy runs from starving the event loop;
- an offscreen real-widget interaction regression now drives inspection
  selection, Grid/Axes toggles, guarded acceptance, asynchronous candidate
  generation, preview startup/cancellation, close blocking during active work,
  and safe close after the worker reaches a terminal boundary.
- direct project-store and CLI regression coverage for no-create read-only
  behavior, plus an event-loop test for asynchronous load/analyze, session
  handoff, decision-log attachment, and final emission.

The first UX slice exit criteria are now covered. The next slice is the typed,
revision-aware LLM proposal and explanation surface described in sections 8
through 12; it must reuse these tested human controls rather than introduce a
parallel mutation path.

Initial agent-UX slice implemented on 2026-09-03:

- the Qt autonomy queue persistently states Advisory, Assisted, or Delegated
  authority and the corresponding mutation boundary;
- the selected proposal separates escaped agent interpretation from
  authoritative regenerated engine evidence and deterministic policy result;
- evaluated and current revisions are visible, stale proposals are labeled in
  text and cannot enter review, and evidence re-evaluation runs on a worker
  before atomically replacing the displayed queue state;
- the real-widget regression covers fresh/stale transitions, review blocking,
  close-during-re-evaluation, restoration after fresh evidence, and untrusted
  HTML-like rationale rendering.

### Proposed continuation - 2026-09-24

This checkpoint prioritizes the next work using the
[code/product review](../docs/PRODUCT_AND_LLM_REVIEW_2026-09-24.md) and the
[BRepGraph assessment](../docs/OCCT_REVIEW_2026-09-10.md). It supplements this
roadmap and the existing geometry/progress plans; completed capabilities should
be reused. The sequence below is proposed, not a claim of implementation.

**First product milestone.** An operator opens an analytic or mostly analytic
STEP assembly, declares its conversion policy, obtains deterministic exact
progress, resolves remaining decisions directly or through an optional LLM,
and exports validated CSG with a reproducible decision history. Closing and
reopening the project must preserve decisions and their authority. Unsupported
geometry must yield explicit residuals and a usable partial project.

The assumed initial application remains the repository's neutronics geometry
workflow. Arbitrary free-form CAD and complete transport-deck authoring are
outside this milestone. Approximation is optional and requires explicit user
policy. A particular LLM provider, model size, or OCCT 8.1 migration is not a
prerequisite.

| Stage | Deliverable | Acceptance gate |
|---|---|---|
| 1. Correctness and restart | Trustworthy fit evidence, source-bound queues, consistent reconstruction, controlled kernel ownership | The reproduced review failures become passing regressions; existing behavior remains covered. |
| 2. Shared task and policy contract | One typed operation path used by human controls, autonomy, and restart | No duplicate mutation or budget charge; stale authority is rejected; each supported operation has a recovery path. |
| 3. BRepGraph experiment | A read-only context/identity adapter with a measured adoption decision | Correct owner/occurrence mapping, preserved metadata, understood invalidation, and acceptable measured cost. |
| 4. Complete human workflow | Focused contexts, actionable decisions, durable partial results, final verification/export | Representative sessions complete or stop usefully and resume without losing intent. |
| 5. Bounded LLM integration | Provider-independent planner using the same contexts, operations, and review surface | Real-model runs obey the contract, stop on uncertainty, and can be reproduced from recorded inputs/results. |
| 6. Product qualification | Comparative evaluation and a declared supported envelope | Demonstrated usefulness and preservation on held-out cases; failures and resource limits remain explicit. |

**Stage 1: repair the evidence and persistence boundary.**

First preserve the known 70-test baseline and promote the temporary review
probes into permanent regressions. Record the current source/dependency identity
and dirty state; do not treat a development build as a performance baseline.
Establish an optimized benchmark configuration separately, keeping test
assertions active in the test configuration.

Deliver three small groups of changes:

1. Separate analytic equivalence, sampled residual, bounded deviation, and
   unknown evidence. Prevent sampling alone from granting exact automatic
   surface-replacement authority. Correct downstream assumptions that rely on
   this flag. Start by failing closed where proof is absent; add recognizers or
   conservative bounds in follow-up changes rather than delaying the fix.
2. Bind queues and approvals to source contents, preparation/units policy,
   effective operation parameters, and candidate identity. Correct overlap
   reconstruction and align budgeted-surface/fixed-point reconstruction with
   execution. Version the persisted contract. Legacy queues without adequate
   identity require revalidation, rather than silently inheriting authority.
3. Establish a consistent non-destructive Boolean execution path before
   relying on shared-shape snapshots. Eliminate duplicate Boolean execution
   and close temporary XCAF documents on all exits. Audit healing, meshing,
   and other mutators separately; Boolean settings alone are insufficient.

Acceptance includes the missed spline ridge, changed-file/same-path restart,
every supported operation's persist/restart path, cumulative budget
reconstruction, and before/after preservation of input geometry on failed or
cancelled operations. A source change must return a specific revalidation
requirement. An operation without sufficient geometric evidence must remain
uncommitted even if a user dismisses a warning.

Implementation checkpoint, 2026-09-24:

- sampled spline residuals are now identified as sampled evidence and cannot
  grant exact authority; a narrow-ridge regression demonstrates the missed
  feature that motivated the change;
- autonomy queue schema 3 records the STEP content, authority-policy, and
  conversion-policy identities and refuses legacy unbound queues, edited
  policy/preparation data, and changed content at the same path;
- unit assertion/normalization and healing preparation are restored on resume,
  while conflicting resume options require fresh evaluation; budgeted shatter
  persists and reuses its normalized effective depth;
- restart reconstruction now covers budgeted surface replacement, overlap
  resolution, and the effective fixed-point policy used during execution;
- binary OCCT Booleans use one shared path that applies non-destructive and
  fuzzy settings before a single build, and splitters are non-destructive;
- temporary XCAF import/export documents are closed on every return path, with
  a regression against the application's retained-document count.

The assertion-enabled Debug build passes all 73 tests. CMake now preserves the
assert-based test oracles in optimized builds as well; the focused Release
evidence, approximation, and XCAF-lifetime tests pass. Existing callback,
preview, approximation, overlap, and artifact tests cover rollback or durable
last-certified-state behavior on failure and cancellation. Stage 1 is complete;
Stage 2 begins with an inventory of the operation registry's argument,
preview, execution, recovery, and replay support.

**Stage 2: make one task contract authoritative.**

Extend the existing operation registry and session API. Avoid building another
general execution framework. Inventory each operation's argument schema,
policy checks, preview, execution, restart, and human recovery support.
Expose only the capabilities actually supported by the selected frontend.

Create a versioned project policy containing units, modeled domain, approved
materials, protected components/interfaces, approximation budgets, resource
limits, and authority mode. Distinguish global restrictions from operation
permissions, and enforce applicable restrictions across all mutation routes.
Natural-language interpretation may propose policy, but may not expand it.

Bind requests to project/source identity, expected revision, policy digest,
candidate evidence, and a unique request ID. Require strict schemas and reject
duplicate IDs, malformed actions, unknown fields where they change meaning,
and nonfinite numeric limits. Keep policy/approval storage outside the
planner's writable scope; retain Lua as trusted scripting.

Use one operation implementation for apply and reconstruction, with explicit
replay mode and recorded effective parameters. Make retries idempotent and
recover interrupted commits without duplicating geometry, audit entries, or
ledger charges. Reuse the project database, artifact store, and worker
supervisor. Add failure injection around persistence and commit boundaries.

Distinguish task results: complete, partial, awaiting decision, failed, and
cancelled. A completed proposal queue must not imply conversion completion.
Resume under a changed policy must explicitly invalidate affected approvals.

Acceptance: CLI and Qt agree on disposition and outcome for the same request;
stale requests cannot commit; retry and crash recovery preserve one logical
operation; every advertised review action either has a working adapter or an
explicit source-edit/alternative/suspend route.

Implementation checkpoint, 2026-09-24:

- the operation registry now declares the autonomy argument schema, Qt review
  support, restart support, and recovery action for each exposed operation;
- the LLM context advertises only the eight mutations with a deterministic
  autonomy evaluator and restart path, rather than every registered mutation;
- registered engine mutations outside that contract are denied with a specific
  policy rule instead of entering an unusable human-review queue;
- a registry invariant test prevents duplicate names, missing schemas/recovery
  routes, duplicate typed operation kinds, or autonomy operations without
  restart support;
- live autonomy and restart reconstruction now call the same typed operation
  dispatcher; the budgeted-shatter regression verifies replay retains its
  normalized depth and approximation policy;
- evaluation and execution dispatch by the registry's typed operation kind,
  removing the parallel operation-name switch as a second source of truth;
- the action protocol now requires its schema/version and nonempty action list,
  requires unique nonempty action IDs, rejects duplicate JSON keys and unknown
  fields, and validates required or forbidden target, candidate, parameter, and
  contained-medium payload fields before queue creation;
- a focused request-contract test covers valid normalized arguments and the
  malformed or ambiguous payload classes above.

The assertion-enabled Debug build now passes all 74 tests after this contract
slice.

Identity and retry checkpoint, 2026-09-24:

- action schema 2 requires a stable request ID, the context source digest, and
  the expected loaded-document revision;
- the context publishes that source/revision pair and the exact action-contract
  version required from a planner;
- queue schema 4 binds the request ID, source identity, expected revision, and
  normalized action payloads with a deterministic digest;
- queue reads reject legacy schemas and edited action payloads, while execution
  rejects wrong-source and stale-revision requests before mutation;
- resume retains the request ID/digest and terminal results, reconstructs
  already-applied geometry without duplicating decision-log operations, and
  verifies pending items against the reconstructed revision;
- contract and CLI regressions cover digest sensitivity, wrong-source/stale
  requests, edited queues, legacy queues, and one-result resume behavior.

Project idempotency and recovery checkpoint, 2026-09-24:

- project schema 7 adds an autonomy-request index keyed by external request ID,
  with submitted payload, source, authority-policy, and conversion-policy
  identities;
- reservation uses SQLite uniqueness to distinguish first execution, exact
  retry, and conflicting ID reuse before CAD is loaded or mutated;
- queue publication, artifact indexing, and transition to awaiting, completed,
  or failed state occur in one database transaction;
- `autonomy --project DIR` returns the immutable stored queue for exact retries
  without loading CAD or appending a second decision-log operation;
- explicit resume advances an awaiting project request while retaining its
  original database identity, and project status reports the request count;
- running reservations have owner-bound, five-minute leases renewed every 30
  seconds during active CLI work; exact retries can reclaim an expired
  reservation, and an obsolete owner cannot publish after ownership changes;
- database and CLI regressions cover persistence across reopen, conflicting ID
  reuse, pending and completed retries, absence of duplicate log creation,
  lease recovery, stale-owner rejection, and rollback when the SQLite
  checkpoint transition is interrupted.
- proposal completion is separate from caller-specific output delivery; an
  exact completed retry reconstructs the immutable queue when STEP or MCNP
  output is requested, and an export failure leaves that queue available for a
  later attempt without duplicating its decision log or request row.
- decision-log paths must be regular files, manifest and append writes are
  checked, and a pre-checkpoint execution failure rolls back request ownership
  immediately so a fresh request can retry or a claimed review returns to its
  preceding state.

The next Stage 2 slice should define which immutable artifact orphans are
retained, collected, or reported for operator recovery.

**Stage 3: qualify BRepGraph for context and identity.**

Run a bounded experiment after the ownership fixes, while Stage 2 contracts
are being stabilized. Start with the locally available interfaces and hide
them behind an application adapter. Use an independently pinned experimental
build when evaluating the OCCT 8.1 direction. Record the actual API/build used;
do not couple product delivery to an anticipated release.

Prototype one repeated definition with several placed occurrences. Provide
owner lookup for a selected face, topology adjacency, occurrence paths, and
application-ID bindings through public builder/view APIs. Keep the session
authoritative and the graph derived in this first step. Querying or rebuilding
the graph must not mutate source geometry or change approval state.

Measure graph construction, peak memory, cold/warm query latency, and the
cost of context refresh against current TopExp maps and definition reuse.
Qualify one later mutation and persist/reload scenario, including split,
merge, deletion, transforms, metadata, and invalidation of external Boolean
results. Preserve source digests across this work; graph identity alone does
not establish correspondence after a new STEP import.

Adopt it initially only for proven context/identity uses. If cost or API
stability is unsuitable, keep the same query interface backed by existing
maps. Topology adjacency, geometric contact, physical containment, and
semantic importance must remain distinct data with their own evidence.
Any future move to graph-authoritative geometry needs a separate migration
decision; two independently mutable authoritative models are not acceptable.

**Stage 4: finish one coherent human workflow and its context API.**

Build focused, paginated context on the Stage 2 contract, using Stage 3's
adapter where qualified. This work must remain deliverable without BRepGraph.
For each selected face, solid, or residual include the owner/occurrence,
relevant neighbours, materials and provenance, protected status, blocker,
remaining budgets, applicable candidates, and relevant rejection history.
Expose argument schemas, explicit size limits, and truncation information.

Finish the path from inspection to proposal, preview, decision, job progress,
verified result, export, close, and resume. Reuse the current Qt worker,
progress, and decision controls. Show clearly whether the reviewer is deciding
a permitted tradeoff, resolving missing intent, or facing insufficient
geometric evidence. Provide grouped review for repeated components with
per-occurrence checks and exceptions. Preserve rejection reasons as reusable
constraints where the user makes them general.

Acceptance scenarios: an exact assembly needs no discretionary approval;
protected geometry remains protected; a permitted approximation is charged
and explained; ambiguous overlap ownership asks a specific question; an
unsupported residual becomes a resumable partial result. Exercise real
display interaction as well as offscreen tests. Final artifacts identify the
geometry, policy, changes, unresolved state, and validation evidence.

**Stage 5: connect an actual planner with bounded authority.**

Inspect the existing `pi_csgify` extension described in the ITER plan before
adding a connector; its external implementation was not qualified in the
September 24 review. Reuse or adapt the typed transport if it satisfies Stage
2, and reuse the asynchronous tool design in the progress plan. Keep protocol
correctness independent of any particular provider or harness.

Allow the planner to retrieve focused context, choose a candidate, request
clarification, and stop. Deterministic exact processing runs before model
intervention. Add schema-constrained output where supported, application-side
validation, bounded retries, loop/stagnation detection, and per-run time/tool
budgets. Supervise expensive geometry jobs with the existing process limits.
Persist model/configuration identity, context, requests, tool results, and
decision provenance. Imported text never grants authority.

Develop first with a scripted planner to test the full interaction, then run
real candidate models on a fixed decision set. Include hostile or misleading
metadata, stale candidates, repeated requests, unavailable inference, budget
exhaustion, and a user changing policy during a run. Start with assisted
operation; enable delegated operation for the same qualified capabilities
under predeclared policy. Preserve headless suspend/resume and human handoff.

Model selection follows measured decision quality, latency, memory, license,
and deployment requirements. The local hardware assessment belongs here; no
model download, provider account, or specific parameter count is assumed.

**Stage 6: evaluate the product and qualify its supported envelope.**

Choose the corpus at the start, adding small adversarial fixtures and a small
set of representative user assemblies to the existing benchmark cases.
Retain a held-out subset. Compare deterministic conversion, human-assisted
conversion, a small local planner, and a stronger planner where available,
using identical geometry rules and budgets. Repeat enough runs to expose
planner variability and record the machine/build configuration.

Measure accepted completion, unresolved blockers, human active time, number
of decisions, protected-feature/material preservation, geometric change,
CSG cells/surfaces/expression complexity, wall time, peak memory, and restart
reproducibility. Validate output with independent checks in the intended
consumer where available. Until that validation runs, describe the result as
geometry-qualified rather than simulation-qualified.

The release gate requires no policy bypass or incorrect automatic approval
in the qualification cases; correct reject/partial behavior; restart/crash
recovery; and documented resource limits. This evidence does not establish
universal correctness. Select the smallest planner that shows useful gains
on held-out cases within the deployment budget. If none improves the baseline,
retain the deterministic/human product and revisit the model's assigned task.

**Execution order and immediate work.**

Stage 1 is the first implementation milestone. Stage 2 follows its identity
and ownership repairs. The read-only BRepGraph experiment can overlap contract
design, but must not block Stage 4. Stage 5 builds on the working human
workflow. Evaluation design starts immediately and qualification continues
through every stage. Existing ITER-scale qualification remains a later gate
for an ITER-scale claim, rather than a prerequisite for a smaller useful
release.

The first three reviewable changes should be: (1) permanent spline regression
and corrected evidence/automatic-approval behavior; (2) source-bound durable
queues with changed-source/legacy-queue tests; (3) execution/reconstruction
parity with an operation-by-operation restart matrix. Kernel ownership and
lifetime hardening follow as the remaining Stage 1 work before expanded
concurrency or graph mutation.

Keep implementation status in these checkpoints with links to actual changes
and validation results. Do not infer that an operation is product-complete
because an earlier technical phase is marked implemented.

## 1. Product position

csgify should present one workflow:

```text
Open model -> understand readiness -> review recommended work
           -> approve bounded changes -> verify -> export
```

An LLM may accelerate that workflow by interpreting intent, explaining
evidence, and producing structured proposals. It must not create geometric
truth, invent certification, or bypass the same review and mutation paths used
by a human.

The governing interaction invariant is:

```text
Human supplies intent and authority.
LLM plans and explains within that authority.
Engine proposes and certifies geometry.
Human or deterministic policy approves.
Session mutates atomically.
Engine verifies.
Project records the result.
```

The application must remain fully useful without an LLM process, network
connection, or chat interface.

## 2. Success outcomes

The implementation is successful when:

1. A first-time user can open a model and identify its readiness and next safe
   action without reading project documentation.
2. Direct human actions and LLM-initiated actions use the same proposal,
   preview, approval, job, verification, undo, and audit machinery.
3. The Qt event loop remains responsive during every import, analysis,
   proposal, mutation, verification, and export that can take noticeable time.
4. Every mutation communicates its exact scope before execution and its
   verified outcome afterward.
5. An LLM receives bounded, typed, revision-aware context rather than a dump of
   the entire model or implicit GUI state.
6. An LLM cannot expand its authority, raise approximation budgets, impersonate
   a human, apply stale work, or retry a mutation twice.
7. The primary workflow is usable with keyboard navigation, high-DPI displays,
   non-default themes, and without relying on color alone.
8. UI behavior and agent behavior are covered by scenario tests rather than
   only construction and serialization tests.

## 3. Users and primary journeys

### 3.1 CAD operator

Needs to inspect a model, understand why it is not emit-ready, compare
proposals visually, approve exact changes, and export with confidence.

Primary journey:

```text
Open project -> readiness summary -> select blocker -> preview recommendation
             -> approve -> watch progress -> verify change -> export
```

### 3.2 Domain reviewer

Needs evidence, provenance, protected-component policy, approximation budgets,
overlap ownership, and a durable record. This user may reject a geometrically
valid change because it is semantically wrong.

### 3.3 Automation developer

Needs versioned schemas, stable identifiers, predictable status codes,
idempotent actions, asynchronous jobs, bounded pagination, and replayable
artifacts.

### 3.4 LLM-assisted operator

Expresses an objective such as:

```text
Make this emit-ready. Ask before removing components or using approximation.
```

The LLM may inspect and propose immediately, but the UI must clearly show the
objective, active authority, operations performed, pending decisions, and
verified terminal result.

## 4. UX principles and release rules

### 4.1 UX is part of feature completeness

A user-facing operation is not complete until it has:

- a discoverable entry point;
- language that states the target and effect;
- preconditions and unavailable-state explanations;
- truthful busy/progress/cancellation behavior;
- an exact-scope confirmation for destructive changes;
- success, partial, cancelled, stale, and failure presentations;
- undo or documented recovery where the operation permits it;
- keyboard and assistive-technology metadata;
- human-facing and machine-facing tests.

### 4.2 Progressive disclosure

Show the decision first and technical proof second:

1. readiness and recommended next action;
2. affected components and expected impact;
3. preview and blockers;
4. expanded certificate, raw metrics, IDs, and provenance.

Stable IDs and message codes remain visible and copyable, but friendly names,
hierarchy, and translated explanations are primary.

### 4.3 Honest state

- Never display a percentage without a real total.
- Never label provisional geometry as applied or certified.
- Never hide that a proposal became stale.
- Never convert a review acknowledgement into geometric authorization.
- Never show an enabled action that can only fail because required input is
  absent; disable it and explain why.
- Read-only actions must not initialize or mutate persistent project state.

### 4.4 One action, one visible result

Action labels include scope, for example:

```text
Remove 3 components (11 solids)...
Apply 7 certified changes...
Export verified MCNP...
```

After completion, the same surface reports what changed, the resulting
revision, verification result, artifact links, and Undo when available.

## 5. Target information architecture

The project workspace becomes the primary Qt shell. Standalone review remains
supported but uses the same panels and models.

```text
+--------------------------------------------------------------------+
| Project | readiness | revision | active authority | global actions |
+------------------+-------------------------------+-----------------+
| Model navigator  | 3D viewport                   | Inspector       |
| hierarchy/search | selection and overlays        | Summary         |
| status/filter    | before / proposed / verified  | Evidence        |
|                  |                               | Actions         |
+------------------+-------------------------------+-----------------+
| Review Inbox | Activity & Jobs | Agent | Diagnostics | Artifacts   |
+--------------------------------------------------------------------+
```

The shell owns navigation and shared state. Coverage, autonomy, removal, and
decomposition should normally open as workspace pages or docks rather than
independent top-level windows.

### 5.1 Project header

Always show:

- project/model name;
- source unit and document revision;
- readiness: `Ready`, `Needs review`, `Blocked`, `Working`, or `Failed`;
- blocker and pending-review counts;
- active authority mode: `Advisory`, `Assisted`, or `Delegated`;
- primary next action.

Example:

```text
Not emit-ready - 3 blockers, 2 proposals waiting for review
[Review proposals] [Explain blockers]
```

### 5.2 Review Inbox

Use one typed inbox for removal, decomposition, surface replacement, hole fill,
contained-medium, approximation, and coverage-correction proposals. Each item
shows:

- friendly target name and hierarchy, with stable ID secondary;
- source: human, rule, or LLM actor;
- proposed operation and exact affected target count;
- confidence and whether the engine certified the preview;
- expected volume/topology impact;
- blockers and policy requirements;
- proposal revision and stale state;
- `Preview`, `Approve`, and `Reject` actions when valid.

Coverage findings that cannot be approved expose only corrective actions.

### 5.3 Activity and jobs

All expensive work appears in one activity model regardless of initiator.
Rows show human titles, target, phase, real progress, duration, outcome, and
initiator. Detail shows a grouped event timeline, diagnostics, revision
transitions, produced artifacts, cancellation, retry, and recovery.

### 5.4 Agent surface

The agent area contains three separate concepts:

1. **Objective:** editable natural-language goal and constraints.
2. **Plan/activity:** structured steps and tool results.
3. **Conversation:** explanations and refinements.

The transcript is not the system of record. Proposals, approvals, jobs,
artifacts, and revisions remain first-class project objects.

## 6. Human-agent contract

### 6.1 Authority modes

#### Advisory

The agent may inspect, search, explain, compare, and create no executable queue
items. It cannot mutate or grant itself more access.

#### Assisted

The agent may create proposals and launch read-only previews. Every mutation
requires an explicit human decision in the Review Inbox.

#### Delegated

The agent may submit actions for deterministic auto-approval only inside a
human-created authority grant. Example:

```json
{
  "mode": "delegated",
  "document_id": "doc:1",
  "base_revision": 17,
  "allowed_operations": ["apply_decomposition_candidate"],
  "target_scope": ["assembly:/vessel/supports"],
  "excluded_component_ids": ["inst:detector", "inst:shield"],
  "allow_approximation": false,
  "allow_removal": false,
  "max_actions": 10,
  "expires_at": "2026-09-03T18:00:00Z",
  "nonce": "..."
}
```

The LLM cannot author or alter the grant. Every request is re-evaluated from
fresh engine evidence and the grant is checked again immediately before
commit.

### 6.2 Task-scoped context

Add a context builder that accepts an objective, selection, scope, pagination,
and token/record budget. Its versioned response contains:

- source digest, document ID, revision, units, and readiness;
- selected entities with friendly names, hierarchy, and stable IDs;
- blockers relevant to the selected scope;
- pending decisions and running jobs;
- recent applied/rejected actions;
- immutable user policy and authority summary;
- operations allowed in the current state;
- artifact references rather than embedded large payloads;
- a truncation flag and continuation cursor.

Imported names, metadata, diagnostics, artifact text, and user-authored CAD
properties are untrusted data. They must be delimited as data and never merged
into system or tool instructions.

### 6.3 Typed asynchronous tools

The initial LLM adapter exposes a deliberately small vocabulary:

```text
cad_get_state
cad_inspect
cad_create_proposal
cad_explain_proposal
cad_start_preview
cad_submit_decision
cad_start_apply
cad_start_verify
cad_start_export
cad_get_job
cad_get_job_events
cad_get_artifact
cad_cancel_job
```

Long operations return a job ID immediately. The adapter never holds one tool
call open for the duration of an OCCT operation.

Every mutation request includes:

- schema version;
- request/idempotency key;
- actor ID;
- authority grant or human approval ID;
- source digest and base revision;
- proposal ID and exact target set;
- operation and policy digest.

The adapter returns typed states rather than requiring an LLM to infer them
from prose:

```text
succeeded | failed | running | cancelling | cancelled
awaiting_human | denied | stale | superseded | no_safe_action
```

### 6.4 Approval handoff

An LLM proposal sent to Qt becomes an immutable review item. The approval is
bound to:

- project/document and source digest;
- base revision;
- proposal ID and exact stable target set;
- operation parameters and policy digest;
- preview/certificate digest;
- approving identity, timestamp, expiry, and nonce.

If any bound value changes, the item becomes visibly stale and must be
regenerated. Approval cannot silently transfer to a similar proposal.

### 6.5 Explanations

The agent may summarize engine evidence, but the UI distinguishes:

- **Engine evidence:** deterministic facts and certificate results.
- **Policy result:** allowed, review required, or denied.
- **Agent explanation:** model-generated interpretation.

The agent must cite proposal, diagnostic, and artifact IDs in its structured
response. Qt links those references to the authoritative evidence.

## 7. Implementation workstreams

### Workstream A - Immediate trust repairs

1. Add explicit `project init`; make `project status`, `events`, and other
   read-only verbs fail without an existing valid project.
2. Fix duplicated mutation-contract help text and stale Qt removal help.
3. Replace unmodified mutation shortcuts such as `A` with deliberate shortcuts
   such as `Ctrl+Enter`; expose shortcuts on controls.
4. Change generic labels to scope-aware labels with counts.
5. Confirm multi-target, removal, approximation, and delegated mutations with a
   concise impact summary.
6. Attach the project decision log automatically when the workspace promises
   audit logging, or remove/disable the misleading field until configured.
7. Disable unavailable workspace actions and show the missing prerequisite in
   a tooltip/status explanation.
8. Log visible warnings when a proposal overlay or artifact capture fails;
   never silently interpret a missing overlay as an empty proposal.

**Exit:** no read-only command creates state; every enabled mutation states its
scope; accidental single-key mutation is impossible; project audit behavior
matches the UI.

### Workstream B - Responsive operation pipeline

Move these operations behind `CadJobController` and the shared activity model:

1. STEP import and initial analysis;
2. proposal generation and preview;
3. autonomy re-evaluation;
4. applying one or many approved actions;
5. terminal verification;
6. STEP export and CSG/MCNP emission;
7. canonical image/artifact capture where it can block.

Add an application-level task coordinator rather than one controller per
window. Enforce one session writer, allow compatible read-only inspection, and
queue or reject conflicting work with an explanation.

Closing a workspace with active work presents the actual state and choices:

- keep working in the background;
- request safe cancellation and close afterward;
- return to the workspace.

**Exit:** no supported large-model workflow blocks the GUI event loop; every
opaque phase emits a heartbeat; cancellation and close behavior are explicit.

### Workstream C - Context-specific decision review

Split the current broad decision panel into a shared shell plus typed review
presenters:

```text
DecisionReviewShell
  RemovalReviewPresenter
  DecompositionReviewPresenter
  SurfaceReviewPresenter
  HoleFillReviewPresenter
  ContainedMediumReviewPresenter
  CoverageCorrectionPresenter
```

The shell supplies before/proposed/verified states, evidence disclosure,
approval identity, revision state, history, and job status. Each presenter
supplies operation-specific controls and vocabulary. Inspection mode contains
no accept/reject controls.

Add:

- friendly component path and semantic role before stable ID;
- a clear `Exit focus` action when selection changes the viewport context;
- complete visual legend derived from the active highlight roles;
- standard views, orientation widget, and concise mouse/keyboard help;
- an expandable technical-evidence section;
- an outcome banner with revision, verification, artifact, and Undo.

**Exit:** users never see irrelevant operation controls, and the primary
decision can be understood without reading raw certificate fields.

### Workstream D - Unified project workspace

1. Add the project readiness header and primary next action.
2. Embed the Review Inbox, Activity/Jobs, Coverage, Agent, Diagnostics, and
   Artifacts as workspace pages/docks.
3. Expose `Find removable components` from an assembly or current selection.
4. Add task entry points such as `Explain blockers`, `Make emit-ready`,
   `Decompose selected`, `Check overlaps`, and `Export verified result`.
5. Preserve selection, focus mode, open review item, and dock layout across a
   workspace session.
6. Add live job updates, filtering, sorting, and operation/state grouping.
7. Turn artifacts into actionable records with Preview, Open, Reveal, Copy ID,
   and Verify digest actions.
8. Replace raw event message boxes with a grouped timeline and linked
   diagnostics/artifacts.

Focused LOD must be described accurately: it limits presentation creation but
does not imply random-access STEP import.

**Exit:** the main workflow can be completed without managing a chain of
top-level windows or knowing which subsystem owns a finding.

### Workstream E - CLI and language coherence

1. Make `inspect`, `propose`, `apply`, `review`, `run`, `project`, and `help`
   the primary documented grammar.
2. Move compatibility verbs to a clearly marked legacy help section and emit
   a structured deprecation diagnostic.
3. Normalize argument order, stdout/stderr behavior, exit-state conventions,
   and mutation-contract wording.
4. Use friendly human output by default; keep stable codes and full metrics in
   JSON.
5. Ensure every CLI operation maps to the same operation registry and typed
   state used by Qt and the LLM adapter.
6. Document exit states including `awaiting_human`, stale work, safe partial
   progress, and failed verification.

**Exit:** a human or model can select the correct command without choosing
among synonymous verbs, and machine output never requires parsing prose.

### Workstream F - Agent coordinator and MCP adapter

1. Implement the task-scoped context builder with pagination and truncation.
2. Implement authority grants and immutable revision-bound approvals.
3. Add idempotency storage and duplicate-request result replay.
4. Implement the asynchronous local MCP adapter over project jobs.
5. Add strict input limits, schema validation, target resolution, and actor
   separation.
6. Recompute all proposal evidence; ignore LLM-supplied confidence or metrics.
7. Return artifact references and bounded event pages.
8. Keep the model provider outside engine and project-store types.

The first adapter is tested without any model dependency. A DeepSeek or other
model sidecar is optional and connects only after protocol qualification.

**Exit:** an adapter client can inspect, propose, submit an approval, start a
job, observe the same progress as Qt, cancel it, and receive the identical
terminal revision and artifacts.

### Workstream G - Agent experience in Qt

1. Add the objective/plan/conversation surface.
2. Show active mode and authority grant persistently.
3. Render tool calls as human task statements, with expandable raw payloads.
4. Put every proposed mutation in the ordinary Review Inbox.
5. Add `Pause agent`, `Stop current job`, and `End delegated authority` actions.
6. Make waiting states explicit: waiting for engine, user, policy, or external
   model.
7. Allow the user to refine scope from the current selection without exposing
   hidden viewer state to the model.
8. On reconnect/restart, reconstruct the activity view from durable jobs and
   review items rather than asking the model to remember it.

**Exit:** a user can always answer: what is the agent trying to do, what did it
do, what is it waiting for, what can it change, and how do I stop it?

### Workstream H - Accessibility and visual system

1. Define semantic visual tokens for selection, proposal, active work,
   provisional, certified, warning, blocker, protected, and stale states.
2. Support those states with text/icons/pattern or outline, not color alone.
3. Verify contrast in light, dark, and high-contrast palettes.
4. Add accessible names, descriptions, state announcements, and relationships
   for controls, tables, viewport, progress, and proposal evidence.
5. Define explicit focus order and keyboard flows for model navigation, review,
   confirmation, cancellation, and artifact access.
6. Avoid global unmodified shortcuts while text input or an agent prompt has
   focus.
7. Test common DPI scales and reduced viewport sizes; allow the evidence panel
   and bottom workspace to collapse without hiding the primary action.
8. Provide a non-3D textual selection and evidence path for users who cannot
   rely on viewport color or fine pointer input.

**Exit:** all primary journeys are keyboard-completable, state is not encoded
only by color, and assistive technology receives meaningful names and updates.

### Workstream I - Documentation and onboarding

1. Replace phase-era README language with a current capability statement.
2. Add a five-minute human quick start and a five-minute automation quick
   start.
3. Publish a Qt/CLI/Lua/MCP capability matrix that distinguishes implemented,
   experimental, planned, and legacy features.
4. Document the proposal/certificate/approval/revision model with one end-to-
   end example.
5. Include screenshots of readiness, review, progress, cancellation, stale
   proposal, failure, and verified completion states.
6. Explain authority modes and the fact that an LLM is never a geometry or
   certification authority.

**Exit:** documentation describes the current product rather than a mixture of
historical milestones and future design.

## 8. Delivery phases

### Phase 0 - Baseline and UX instrumentation

- Capture the current primary journeys with fixtures of small, blocked, and
  large assemblies.
- Record task completion, clicks/window transitions, UI stalls, cancellation
  latency, and recurring error codes in test runs.
- Create the semantic state vocabulary and shared human-message catalog.
- Add feature status labels to documentation.

**Gate:** baseline scenarios and measurable UX budgets exist before redesign.

### Phase 1 - Trust and clarity

Implement Workstream A, the first CLI cleanup items, dynamic action labels,
contextual disabled-state explanations, and the first accessibility metadata.

**Gate:** no surprising persistence, misleading audit field, accidental
single-key mutation, duplicated help contract, or ambiguous batch apply label.

### Phase 2 - Responsiveness and one activity model

Implement Workstream B and upgrade Jobs/Artifacts presentation. Reuse the
existing progress/event contract; do not introduce a parallel agent progress
schema.

**Gate:** import, analyze, propose, apply, verify, and export remain visibly
responsive on the representative large assembly.

### Phase 3 - Decision and workspace redesign

Implement context-specific review presenters, readiness header, Review Inbox,
integrated coverage and activity, viewport navigation help, and direct removal
planning.

**Gate:** the CAD operator primary journey completes inside one workspace with
no irrelevant controls and no documentation lookup.

### Phase 4 - Agent protocol

Implement context scoping, authority grants, approval objects, idempotency, and
the local asynchronous MCP adapter.

**Gate:** deterministic protocol tests pass with no external model and Qt and
the adapter observe identical job/revision state.

### Phase 5 - Agent UX

Implement the Qt objective/plan/conversation surface and connect an optional
model sidecar. Start in Advisory and Assisted modes; enable Delegated mode only
after its safety evaluation passes.

**Gate:** a human can inspect and stop every agent action, and no proposal can
mutate outside a visible current authority grant.

### Phase 6 - Accessibility, qualification, and rollout

Complete accessibility, visual regression, performance, recovery, adversarial
agent, and documentation work. Remove legacy UI paths only after equivalent
journeys pass.

**Gate:** all release acceptance criteria in section 11 pass.

## 9. Testing and evaluation

### 9.1 Qt interaction tests

Use QtTest for behavior, not only offscreen construction:

- action visibility and enablement for each review type;
- complete keyboard journey and focus order;
- safe shortcut handling while text fields have focus;
- exact-scope confirmation content;
- selection/focus/show-all transitions;
- stale proposal invalidation after a revision change;
- progress, cancellation, close-during-job, rollback, and retry;
- partial batch selection and protected-component handling;
- Review Inbox reconciliation after mutation;
- activity and artifact links after restart.

### 9.2 Visual tests

- screenshot baselines for light/dark themes and common DPI scales;
- small and constrained window layouts;
- every semantic highlight role and its legend;
- provisional, certified, stale, blocked, failed, and cancelled states;
- display-capable OCCT capture tests with explicit skips when unavailable.

### 9.3 Accessibility tests

- automated accessible-name and focus traversal checks;
- no critical state communicated only by color;
- screen-reader smoke tests for readiness, proposal, progress, and outcome;
- keyboard-only completion of the primary operator journey.

### 9.4 Agent protocol tests

- unknown or oversized operation payload;
- prompt injection in component names, CAD metadata, and artifacts;
- attempted human impersonation or authority expansion;
- stale revision, changed target set, expired grant, and changed policy;
- duplicated mutation request after timeout;
- partial approval of a batch;
- cancellation races and job restart/recovery;
- fabricated confidence/certificate metrics ignored;
- context truncation with safe pagination;
- model unavailable without loss of project work;
- agent claims success while terminal verification failed.

### 9.5 End-to-end agent evaluations

Evaluate outcomes, not preferred wording:

1. Explain why a selected solid cannot emit without proposing an unsafe fix.
2. Make an L-shaped fixture emit-ready using a certified decomposition.
3. Propose rail removal while preserving a protected component.
4. Ask for approval when removal exceeds delegated authority.
5. Recover when the model changes between proposal and approval.
6. Stop safely when no certified action exists.
7. Resume a durable queue after application restart.
8. Produce a final explanation whose referenced evidence and artifacts exist.

Each evaluation records task success, unsafe-action count, unnecessary approval
count, tool-call count, context size, elapsed time, and final verified revision.

### 9.6 Performance budgets

Set fixture-specific values during Phase 0. At minimum enforce:

- no GUI-thread operation above 100 ms without yielding or job handoff;
- progress repaint at the configured bounded frequency;
- bounded event and transcript rendering independent of job duration;
- prompt/context size bounded by requested budget;
- cancellation visible immediately and honored at the next certified safe
  boundary;
- no full AIS rebuild for per-target progress updates.

## 10. Migration strategy

- Extract reusable models/presenters from `viewer_qt.cpp` incrementally; do not
  rewrite the working OCCT viewport.
- Keep standalone `view` operational while embedding the same review component
  in the project workspace.
- Introduce the unified Review Inbox beside the existing autonomy queue, then
  migrate queue rendering after parity tests.
- Preserve current CLI and Lua behavior during a documented compatibility
  window.
- Store new approval, authority, and idempotency records through schema
  migrations with explicit version readers.
- Keep the existing engine certificates and mutation postconditions as the
  only authority for geometry safety.

Likely extraction targets:

```text
src/qt/workspace_main_window.*
src/qt/readiness_model.*
src/qt/review_inbox_model.*
src/qt/decision_review_shell.*
src/qt/removal_review_presenter.*
src/qt/decomposition_review_presenter.*
src/qt/activity_model.*
src/qt/artifact_model.*
src/qt/agent_controller.*
src/qt/agent_context_builder.*
src/qt/authority_model.*
src/agent/mcp_adapter.*
src/agent/approval_store.*
```

Names are illustrative; prefer boundaries that reduce the current large Qt and
CLI translation units without duplicating session state.

## 11. Release acceptance criteria

The UX-first workspace is ready when:

1. A new user completes open -> diagnose -> review -> apply -> verify -> export
   using visible controls and in-product explanations.
2. The same project supports direct human and LLM-assisted work without
   different mutation or certification paths.
3. Every destructive action shows exact scope, requires appropriate authority,
   and produces a visible verified result or recovery path.
4. No supported long operation freezes Qt or remains visually silent beyond
   the progress heartbeat interval.
5. Read-only commands and views create no persistent state.
6. Review items become stale on revision, target, policy, or certificate
   changes and cannot be applied.
7. LLM retries are idempotent and cannot duplicate a committed operation.
8. Imported data cannot alter agent instructions or authority.
9. The primary journeys work without color, mouse-only interaction, or an LLM.
10. Jobs, decisions, artifacts, and agent explanations resolve to the same
    project revision and authoritative evidence.
11. Qt interaction, accessibility, agent safety, recovery, and representative
    large-assembly performance suites pass.
12. Existing geometry, replay, overlap, approximation, coverage, and emission
    safety tests remain green.

## 12. First implementation slice

Start with a small vertical slice that improves the existing product before
adding the MCP adapter:

1. stop `project status` from creating a project and add `project init`;
2. fix stale/duplicated help and rename batch actions with exact counts;
3. remove unmodified mutation shortcuts;
4. attach the workspace decision log consistently;
5. add a readiness banner and primary `Review blockers` action;
6. hide irrelevant decision controls by review type;
7. move initial STEP load/analyze and final emit to `CadJobController`;
8. add QtTest coverage for the above behavior;
9. update the README quick start and capability status.

This slice addresses the most serious trust and comprehension gaps, exercises
the shared job architecture, and creates the workspace surface that the later
LLM approval handoff will reuse.

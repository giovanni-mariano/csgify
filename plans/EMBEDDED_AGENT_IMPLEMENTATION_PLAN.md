# Embedded agent implementation plan

Status: in progress. The advisory question/answer prototype, durable
conversation storage, and first live provider path are implemented as of
2026-09-28. Assisted mutation runs and full qualification remain.
Created: 2026-09-28.
Product decision: embed the agent workflow in the existing csgify Qt application.

This plan supersedes the external-agent-only delivery choice in Phase 5 of
[FIRST_PRODUCT_PROTOTYPE_PLAN.md](FIRST_PRODUCT_PROTOTYPE_PLAN.md).
The CLI remains supported. An MCP server is not required for this delivery.
Embedding the workflow does not require local inference: the first provider
may be a hosted service configured by the user.

## 1. Delivery outcome and scope

An engineer opens a project, selects geometry, enters an objective such as
“Make this selected body emit-ready,” and starts an Assisted run. The agent
inspects current evidence, requests focused context, and submits a typed
proposal. The engineer reviews it in the existing geometry window. After the
engine commits and verifies the change, the agent receives the result and
continues, asks a question, or stops with a useful partial outcome.

The first release includes:

- Two-way project conversation: user questions, grounded answers, follow-up
  questions and retained conversation history, with or without an active run.
- One configured model provider and a scripted provider for tests.
- Advisory and Assisted modes, with Assisted as the default.
- One active run per project and one in-flight model request per run.
- Read-only context navigation and one proposed mutation per turn.
- Existing autonomy operations, enabled individually after their execution,
  review, and recovery paths pass qualification.
- Persistent turns, questions, approvals, rejections, and execution links.
- Pause, resume, stop, and restoration after application restart.
- Explicit user export through the existing STEP/MCNP controls.

Delegated execution, multiple simultaneous agents, additional providers,
bundled model downloads, arbitrary shell/Lua tools, new geometry algorithms,
and automatic export are subsequent work. The first release does not depend
on them. Advisory mode may inspect and explain but cannot enqueue mutations;
Assisted mode requires explicit approval for every proposed mutation.

## 2. Questions and answers are a primary workflow

The user can ask a question at any time, including before starting a task:

- “What is this component, and what do we actually know about its material?”
- “Why can this model not be exported yet?”
- “Why are you proposing this split? What are the alternatives?”
- “What did the last operation change?”
- “What does this tolerance mean?”
- “What happens if I keep this residual?”

Provide a persistent conversation thread per project, with optional links to a
run and selected target. Follow-ups retain the question's target and evidence;
the composer shows the context attachment and lets the user change it. Asking
“why?” after a proposal refers to that proposal even if the viewport selection
has moved. Persist questions and answers independently of geometry tasks.

An answer may use bounded read-only tools to obtain evidence. It identifies
the relevant target, revision, diagnostics, candidate or job result, using
application-validated references that can select geometry or open evidence.
Distinguish engine measurements, explanatory interpretation, and unknown facts.
For missing material or engineering intent, explain what is missing and ask a
specific follow-up. General explanations need not load geometry, but must not
imply that a specific model has been inspected. V1 answers use model knowledge
and local project context; no external web browsing is required.

Questions do not imply approval, create geometry tasks, or relax policy.
“Why remove this?” explains the proposal. “Proceed with the proposed removal”
opens the existing scoped review/confirmation. Interpret ambiguous requests
through a short clarification; mutation remains a structured, reviewed action.
In Assisted mode the user can converse freely before choosing Start.

When a question arrives during an agent run, pause admission of new run work
at the next safe boundary and prioritize the conversation. Let an executing
geometry operation finish/cancel safely. Answer from an immutable snapshot
with its revision shown, or wait for fresh evidence if the question depends
on the pending result. Cancel or finish any pending model request before
issuing the question's request; never run competing conversational and planning
loops. Resume task planning after the answer when the objective is unchanged;
a changed objective or scope invalidates outstanding proposals and requires
the user to start/resume that updated task explicitly. Answering a question
alone does not invalidate an approval; new instructions or decisions can.

Stream answer text where supported and provide Stop response. Show “Gathering
evidence” while tools execute. Keep full local history while sending bounded
recent messages and relevant summaries to the provider. Explain when a past
answer refers to an older revision. Conversation usage is visible and bounded
even when no task is active; planning run limits remain separately enforceable.

## 3. Existing integration points and required repairs

Source inspected when preparing this plan:

| Existing component | Reuse | Required extension |
|---|---|---|
| `include/csgify/autonomy.hpp`, `src/autonomy.cpp` | Context, typed requests, policy evaluation, common execution dispatcher | In-memory context builder, snapshot binding, capability filtering |
| `src/operation_registry.cpp` | Eight advertised operations and review/restart metadata | Per-operation embedded qualification gate; no duplicate operation vocabulary |
| `src/qt/cad_job_controller.*`, `cad_job_worker.*` | Geometry worker and progress signals | Shared session scheduling for context, evaluation, preview, and mutation |
| `src/viewer_qt.cpp` | Geometry review, queue, project entry, policy and export | Agent dock, review callbacks, coordinated session access and checked persistence |
| `include/csgify/project_store.hpp`, `src/project_store.cpp` | SQLite, immutable artifacts, jobs, autonomy request reservations and leases | Agent run/turn records and durable operation reconciliation |
| `src/main.cpp` | Request binding, retries, queue reconstruction | Extract reusable request lifecycle code from CLI orchestration |
| Journal, recipe, replay code | Source/preparation/version binding and reconstruction | Correlation to agent operations and publication recovery coverage |

Specific gaps must not be mistaken for completed infrastructure:

- `CadJobController::active()` protects one controller; windows can construct
  independent controllers for the same session.
- Queue evidence regeneration has a separate thread, and some queue execution
  and review paths still access Session directly.
- `AutonomyQueueWindow::persist()` writes a queue file without propagating its
  return status. Successful geometry execution is not proof of durable queue
  publication.
- CLI request reservation/retry logic is not yet a shared embedded service.
- Publication compensation is established for project removal, not uniformly
  for every agent operation. Per-operation crash recovery remains a release
  requirement.
- The context writer analyzes and generates candidates while serializing to a
  file. It is read-only with respect to committed geometry but still touches
  session/kernel state and must use the geometry coordinator.
- Context completeness needs explicit treatment for suppressed owners,
  multi-solid occurrences, material assignment, and staged interface decisions.
  Missing information must be represented as unknown, not inferred as absent.

The recorded 75/75 result belongs to the preceding prototype work. It is not
validation of this planned integration.

## 4. Architecture and ownership

```text
Agent dock / geometry review
           |
     AgentController -------- AgentProvider (asynchronous model I/O)
           |
     AgentToolRouter -------- immutable context/results
           |
     ProjectExecutionCoordinator
           |
     Existing geometry workers and autonomy evaluation/dispatch
           |
     Session + decision journal + ProjectStore/artifacts
```

Session remains the geometry authority. The model receives values and returns
structured messages; it never receives a Session pointer, arbitrary file
paths, a database connection, or a general command executor.

Introduce one execution coordinator owned by the project runtime. All Session
access from project, geometry review, agent, preview, undo, and export routes
uses that coordinator. Standalone viewers receive an equivalent owner for
their session. Initially serialize even read queries that touch the kernel;
the UI can inspect immutable snapshots while a job runs. Use existing workers
behind this boundary. Do not hold the coordinator during model network calls
or while waiting for a human answer.

Manual edits remain possible while inference is pending. They invalidate its
snapshot. Capture the run target explicitly; changing selection in the viewer
does not silently retarget the run. “Use current selection” creates a new
context generation.

The controller is a state machine with provider-independent logic. Network I/O
uses asynchronous Qt facilities; completed responses return through queued
signals. It does not block the GUI or execute geometry in a network callback.

## 5. Context and tool protocol

Factor the existing context writer into an in-memory builder plus the existing
CLI file wrapper. Both must emit the same versioned contract and preserve
its byte ceiling, paging, truncation, and provenance. Reuse analysis within a
snapshot where possible; avoid redoing full analysis for each page.

Each request has an application-owned envelope containing:

- Run, turn, request, and context IDs, plus the context digest.
- Source digest, document identity, Session generation and document revision.
- Preparation, authority and conversion policy digests, engine/kernel identity.
- Run scope, effective mode, capability version and decision-history generation.

Persist this envelope before sending a request. Tool results and responses
reference that context ID. Build `AutonomyRequest` identity fields from the
saved envelope, never from the current revision at response arrival. Reject
stale responses if geometry, policy, run scope, session generation, or relevant
human decisions changed. Check again under the coordinator immediately before
evaluation and execution. This also covers undo/reload and late responses after
stop. Regenerate candidates and invalidate approvals after a change.

Expose a small typed protocol:

| Request | Behavior |
|---|---|
| `get_context` | Document/target summary, readiness, paginated solids, occurrences, overlaps, and history |
| `inspect_target` | Face, solid, or piece details, owner/protection semantics and blockers |
| `get_candidates` | Fresh engine-generated candidates for a known target, with explicit paging |
| `submit_proposal` | One existing typed operation and rationale, bound to the current context |
| `answer` | User-visible answer with validated references to the supporting context/evidence |
| `ask_user` | Persist a targeted engineering question and optional suggested answers |
| `stop` | Record the model's explanation and return control to the application |

The first three tools are read-only. Target IDs must resolve in the current
document and run scope. Candidate IDs must belong to the target and snapshot.
Context scope may include relevant neighbours for inspection; modifying one
requires a human scope change. Document-wide operations such as fixed-point
decomposition cannot silently execute inside a selected-body run.

Use a versioned tagged response schema for tool requests, proposals, questions,
explanations, and stop messages. Native provider tool calls may map into it;
strict local parsing remains authoritative. Validate types, unknown fields,
bounded lengths and nesting, finite numbers, IDs, and advertised operations.
Reject a malformed or truncated response as a whole. Streaming explanation
text is display-only; partial arguments never become executable proposals.

CAD names, annotations, and prior model text are data. They cannot grant tools,
change authority, introduce URLs to fetch, or amend protected-material rules.
Render explanations as escaped text and label them separately from engine
measurements. Human answers can clarify intent; policy changes still use the
existing typed policy editor and invalidate old evidence.

## 6. Run lifecycle and bounded progress

```text
Idle → Building context → Waiting for model → Validating response
  ↳ Querying context → Waiting for model
  ↳ Awaiting answer → Building context
  ↳ Awaiting review → Executing → Publishing → Verifying → Building context
  ↳ Completed / Partial / Paused / Stopped / Failed / Recovery required
```

On rejection, store the rationale, invalidate the old decision context, and
return that constraint to the model. On approval, recheck the pinned identity
and applicable policy, regenerate required evidence, and use the existing
Session operation. An approval authorizes only the reviewed action/parameters.
Missing proof cannot be replaced by model confidence or a human acknowledgement.

After execution, publish the durable result before starting another turn.
Refresh geometry, ledger, diagnostics, pending reviews and export readiness.
Queue completion, successful task completion, and MCNP readiness remain
separate values. A model `stop` claiming success triggers the engine check;
the application determines the displayed outcome for the requested scope.

Proposed initial configurable defaults: 12 model requests, 24 read queries,
8 committed actions, 2 transient network retries per request, and 10 minutes
of active run time, excluding human wait. Preserve the current 1 MiB context
ceiling; configure an output cap within the selected provider's supported
limits. Reserve the next request's token allowance before sending it. If
verified pricing is unavailable, enforce token limits and show cost as unknown.
Counters survive retry and restart.

Detect repeated normalized proposals against the same evidence and decisions.
Two repetitions without new evidence or human intent stop with a partial
result. Measure progress using engine blocker/residual/readiness evidence;
request count and revision increases alone are not progress. Read-query and
turn limits also stop an endless inspection loop.

Pause prevents new work after the active operation reaches a safe boundary.
Stop aborts pending model I/O, ignores late replies, requests geometry
cancellation where supported, and preserves any committed result. Neither
pretends to roll back an already committed action. Reaching a time limit during
an OCCT call prevents subsequent work; a worker thread cannot promise a hard
kernel deadline. Closing waits for a safe geometry boundary and saves the run.

## 7. Durable execution and restart

Extend ProjectStore with migrations for:

- `agent_conversations` and `agent_messages`: user/assistant roles, parent
  message, context and evidence references, optional run link, usage and
  interruption state. Conversations exist without an agent run.
- `agent_runs`: objective, scope, mode, provider/model reference, limits,
  counters, status and stop reason.
- `agent_turns`: context/envelope artifacts, normalized provider response,
  usage, request generation and retry state.
- `agent_events`: ordered state changes and links to conversation messages;
  store each question/answer once rather than maintaining two transcripts.
- `agent_operation_links`: turn/proposal → request reservation → job,
  journal operation and published result, including the approval identity.

Use existing immutable artifacts for large payloads. Keep compact indexed
records in SQLite; never store credentials or authorization headers in them.
Persist explicit tool results and user-visible rationale, not private model
reasoning. Summarize history within context limits while retaining rejection
constraints and references to full local artifacts.

Extract CLI reservation and request lifecycle handling into a reusable service.
Use stable application-assigned IDs and the existing conflict/lease rules.
Lease expiry alone is not evidence that a previous worker did not commit.

Publication sequence for each qualified operation:

1. Persist proposal, approval, bound evidence and execution intent; reserve the
   stable request/operation identity before dispatch.
2. Execute through the coordinator and Session with journal correlation.
3. Stage immutable result/evidence artifacts and atomically publish the SQLite
   references and terminal outcome using existing project job primitives.
4. Expose the saved result to the agent and admit the next turn.

Define the journal commit marker and reconciliation rules before enabling
execution. A SQLite transaction cannot atomically include the live Session,
journal and artifact files. For an operation that spans several certified
commits, retain each commit's correlation and partial progress; never assume
one undo reverses the whole operation.

If publication fails, use the operation's qualified compensation or enter
recovery-required state with further edits disabled. Preserve orphan artifacts
for diagnosis. Do not infer committed work from an artifact's existence.

On reopen, reconstruct the existing durable geometry history once, reconcile
operation links and pending intents against journal commits, then restore the
agent run as paused. Never replay a saved proposal merely because a model
response or a queued intent exists. If evidence cannot distinguish committed
from uncommitted work, block that operation for recovery. Resume obtains fresh
context and regenerates review evidence. Unfinished model requests can be
reissued under a new generation; duplicate responses cannot create another
operation or budget charge.

## 8. Workspace and provider configuration

Reuse the design tokens in [GEOMETRY_WORKSPACE_DESIGN.md](../docs/GEOMETRY_WORKSPACE_DESIGN.md):
canvas `#edf2f5`, panel `#ffffff`, text `#203442`, secondary `#526775`, selection
wash `#d8f3f7`, action `#126b80`; Noto Sans with system fallback; 4/8/12/16
spacing. Preserve current cyan/green/orange/magenta geometry meanings.

```text
Assembly             Geometry viewport                Inspector
                     Before / Preview / Applied       Evidence / Alternatives
─────────────────────────────────────────────────────────────────────────────
Conversation: questions, answers, evidence links     Proposal → existing review
Ask about this project… [attached target/revision]  Send / Stop response
Task: objective and pinned scope         Mode       Start / Pause / Stop
Activity, recent result, next step                   History / usage (expand)
```

Use a resizable, collapsible dock with a conversation and compact activity
view. The question composer is usable without an active task. “Ask” and
“Start task” have distinct controls and predictable effects. Proposal
selection highlights the target and opens the established evidence/review
path. Support missing source, unconfigured provider, loading, query progress,
waiting for model, question, stale proposal, rejection, execution, recovery,
partial and verified outcomes. Preserve keyboard acceptance and selection
context. Review can temporarily hide the dock without ending the run.

Add a provider settings dialog for endpoint/provider, model identifier,
credential source, request limits, and a deliberate connection test. Implement
one concrete provider after checking its current official API contract; do not
claim compatibility with every endpoint using a similar JSON format. Use an
asynchronous provider interface with cancellation, timeout, response-size
limits, usage reporting and normalized errors. Retry transient transport
failures only within the run allowance; authentication/schema errors need a
specific repair message.

Use a session-only key initially, with optional environment-based setup for
development. Persistent credentials require an OS credential-store adapter;
there is no plaintext project-file fallback. Show what context is sent to the
chosen endpoint when configuring the connection. Remove unnecessary local
paths and exclude raw STEP/screenshots by default. The provider receives only
the curated context. Provider/model selection is an implementation-time
configuration decision; it does not block the core or scripted workflow.

## 9. Implementation sequence and acceptance gates

| Phase | Deliverable | Exit gate |
|---|---|---|
| 1. Contract and context — implemented | Protocol types for conversation and planning, immutable envelopes, in-memory context, capability/scope filtering and scripted provider | Questions and follow-ups produce grounded read-only answers; stale/invalid/out-of-scope responses cannot enqueue a mutation; CLI context remains compatible |
| 2. Session coordination | Shared project coordinator; route review, query, preview, undo and execution through it | Human edit during inference invalidates response; two windows cannot access the same Session concurrently; UI remains responsive |
| 3. Durable lifecycle | Extract reservations, migrate agent records, journal correlation and publication/restart reconciliation | Interrupted approved action reconstructs once or blocks visibly; no duplicate journal mutation or ledger charge |
| 4. Scripted Qt workflow | Conversation composer/history, agent dock, controller states, existing review adapters, question/rejection loop, stop/resume | Ask → answer → follow-up works without a run; import → select → inspect → propose → review → commit → verify → reopen succeeds without terminal interaction |
| 5. One real provider | Settings, asynchronous transport, credential handling, strict output parsing | Real model completes the same workflow; disconnect/timeout/cancel preserves project state |
| 6. Qualification | Operation matrix, repeated-component and residual cases, native display walkthrough, packaged demo | All advertised embedded operations pass lifecycle tests; recorded real-model runs meet the assisted acceptance cases |

Phase 4 is an engineering demo and must label its provider as simulated.
Phase 5 establishes live integration, not release qualification. Ship the
Assisted prototype after Phase 6 and the inherited human-workflow durability
gates pass. Delegated mode is a separate later gate.

Suggested new files (names may be adjusted to local conventions):

```text
include/csgify/agent.hpp                 protocol, snapshot and run records
src/agent/agent_protocol.*               strict parsing and validation
src/agent/agent_context.*                existing context builder adaptation
src/agent/agent_run.*                    provider-independent state machine
src/autonomy_request_service.*          extracted reservation/lifecycle logic
src/qt/project_execution_coordinator.*  ownership and job serialization
src/qt/agent_controller.*                Qt orchestration and review callbacks
src/qt/agent_provider.*                  asynchronous provider abstraction
src/qt/<provider>_agent_provider.*       first concrete transport
src/qt/agent_run_dock.*                  conversation, objective and activity
src/qt/agent_settings_dialog.*           provider configuration
tests/scripted_agent_provider.*         deterministic replies/failures
```

Extend ProjectStore, job workers, project/queue windows and CMake in place.
Keep transport dependencies out of the engine library. Add optional Qt Network
support for the embedded connection while preserving builds without the Qt
agent feature. Avoid growing a second mutation switch in the controller.

## 10. Verification and demo handoff

Meaningful regression cases:

- User question with no active run, multi-turn follow-up, selection change,
  evidence-link navigation, unknown semantics, general explanation without a
  loaded model, and conversation history after reopen. Questions leave
  geometry revision and policy unchanged.
- Question during inference, pending review and executing geometry; answer
  uses a declared snapshot, only one model request is active, and a revised
  objective cannot reuse the earlier approval.
- Stale revision, changed policy, reload/undo, changed scope, rejection and
  late response after stop; no old approval or response becomes executable.
- Invalid JSON, unexpected operation, invented candidate, oversized response,
  ambiguous material/ownership and misleading CAD text.
- Multiple pages and read queries; correct owner for faces, pieces and
  repeated multi-solid occurrences; explicit unknown/truncated semantics.
- Concurrent human review and agent work; responsive GUI during model and
  geometry jobs; cancellation and close at supported boundaries.
- Crash before execution, after journal commit, after artifact staging and
  before/after SQLite completion; retries and partial fixed-point commits.
- One persist/reopen/ledger test for each of the eight advertised operations.
  Unqualified operations remain explicitly unavailable in embedded runs.
- Provider errors, unavailable credentials, exhausted limits, repeated
  proposals, rejected alternatives and verified useful stopping.

Use a scripted provider and fault injection for CI. Live-model tests are
explicit opt-in evaluations; no routine test should require network access or
spend model credits. Run focused protocol/store/Qt tests per increment and the
full existing suite at integration gates. Inspect the native viewport at
normal and compact window sizes; offscreen Qt tests do not qualify OpenGL.

Record real-model demonstrations for an exact case, a reviewed decomposition,
and an unresolved ownership or unsupported-geometry case. Compare final engine
results, human interventions, retries, token usage and elapsed active time with
the deterministic/human baseline. Include one close/reopen during pending
review and one after accepted work. Demonstrate a user question before starting
a run, a follow-up about a proposed operation, and an explanation of the saved
result after reopen. No universal quality or speed claim follows
from a single successful run.

Deliver setup instructions, a reproducible demo project, a short screen
recording, the operation qualification matrix and known limits. The demo must
work entirely through the desktop application after provider setup.

The first reviewable implementation increment is Phase 1: in-memory context,
strict protocol/envelope validation, and scripted-provider tests. It needs no
provider credentials and establishes the contract needed by all later phases.

Implementation progress (2026-09-28):

- Phase 1 is implemented: the bounded autonomy context now has an in-memory
  builder; agent responses use a strict versioned schema; context, revision,
  policy, scope, operation capability and evidence references are validated;
  proposals bind to the existing autonomy request without executing them; and
  a scripted provider covers answers, follow-up questions and proposals.
- Phase 2 has started: agent context generation runs through the Qt geometry
  worker, and Qt controllers reserve a live `Session` so concurrent controller
  access is rejected. Remaining direct/background session paths still need to
  be folded into the shared coordinator before the phase exit gate is met.
- Phase 3 has started: project schema 8 persists conversations, ordered user
  and assistant messages, evidence references, context identity, document
  revision, provider/model identity, simulation status, and terminal state.
  History restores on reopen. Durable runs, turns, operation links, usage, and
  crash reconciliation are not implemented yet.
- Phase 4 has started: the project workspace contains a resizable conversation
  dock with Ask, Ctrl+Enter, Stop response, transcript, context revision and
  evidence references. The scripted fallback is deliberately labeled
  simulated and remains the deterministic CI path.
- Phase 5 has started: an optional Qt Network OpenAI Responses adapter runs
  asynchronously, uses strict structured output, enforces timeout and response
  limits, supports cancellation, validates evidence references and stale
  revisions, and never persists credentials. It is selected only when both
  `OPENAI_API_KEY` and `CSGIFY_OPENAI_MODEL` are present; otherwise the
  simulated provider remains active. A settings dialog, connection test,
  usage accounting, retries, streaming, and opt-in live evaluation still
  remain before the phase exit gate is met.
- The integration build and all 78 configured offline tests pass. This covers
  protocol, provider codec, schema-8 persistence, Qt dock, workspace smoke,
  cancellation plumbing, and existing geometry regressions; it is not a live
  provider or native-display qualification run.

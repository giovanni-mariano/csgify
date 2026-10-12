# Fixed-Point Decomposition Coordinator and Global Progress Policy

## Status

- **Purpose:** turn the decomposition capabilities already present in csgify
  into a bounded, deterministic, single-command workflow.
- **Primary target:** reproduce the certified FWTBM1 result directly from the
  original STEP without manual round trips or a user approving exact,
  volume-preserving intermediate splits.
- **Scope of this plan:** orchestration, global progress scoring, trial
  validation, policy, reporting, replay, Qt escalation, extension integration,
  and regression/performance testing.
- **Not in the critical path:** new approximate surface fitting, general
  complement emission, or new geometric cutter families. Those remain separate
  follow-up projects.

### Implementation status (2026-08-30)

- Phases A-C are implemented: global scoring, copied-model trials, OCCT
  coalescing, safety/resource gates, fixed-point orchestration, partial commit,
  progress callbacks, cancellation, and precise depth/piece stop reasons.
- Phase D is implemented: CLI and Lua entry points, JSONL progress events,
  accepted-action audit evidence, compact Lua/JSON recipes, optional STEP
  preservation, and replay terminal-geometry postconditions.
- Phase E is implemented: Qt worker-thread progress/cancellation, fresh residual
  review, autonomy context/operation support, and Pi-extension capability
  negotiation with one-call STEP+MCNP production.
- Phase F qualification and rollout are complete for FWTBM1 and both Qt/no-Qt
  regression suites. Fixed-point is the default `decompose-apply` path;
  `--single-pass` preserves the former behavior for compatibility testing.

## 1. Current state and problem statement

csgify already contains most of the geometry operations required by the target
workflow:

- `propose_decomposition_candidates` and
  `propose_bsp_decomposition_candidates` generate and score face-, plane-, and
  multi-plane candidates.
- `apply_candidate_geometry` dispatches candidates to the existing OCCT split
  implementations.
- `decompose_solid_prismatic` handles the pathological all-planar/prismatic
  case with bounded sequential subdivision.
- `decompose_solid_shell_difference` handles detected cavity shells.
- `shatter_solid` recursively tries failure-face, all-surface, and closure-plane
  cuts.
- `coalesce_decomposed_pieces` performs certified fixed-point OCCT fusion.
- `validate_convex_halfspace_cell`, post-mutation overlap checks, and volume
  guards provide the exactness boundary.
- the autonomy queue can already auto-apply a strictly bounded certified
  intermediate candidate when explicitly enabled.

The missing layer is document-wide orchestration. `decompose-apply` snapshots
the original non-convex solids and invokes one per-target operation. The current
`auto_decompose_bsp` owns a local queue, stops on its first policy/budget
failure, and calls `finish_document_mutation(..., !stopped, ...)`; therefore a
stopped run rolls the whole operation back. It does not compare the state of the
entire document before and after an intermediate split.

This produced two important observations on FWTBM1:

1. a candidate can create at least one exact child and still make the global
   residual/piece state worse; and
2. STEP export/reload between rounds loses useful in-memory provenance and can
   amplify fragments.

The new coordinator must keep one in-memory session, evaluate every action
against a fresh global state, accept only certified monotonic progress, and
stop predictably at explicit resource bounds.

## 2. Goals

### 2.1 Functional goals

1. Run decomposition to a fixed point in one session and one CLI invocation.
2. Refresh residual targets and candidate IDs after every accepted mutation.
3. Evaluate a candidate on temporary geometry before touching the live model.
4. Reject locally attractive candidates that worsen the global document state.
5. Run OCCT coalescing as part of each trial, so the score reflects the geometry
   that would actually be committed.
6. Commit certified partial progress when the remaining geometry genuinely
   needs a human or an unsupported strategy.
7. Emit MCNP automatically when the final document is exact and overlap-free.
8. Otherwise export an exact partial STEP, a machine-readable report, and a
   review queue describing only the unresolved decisions.
9. Record every accepted automatic decision with sufficient evidence for audit
   and deterministic replay.
10. Make the same coordinator callable from C++, Lua, the CLI, the Qt viewer,
    and `pi_csgify`.

### 2.2 Safety goals

- Never accept a split merely because it improves its parent target.
- Never treat candidate confidence as a substitute for geometry validation.
- Never silently accept approximation, unsupported surfaces, overlaps, invalid
  topology, excessive slivers, or volume drift.
- Bound actions, splitter calls, active pieces, residual pieces, candidate
  evaluations, recursion depth, and elapsed time.
- Compare cumulative volume against the run baseline, not only parent/child
  volume for one action.
- Avoid the unbounded all-planar arrangement path that caused the original
  memory problem.
- Preserve the last committed safe state on exceptions, failed postconditions,
  cancellation, or resource exhaustion.

### 2.3 Non-goals

- The coordinator does not authorize spline approximation by itself.
- It does not delete ordinary engineering solids based only on size.
- It does not introduce a second geometry kernel or perform CSG simplification
  solely in Alea; OCCT remains responsible for geometric fusion.
- It does not guarantee every CAD model can be decomposed. A precise bounded
  residual result is a valid terminal outcome.

## 3. Core invariants

Every accepted iteration must satisfy all of the following:

1. **Valid trial model:** OCCT shapes are valid enough for the existing analysis
   and volume routines; no operation reports failure.
2. **No new overlap:** the trial introduces no `mutation_introduced_overlap`,
   `solid_overlap`, or `overlap_unknown` error beyond explicitly allowed
   pre-existing overlaps.
3. **Local partition conservation:** the candidate's children preserve the
   target volume within the per-action tolerance.
4. **Cumulative conservation:** total active model volume remains within the
   run's cumulative relative-volume budget from the baseline.
5. **No unsupported regression:** a decomposition-only run does not introduce
   new spline/surface-replacement/user-decision requirements.
6. **No forbidden sliver:** candidate and post-coalescing geometry satisfy the
   configured minimum volume fraction.
7. **Resource compliance:** all hard counters remain within policy.
8. **Strict global progress:** the post-trial progress score compares strictly
   better than the pre-trial score after tolerance bucketing.
9. **Fresh evidence:** targets and candidates come from the current working
   revision, never from a prior iteration or LLM-supplied metrics.
10. **Exact terminal state:** MCNP emission is attempted only when every active
    body is exact-CSG supported and the ordinary emit readiness checks pass.

## 4. Public data model

Add the following public types to `include/csgify/session.hpp` or a focused new
header, `include/csgify/fixed_point_decomposition.hpp`. Prefer the focused header
and include it from `session.hpp` only where required.

### 4.1 `DecompositionProgressState`

```cpp
struct DecompositionProgressState {
    std::size_t unsupported_face_count = 0;
    double residual_volume = 0.0;
    std::size_t halfspace_violation_count = 0;
    std::size_t residual_count = 0;
    std::size_t active_piece_count = 0;
    std::size_t exact_body_count = 0;
    std::size_t overlap_error_count = 0;
    double total_volume = 0.0;
};
```

Definitions:

- `unsupported_face_count`: active faces needing spline/surface replacement or
  an explicit user decision. Count faces rather than only owner solids so the
  score detects partial regression.
- `residual_volume`: summed volume of active bodies that are not exact-CSG
  supported.
- `halfspace_violation_count`: summed convexity violation count over residual
  bodies for which analytic halfspace analysis is meaningful.
- `residual_count`: number of active non-exact bodies.
- `active_piece_count`: active original solids, decomposition pieces, and exact
  synthetic bodies participating in emission. Suppressed/inactive history is
  excluded.
- `exact_body_count`: diagnostic metric; it is not used ahead of residual
  volume because generating many tiny exact pieces is not inherently good.
- `overlap_error_count`: hard-gate evidence, not a metric that can be traded
  against progress.
- `total_volume`: used for cumulative conservation, not score ordering.

### 4.2 `DecompositionProgressScore`

The score is a stable, tolerance-aware representation of the state:

```cpp
struct DecompositionProgressScore {
    std::size_t unsupported_face_count = 0;
    std::uint64_t residual_volume_bucket = 0;
    std::size_t halfspace_violation_count = 0;
    std::size_t residual_count = 0;
    std::size_t active_piece_count = 0;
};
```

Compare it lexicographically in exactly this order:

```text
unsupported faces
→ residual volume bucket
→ halfspace violations
→ residual count
→ active piece count
```

The residual volume bucket prevents floating-point noise from being counted as
progress. Compute it using a policy scale based on baseline volume, for example:

```text
bucket_width = max(baseline_volume * progress_volume_epsilon,
                   absolute_volume_epsilon)
bucket = ceil(residual_volume / bucket_width)
```

Use checked/saturating conversion for non-finite or out-of-range values. A state
with NaN/Inf metrics is invalid and cannot be accepted.

Consequences of this order:

- creating an unsupported face can never be justified by reducing convexity;
- reducing non-exact material is more valuable than multiplying exact slivers;
- when residual volume is unchanged within tolerance, reduced geometric
  complexity counts as progress;
- if geometry complexity is tied, fewer unresolved bodies wins;
- piece count is the final tie-breaker, preventing neutral fragmentation.

### 4.3 `FixedPointDecompositionPolicy`

```cpp
struct FixedPointDecompositionPolicy {
    DecompositionPolicy candidate_policy;

    double max_action_relative_volume_delta = 1.0e-8;
    double max_cumulative_relative_volume_delta = 1.0e-8;
    double progress_volume_epsilon = 1.0e-10;
    double absolute_volume_epsilon = 1.0e-9;
    double min_piece_volume_fraction = 1.0e-8;

    std::size_t max_actions = 64;
    std::size_t max_candidate_evaluations = 4096;
    std::size_t max_splitter_calls = 4096;
    std::size_t max_active_pieces = 4096;
    std::size_t max_residual_pieces = 256;
    std::size_t max_candidates_per_target = 128;
    std::size_t max_stagnant_target_visits = 2;
    int max_target_depth = 16;
    std::uint64_t max_elapsed_ms = 0; // 0 means no wall-clock limit

    bool allow_medium_certified_progress = true;
    bool allow_low_certified_progress = false;
    bool allow_bbox_fallback = false;
    bool commit_certified_partial_progress = true;
    bool run_prismatic_strategy = true;
    bool run_shell_difference_strategy = true;
    bool run_shatter_fallback = false;
};
```

Policy interpretation:

- high and medium are search/ranking labels. A medium candidate may run
  automatically only after trial geometry satisfies every invariant and strictly
  improves the global score;
- low-confidence and bbox fallback remain explicit opt-ins because they are the
  main sources of wide search and fragment growth;
- `shatter` is a bounded fallback, off by default in the first rollout. It must
  pass the same trial/global gates if enabled;
- approximation and removal policies stay separate and are never inferred from
  this structure.

### 4.4 Result and trace types

Add:

```cpp
enum class FixedPointStopReason {
    exact,
    already_exact,
    no_improving_candidate,
    unsupported_geometry,
    action_budget,
    candidate_budget,
    splitter_budget,
    piece_budget,
    residual_budget,
    depth_budget,
    elapsed_time,
    overlap,
    volume_drift,
    cancelled,
    internal_error
};

struct FixedPointIterationRecord {
    std::size_t iteration = 0;
    std::string target_id;
    std::string candidate_id;
    std::string strategy;
    std::string confidence;
    DecompositionProgressState before;
    DecompositionProgressState after;
    double action_relative_volume_delta = 0.0;
    double cumulative_relative_volume_delta = 0.0;
    std::size_t pieces_before_coalesce = 0;
    std::size_t pieces_after_coalesce = 0;
    std::size_t candidates_evaluated = 0;
    bool accepted = false;
    std::string rejection_reason;
};

struct FixedPointDecompositionSummary {
    bool exact = false;
    bool mutated = false;
    FixedPointStopReason stop_reason;
    DecompositionProgressState baseline;
    DecompositionProgressState final_state;
    std::size_t actions_applied = 0;
    std::size_t candidates_evaluated = 0;
    std::size_t splitter_calls = 0;
    std::size_t peak_active_pieces = 0;
    std::vector<FixedPointIterationRecord> iterations;
};

struct FixedPointDecompositionResult {
    CommandResult command;
    FixedPointDecompositionSummary summary;
};
```

Keep a callback/cancellation hook in the internal runner so the Qt application
can remain responsive. Do not expose Qt types through the engine API.

## 5. Engine architecture

### 5.1 New files

Add:

- `src/fixed_point_decomposition.cpp`
- `src/fixed_point_decomposition.hpp` for internal helpers
- optionally `include/csgify/fixed_point_decomposition.hpp` for the public data
  structures
- `tests/fixed_point_progress.cpp`
- `tests/fixed_point_decomposition_smoke.sh`

Register them in `CMakeLists.txt` for both Qt-enabled and no-Qt builds.

Do not place the loop in `src/main.cpp`. The CLI, Lua, viewer, and extension must
all call the same session-level implementation.

### 5.2 Separate geometry dispatch from live-session mutation

Refactor current `apply_candidate_geometry` into two layers:

1. a model-level function that applies a fully resolved candidate to a supplied
   `CadModel` without logging, revision changes, or session side effects;
2. a live-session wrapper that records timing, performance counters, decision
   log entries, and commit semantics.

Suggested internal signature:

```cpp
bool apply_candidate_geometry_to_model(
    CadModel& model,
    const std::string& local_target_id,
    const DecompositionCandidate& candidate,
    const ConversionOptions& options,
    CandidateApplicationMetrics* metrics,
    std::vector<Diagnostic>* diagnostics);
```

The existing `apply_decomposition_candidate` and `auto_decompose_bsp` should use
this helper too, eliminating divergent dispatch logic.

### 5.3 Trial model

For every candidate:

1. copy the current working `CadModel` into `trial_model` using the same
   TopoDS-handle/snapshot semantics already relied upon by
   `CadDocument::committed_state`;
2. apply the candidate only to `trial_model`;
3. run `coalesce_decomposed_pieces(trial_model, options)`;
4. run full analysis and overlap detection on `trial_model`;
5. collect its `DecompositionProgressState`;
6. run hard policy checks;
7. compare the global score;
8. discard the trial on rejection or move it into the working model on
   acceptance.

No rejected trial may:

- increment the document revision;
- append a normal mutation record to the decision log;
- modify cached proposals or live model diagnostics;
- advance overlap high-water marks;
- alter performance counters except the explicit trial/candidate counters.

Rejected trials may be retained only as bounded iteration evidence in the final
report/review queue.

### 5.4 Global state collector

Implement one internal collector:

```cpp
DecompositionProgressState collect_decomposition_progress(
    const CadModel& model,
    const ConversionOptions& options,
    const ConversionReport* already_analyzed = nullptr);
```

It must count both unsuppressed STEP solids and active decomposed pieces. Reuse
`analyze_model` and existing certificate helpers; do not introduce a weaker
parallel convexity classifier.

Add unit tests proving that inactive decomposition history is excluded and that
coalesced replacements are counted once.

### 5.5 Deterministic target ordering

At the start of every iteration, regenerate the active residual list from the
working model. Sort it deterministically by:

1. unsupported geometry first, so it is escalated immediately rather than fed
   to an irrelevant splitter;
2. descending residual volume;
3. descending halfspace violation count;
4. stable local shape ID.

For a decomposition-only run, targets requiring a spline/surface replacement
are marked blocked and skipped. If all remaining residuals are blocked, stop
with `unsupported_geometry` and populate the human-review queue.

Do not carry `piece:N` targets across an accepted action. Coalescing can
deactivate or replace them. Regenerate target IDs from the new working model.

### 5.6 Strategy order

For each current target, evaluate strategies in this order:

1. shell-difference decomposition when `cavity_shell_count > 0`;
2. bounded prismatic decomposition when `prismatic_candidate` is true;
3. BSP candidates from the convexity-failure face;
4. remaining face-derived BSP candidates;
5. multi-plane candidates;
6. bbox fallback only when policy allows it;
7. bounded `shatter` fallback only when policy allows it.

Prismatic, shell-difference, and shatter operations currently mutate a model
directly rather than returning a candidate. Wrap each as a trial strategy with
the same before/after global evaluation. They do not bypass the fixed-point
gates.

### 5.7 Candidate ranking

Evaluate all policy-eligible candidates for the selected target up to
`max_candidates_per_target`. For each valid trial, calculate its post-score.
Choose the candidate with the lowest post-score. Tie-break in this order:

1. lower cumulative volume delta;
2. fewer post-coalescing active pieces;
3. confidence rank (`high`, then `medium`, then explicitly enabled `low`);
4. existing candidate `score` descending;
5. stable candidate ID/tool parameters.

Do not stop at the first locally usable candidate. The coordinator is allowed to
use the existing lazy proposal path only when it can prove no omitted candidate
could beat the current score class. Initially prefer correctness and bounded
exhaustive evaluation; optimize later using measured profiles.

### 5.8 Acceptance algorithm

Pseudocode:

```text
working = live document model snapshot
baseline = collect_progress(working)
current = baseline

if current.residual_count == 0:
    return already_exact

while true:
    enforce global budgets and cancellation
    targets = regenerate_and_sort_residual_targets(working)

    if targets empty:
        stop exact

    best_trial = none
    for target in targets:
        trials = evaluate_bounded_strategies(working, target)
        best_for_target = minimum trial passing hard gates
                          and score(trial) < score(current)
        if best_for_target exists:
            best_trial = best_for_target
            break
        mark target stagnant for this working-state fingerprint

    if best_trial is none:
        stop unsupported_geometry or no_improving_candidate

    working = move(best_trial.model)
    current = best_trial.state
    append accepted iteration trace
    clear stale target/candidate handles

    if current.residual_count == 0:
        stop exact
```

The target loop uses the deterministic priority order, rather than evaluating
the Cartesian product of every residual and candidate, to keep memory and time
bounded. A later optimization may maintain a bounded priority queue, but it
must invalidate all entries after a mutation.

### 5.9 Stagnation and cycle detection

Create a lightweight working-state fingerprint containing:

- progress score;
- sorted active residual shape fingerprints;
- sorted source IDs and volumes of active decomposition pieces.

If an accepted trial somehow reproduces a prior fingerprint, reject it as a
cycle even if floating metrics differ. If a target has no improving candidate
for `max_stagnant_target_visits` distinct working states, omit it until another
accepted action changes one of its shape/source fingerprints.

Store only a bounded set of fingerprints proportional to `max_actions`.

### 5.10 Commit semantics

The coordinator is one document command with a private working model:

- if the document was already exact, return success without incrementing the
  revision;
- if at least one action was accepted, atomically install the final working
  model and increment the document revision once;
- if stopped incomplete and `commit_certified_partial_progress` is true, commit
  the safe partial model and return `command.ok == true`; the accompanying
  report remains not emit-ready and the summary identifies the stop reason;
- if partial commit is false, restore the original model and mark every staged
  log action undone;
- if no action was accepted and residuals remain, do not mutate and return
  `command.ok == false` with a precise stop diagnostic;
- an exception, new overlap, invalid metric, or failed final postcondition
  restores the original committed state.

This deliberately separates "operation applied" from "document globally
emit-ready", matching the existing `CommandResult` contract.

### 5.11 Coalescing policy

Run `coalesce_decomposed_pieces` after every successful trial strategy, before
global scoring. Also run it once at the end in case source groups became
mergeable across iterations.

Record:

- piece count immediately after splitting;
- piece count after coalescing;
- number of certified fusions;
- coalescing elapsed time.

Do not use Alea simplification to decide whether OCCT solids can be merged. Alea
continues to simplify the final Boolean expression after geometry has been
certified.

## 6. Session API

Add:

```cpp
FixedPointDecompositionResult decompose_to_fixed_point(
    Session& session,
    const DocumentId& document_id,
    const FixedPointDecompositionPolicy& policy = {},
    ReportIdMode id_mode = ReportIdMode::scoped,
    FixedPointProgressCallback progress = {},
    CancellationToken cancellation = {});
```

If introducing general callback/token types is too broad for the first patch,
keep them internal and expose a minimal `std::function<bool(const ...&)>` whose
false return means cancellation.

The operation must work on the entire document because its acceptance rule is
global. A target filter may be provided for debugging, but the score must still
include every active body in the document.

Preserve `auto_decompose_bsp` as a compatibility API. Refactor it to reuse the
model-level candidate dispatcher, but do not silently change its historic
transaction behavior in the same patch. Deprecation or redirection can happen
after the fixed-point path has its own regression coverage.

## 7. CLI design

### 7.1 `decompose-apply`

Make the fixed-point coordinator the implementation behind `decompose-apply`
after an initial opt-in stabilization phase.

Rollout:

1. first release: `--fixed-point` selects the new coordinator;
2. validate the project corpus and extension;
3. make fixed-point the default;
4. retain `--single-pass` temporarily for compatibility and diagnosis.

Add options:

```text
--fixed-point
--single-pass
--max-actions N
--max-candidate-evaluations N
--max-splitter-calls N
--max-active-pieces N
--max-residual-pieces N
--max-elapsed-ms N
--max-cumulative-volume-delta X
--progress-volume-epsilon X
--allow-certified-intermediate
--allow-low-certified-progress
--allow-bbox-fallback-auto
--allow-shatter-fallback
--no-partial-commit
--progress-json PATH
```

Reuse existing `--max-depth`, `--max-splits`, prismatic budgets, overlap policy,
`--out`, `--out-step`, `--report`, and `--log`. Resolve naming overlap by
documenting `--max-splits` as a deprecated alias for `--max-actions` in
fixed-point mode.

Recommended automatic behavior:

- exact geometry, medium confidence, strict trial certification, and strict
  global progress: automatic;
- low confidence or bbox fallback: explicit opt-in;
- approximation, ambiguous removal, volume override, or new unsupported
  geometry: human review;
- safe partial result: report and export STEP, but do not emit an incomplete
  MCNP deck.

### 7.2 Console output

Print one concise progress line per accepted action when stderr is interactive:

```text
fixed-point 7/64: piece:43 candidate:3 residuals 8→7,
residual-volume 4.81e6→3.92e6, pieces 46→49→44(coalesced)
```

Final summary:

```text
fixed-point: exact after 12 actions; 79 active cells; 0 residuals;
relative volume delta 8.72e-9; stop=exact
```

Avoid printing every rejected candidate unless verbose/debug output is
explicitly requested.

## 8. Lua API

Add:

```lua
local result = s:decompose_to_fixed_point("doc:1", {
  max_actions = 64,
  max_active_pieces = 4096,
  max_cumulative_relative_volume_delta = 1e-8,
  allow_medium_certified_progress = true,
  allow_low_certified_progress = false,
  commit_certified_partial_progress = true
})
```

Return a summary table containing the baseline/final progress states, stop
reason, counters, exact/mutated flags, and a bounded iteration list.

Refactor the current Lua `safe_all` BSP loop to call this method. Continue to
run overlap precheck, optional bounded spline fitting, and explicitly authorized
removals outside the decomposition coordinator. This keeps approximation and
deletion authority separate.

## 9. Autonomy and LLM integration

### 9.1 Deterministic autonomy boundary

The LLM may decide to invoke the fixed-point operation and choose a policy from
an externally authorized envelope. It must not choose or assert geometric
metrics. csgify recomputes every target, candidate, trial, and score.

Add a registered operation:

```text
decompose_to_fixed_point(document)
```

Its autonomy evaluator checks only that requested limits do not exceed the host
policy. The individual automatic iterations are engine decisions under that
bounded grant and are all audited.

### 9.2 Context output

Extend `csgify context` with:

- the current global progress state/score;
- fixed-point policy ceiling;
- last run stop reason;
- residual targets ordered by coordinator priority;
- top rejected candidate reasons;
- resource usage and remaining budgets.

This allows an LLM to reason about whether to request a wider budget, ask for a
surface replacement, or escalate to the user without manufacturing geometry
evidence.

### 9.3 Audit records

For each accepted iteration record:

- actor and parent fixed-point run ID;
- target and freshly resolved candidate/tool parameters;
- policy rule (`decomposition.global_certified_progress`);
- before/after progress score and human-readable metric deltas;
- action and cumulative volume delta;
- pre/post-coalescing piece counts;
- document working-state fingerprint;
- candidate confidence and original ranking score;
- resource counters after acceptance.

Rejected trials go to the bounded run report, not the normal mutation log. The
log must remain replayable and must not contain speculative mutations.

## 10. Decision log and replay

There are two viable replay representations. Implement the first initially:

1. **expanded deterministic actions:** record each accepted resolved
   `decompose_by_face`, `decompose_by_plane`, `decompose_by_planes`, prismatic,
   or shell-difference action with its exact parameters, followed by the normal
   emit/export operation;
2. **coordinator macro:** record input policy and rerun candidate search.

Expanded actions are preferred because candidate rankings can evolve as the
software improves. Replay must reproduce the accepted geometry, not repeat the
historical search.

Add replay verification for:

- source fingerprint resolution;
- per-action volume postcondition;
- final global score equal to the recorded score;
- final active body count and total volume;
- byte-identical MCNP output where existing tests already guarantee stable
  ordering.

If a replayed action no longer satisfies its recorded postconditions, stop with
`replay_geometry_postcondition_failed`; never fall back to a newly proposed
candidate silently.

## 11. Report schema and diagnostics

Add a top-level `fixed_point_decomposition` object to JSON reports:

```json
{
  "exact": true,
  "mutated": true,
  "stop_reason": "exact",
  "baseline": {},
  "final": {},
  "actions_applied": 12,
  "candidates_evaluated": 83,
  "splitter_calls": 83,
  "peak_active_pieces": 79,
  "elapsed_ms": 12450.0,
  "iterations": []
}
```

The normal report summary remains authoritative for final emit readiness.

Add stable diagnostic codes:

- `fixed_point_decomposition_complete`
- `fixed_point_decomposition_already_exact`
- `fixed_point_decomposition_partial`
- `fixed_point_no_improving_candidate`
- `fixed_point_unsupported_geometry`
- `fixed_point_action_budget`
- `fixed_point_candidate_budget`
- `fixed_point_splitter_budget`
- `fixed_point_piece_budget`
- `fixed_point_residual_budget`
- `fixed_point_depth_budget`
- `fixed_point_elapsed_time`
- `fixed_point_candidate_rejected_volume`
- `fixed_point_candidate_rejected_overlap`
- `fixed_point_candidate_rejected_sliver`
- `fixed_point_candidate_rejected_unsupported_regression`
- `fixed_point_candidate_rejected_no_global_progress`
- `fixed_point_cycle_detected`
- `fixed_point_cancelled`
- `fixed_point_internal_error`

Only summarize repeated candidate rejection codes in the final diagnostics.
Keep detailed per-candidate evidence inside the bounded iteration trace so large
models do not generate enormous stderr/report output.

## 12. Qt viewer workflow

The coordinator should run on a worker thread. Use the callback to publish
immutable progress snapshots to the UI thread.

Viewer states:

- running: show accepted action count, residual count/volume, active pieces,
  elapsed time, and current target;
- exact: enable MCNP/STEP export and show the final audit summary;
- partial: display exact pieces normally and unresolved bodies in orange;
- human review: show the best rejected trials for the selected residual;
- cancelled/resource-limited: preserve and allow export of the last certified
  partial state.

For a candidate requiring human authority, the viewer should show:

- current geometry and trial preview side by side or toggleable;
- exact, residual, and sliver child colors;
- before/after global score;
- volume delta and resource impact;
- the precise failed policy gate;
- actions: approve this exact trial once, widen a named policy limit, reject,
  or stop/export partial.

Normal certified progress should not open modal authorization dialogs. It is
reported in the audit/history panel.

## 13. `pi_csgify` integration

After the core API and CLI stabilize, update the extension to prefer the
session-level fixed-point API rather than scripting repeated exported STEP
rounds.

Integration contract:

1. query csgify capabilities/version;
2. build a host-authorized `FixedPointDecompositionPolicy`;
3. invoke one in-memory fixed-point run;
4. stream progress events without polling exported files;
5. on `exact`, request MCNP and optional STEP export;
6. on safe partial stop, return the partial STEP/report plus review queue;
7. on unsafe/internal failure, return the unmodified source and diagnostics;
8. expose the audit trail to the calling LLM.

The extension should not duplicate the global score comparator or candidate
safety logic. It may set lower limits, but never reinterpret a rejected trial as
safe.

## 14. Testing strategy

### 14.1 Unit tests

Create `tests/fixed_point_progress.cpp` covering:

- lexicographic ordering of every score field;
- volume bucket boundaries and floating jitter;
- NaN/Inf rejection and overflow-safe bucketing;
- exact-body growth not compensating for increased residual volume;
- fewer residuals winning when residual volume/complexity tie;
- fewer active pieces winning only as the final tie-break;
- inactive history excluded from state collection;
- deterministic target and candidate tie-breaking;
- cycle/stagnation fingerprint behavior.

### 14.2 Synthetic engine tests

Use generated OCCT geometry to test:

- a box returns `already_exact` with no revision change;
- an L-shape converges and preserves volume;
- a candidate that creates an exact sliver but increases global residual volume
  is rejected;
- a split followed by certified fusion receives its post-coalescing score;
- a curved residual still uses the Boolean halfspace certificate;
- a planar small partition uses the vertex-complete certificate and does not
  regress the OCCT 8 false-negative fix;
- an overlap-producing trial is discarded without high-water/log leakage;
- an exception or invalid metric leaves the live model unchanged.

### 14.3 CLI smoke tests

Create `tests/fixed_point_decomposition_smoke.sh` covering:

- exact completion and MCNP emission;
- partial commit with STEP/report but no incomplete deck;
- `--no-partial-commit` rollback;
- action, candidate, splitter, piece, residual, depth, and elapsed-time stops;
- medium certified progress accepted in fixed-point auto policy;
- low/bbox/shatter denied by default and accepted only with explicit flags;
- report schema and diagnostic codes;
- decision-log compaction and replay;
- no-Qt build behavior.

### 14.4 Qt tests

Extend `viewer_session_smoke.sh` and controller tests to cover:

- progress callbacks are marshalled to the UI thread;
- cancellation does not close the viewer;
- clicking an unresolved row selects/previews the correct residual;
- automatic exact iterations do not create authorization dialogs;
- approving one rejected trial resumes from the current certified revision;
- closing/cancelling preserves the last safe state.

### 14.5 FWTBM1 acceptance test

Keep the large user model out of the default lightweight unit suite if licensing
or repository size prevents adding it. Provide an opt-in CTest/benchmark that
uses:

```text
/home/giovanni/projects/cad_examples/geouned_steps/new_FWTBM1/FWTBM1.step
```

Acceptance criteria:

- one `decompose-apply --fixed-point` invocation from the original STEP;
- no STEP reload between iterations;
- final report exact and overlap-free;
- 0 residuals and 0 splines;
- relative total-volume delta <= `1e-8`;
- MCNP emission succeeds for every final active body;
- final active pieces no worse than the certified 79-piece baseline unless a
  report explains a deliberate representation change;
- peak RSS below 1 GiB, with a target below 256 MiB;
- no unbounded all-planar batch splitter call;
- deterministic final body count, volume, stop reason, and action trace on two
  consecutive runs with the same build.

### 14.6 Full regression gate

Before defaulting `decompose-apply` to fixed-point:

- Qt build: all CTest tests pass;
- no-Qt build: all CTest tests pass;
- `git diff --check` passes;
- existing recipes replay unchanged;
- `shatter`, shell difference, prismatic decomposition, overlap gates, and
  autonomy tests retain their existing contracts;
- memory/performance results are recorded for FWTBM1 and rails.

## 15. Performance and memory design

The earlier 64 GiB incident was algorithmic, not a lack of physical RAM. The
coordinator must make memory growth observable and bounded.

Rules:

- keep only one accepted working model plus one candidate trial at a time;
- evaluate candidates sequentially unless future profiling proves bounded
  parallel trials are worthwhile;
- retain summaries, not trial B-reps, for rejected candidates;
- release each rejected trial before evaluating the next;
- cap active pieces and candidate count before invoking another splitter;
- prefer sequential prismatic support-plane processing over a full plane
  arrangement;
- coalesce before retaining a trial;
- avoid STEP serialization inside the loop;
- keep iteration traces bounded by candidate/action limits;
- report peak active pieces, candidate trials, splitter calls, and elapsed time.

Optional Linux RSS sampling can be added as telemetry, but piece/candidate/
splitter bounds remain the portable hard guard. A future host callback may
cancel based on process memory without embedding platform-specific policy in
the geometry engine.

## 16. Implementation phases

### Phase A — Progress model and comparator

Files:

- public/internal fixed-point headers
- `src/fixed_point_decomposition.cpp`
- `tests/fixed_point_progress.cpp`
- `CMakeLists.txt`

Deliverables:

- state collector;
- tolerance-aware score;
- comparator and fingerprints;
- JSON serialization helpers;
- unit tests.

Exit criterion: score behavior is completely covered without performing live
mutations.

### Phase B — Trial application and hard gates

Files:

- `src/session.cpp`
- `src/session_view.hpp` if internal accessors are required
- candidate/model dispatch helpers
- overlap/analysis integration
- synthetic tests

Deliverables:

- model-level candidate dispatcher;
- temporary trial application;
- coalescing before score;
- overlap, volume, unsupported, sliver, and budget gates;
- proof that rejected trials have no session/log/revision side effects.

Exit criterion: one candidate can be trialed, accepted, or rejected atomically
with deterministic evidence.

### Phase C — Fixed-point loop and partial commit

Files:

- fixed-point implementation
- `include/csgify/session.hpp`
- `src/session.cpp`
- report types/serialization

Deliverables:

- fresh target regeneration;
- deterministic strategy/candidate selection;
- strict global monotonicity;
- cycle/stagnation detection;
- all resource stops;
- exact, partial, and rollback commit modes;
- progress callback/cancellation.

Exit criterion: synthetic multi-round geometry converges in one API call, while
a non-convergent case terminates with a certified partial result.

### Phase D — CLI, Lua, audit, and replay

Files:

- `src/main.cpp`
- `src/lua_runner.cpp`
- `src/operation_registry.cpp`
- `src/decision_log.*`
- `src/recipe.cpp`
- `src/replay.cpp`
- `src/report.cpp`
- documentation and smoke tests

Deliverables:

- `decompose-apply --fixed-point`;
- Lua session method;
- bounded progress JSON;
- expanded accepted-action audit trail;
- deterministic replay postconditions;
- context/autonomy operation registration.

Exit criterion: direct run, compacted recipe, JSON replay, and Lua run produce
equivalent exact geometry/decks.

### Phase E — Qt and extension integration

Files:

- viewer/controller/Qt worker files
- Qt smoke/controller tests
- `pi_csgify` adapter in its own repository

Deliverables:

- non-blocking progress UI and cancellation;
- residual/rejected-trial review;
- automatic operations reported without authorization dialogs;
- extension capability negotiation and one-call orchestration.

Exit criterion: the extension can run the coordinator autonomously, and the
viewer is invoked only for a genuine failed policy gate or explicit request.

### Phase F — FWTBM1 qualification and default switch

Deliverables:

- reproducible FWTBM1 benchmark report;
- memory/runtime/action trace baseline;
- two-run determinism check;
- full Qt/no-Qt regression pass;
- fixed-point becomes the default `decompose-apply` path;
- temporary `--single-pass` compatibility option documented.

Exit criterion: the original FWTBM1 command completes from source STEP to
certified STEP and MCNP in one invocation within all declared bounds.

## 17. Risks and mitigations

### Trial copies share OCCT handles

Risk: an OCCT operation mutates referenced shapes in place and contaminates the
working model.

Mitigation: verify the splitter/fuse paths construct new shapes. Add a test that
hashes/fingerprints the working model before and after a rejected trial. If any
operation is in-place, deep-copy only that target with `BRepBuilderAPI_Copy`
before trial application.

### Score rewards artificial residual-volume reduction

Risk: a trial drops material and appears to reduce residual volume.

Mitigation: volume conservation is a hard gate evaluated before score. Sliver
drops count against both local and cumulative volume budgets.

### Coalescing makes target IDs unstable

Risk: queued `piece:N` identifiers refer to inactive pieces.

Mitigation: regenerate targets after every accepted trial and use fingerprints
only for cycle/stagnation history, never as a mutation handle without fresh
resolution.

### Candidate evaluation becomes expensive

Risk: exhaustive trials multiply splitter cost.

Mitigation: strict candidate/splitter/time budgets, deterministic target
priority, one trial model at a time, failure-face candidates first, and later
profile-guided pruning that preserves score optimality classes.

### Partial commit surprises existing callers

Risk: callers interpret `CommandResult.ok` as global readiness.

Mitigation: retain the documented operation-applied semantics, expose
`summary.exact`, keep `report.ok()` authoritative, and roll out behind
`--fixed-point` before changing the CLI default.

### Logs become very large

Risk: every rejected candidate bloats decision logs and LLM context.

Mitigation: log only accepted mutations; place bounded rejection aggregates in
the run report and expose only the top relevant entries in context/viewer.

### A lexicographic score blocks necessary temporary worsening

Risk: some decompositions may require an intermediate state with more residual
volume/count before later convergence.

Mitigation: exact partitioning should not increase residual volume when exact
children are peeled away; unchanged volume may still improve halfspace
complexity. Cases requiring true temporary worsening remain human/explicit
search territory. A future beam-search mode may be added behind a separate,
strict resource policy rather than weakening the default invariant.

## 18. Definition of done

The project is complete when all of the following are true:

1. `decompose_to_fixed_point` is a reusable session API with no CLI/Qt
   dependency.
2. Every candidate is trialed with OCCT coalescing and the normal geometry
   certificates before live mutation.
3. Every accepted iteration strictly improves the documented global score and
   passes cumulative safety/resource gates.
4. Partial results commit or roll back exactly according to explicit policy.
5. Accepted actions are fully auditable and replayable; rejected trials leave no
   mutation records.
6. CLI, Lua, Qt, autonomy context, and the extension share the same engine
   implementation.
7. Human review is requested only when a named gate fails or an unsafe authority
   expansion is required.
8. The full Qt and no-Qt test suites pass.
9. FWTBM1 reaches an exact, overlap-free, zero-spline MCNP result from its
   original STEP in one command, with relative volume delta at most `1e-8`, no
   more than the 79-piece certified baseline, and peak RSS below 1 GiB.
10. The original memory-exhausting all-planar path cannot be entered by the
    fixed-point coordinator without an explicit bounded strategy.

## 19. Recommended first implementation slice

Implement Phases A and B together as the first reviewable change:

1. add the state/score types and comparator;
2. extract model-level candidate dispatch;
3. trial one candidate on a copied model;
4. coalesce and collect post-trial state;
5. enforce volume/overlap/unsupported/sliver gates;
6. return structured accepted/rejected evidence without committing;
7. cover it with unit and synthetic tests.

This establishes the safety boundary first. The fixed-point loop then becomes a
small deterministic consumer of a tested trial evaluator rather than a large
orchestrator that mixes geometry mutation, policy, logging, and CLI behavior.

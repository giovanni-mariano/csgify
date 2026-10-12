# Local Incremental Decomposition and Certification Plan

## Status

- **Document status:** implementation in progress; core incremental coordinator
  and CLI/Qt/Pi progress integration landed, with Phase 8 qualification and
  rollout gates remaining.
- **Created:** 2026-08-31.
- **Primary objective:** obtain GEOUNED-like decomposition throughput on large
  analytic assemblies while retaining csgify's stricter MCNP overlap, volume,
  provenance, replay, and final-emission guarantees.
- **Scope:** the fixed-point coordinator, candidate generation, OCCT split
  execution, local certification, cached progress accounting, incremental
  overlap checks, reporting, tests, benchmarks, and `pi_csgify` integration.
- **Not in scope:** changing libalea void-generation algorithms, weakening the
  final MCNP safety gate, accepting approximation without an explicit budget,
  or making `shatter` the normal first-line decomposition strategy.
- **Relationship to existing plans:** this is a performance and certification
  successor to `FIXED_POINT_DECOMPOSITION_COORDINATOR.md`. It preserves that
  plan's global progress policy and terminal guarantees, but replaces copied
  whole-model candidate trials with local immutable trials and incremental
  state updates.

### Implementation checkpoint — 2026-08-31

Completed in the core engine:

- reproducible decomposition benchmark manifest/runner and performance JSON;
- body progress-certificate ledger with projected parent-to-child replacement;
- immutable ordinary BSP child retention and exact local partition proof;
- affected-neighbour overlap certification with baseline-overlap inheritance;
- source-local OCCT coalescing for structural child partitions;
- parallel face-plane family batches, duplicate support removal, stagnant-target
  suppression, pre-call hard candidate/splitter budgets, and a configurable
  64-candidate per-target fairness cap exposed through CLI, Lua, and Pi;
- constant-call exact partition certification using compound-minus-parent and
  parent-minus-compound Booleans; sibling disjointness follows from certified
  union containment/coverage plus the conserved child-volume sum;
- algebraic candidate progress prescreening and record-only exact
  certification: after one candidate is certified, only projected candidates
  capable of beating it spend additional OCCT Boolean proof calls;
- terminal authoritative global reconciliation, optional per-action debug
  reconciliation, and a legacy global-trial comparison mode;
- CLI and Lua policy controls plus decision-log/replay-compatible plane actions;
- `pi_csgify` 0.4.3 capability probing, fixed-point workflow policy, and local
  proof/reconciliation telemetry in tool results (source and active install);
- focused ledger, partition, neighbour-overlap, progress, replay, and budget
  regressions. The complete 61-test suite passes.

Measured Triangle checkpoints use the same source digest. The first incremental
run took approximately 367 seconds with per-action reconciliation. Local BSP
proofs, parallel batches, stagnant-target suppression, set-inclusion overlap
proofs, removal of the duplicate CLI pre-analysis, and immutable exact-overlap
pair reuse reduced the current end-to-end safe-partial run to 114.8 seconds
with 151,948 KiB peak RSS. Moving prismatic/revolution mutation adapters into
one-body sandboxes then reduced the current checkpoint to 113.0 seconds and
151,848 KiB peak RSS, with zero whole-model trial copies. The
coordinator applied 42 actions, produced the same 71 residuals, used 1,929
bounded splitter calls, and wrote the partial STEP without invoking shatter.
Local overlap proof time fell from 79.2 seconds (5,313 exact Commons) to 0.2 ms.
The terminal global sweep reused 1,610 exact unchanged-pair results and freshly
evaluated 3,616 candidate pairs; total global reconciliation fell from ~63.6 s
to ~41.8 s while still performing two authoritative sweeps. These are
architectural checkpoints, not the rollout target: proposal generation
(~43.8 s) and global overlap reconciliation (~34.3 s) now dominate the run.

FWTBM1 qualification retains the same safe partial geometry (59 active pieces,
58 exact bodies, and one 2.926639 mm3 residual). Successive constant-call
partition proof, algebraic prescreen, 64-candidate fairness cap, and record-only
certification reduced wall time from 144.7 s to 74.6 s. The latest run used
156,788 KiB peak RSS, two certified actions, five local Boolean proof calls,
and 5.1 s of local certificate time. This directly closes the original memory
concern on the 64 GiB workstation; the remaining residual is a geometry/search
outcome, not resource exhaustion.

The first bounded toy-car qualification (before the fairness cap and
record-only proof optimization) stopped safely at the five-minute policy limit:
7 actions, 11 exact bodies, 17 residual pieces, 95,864 KiB peak RSS, and no
shatter. A repeat reached 13 actions and 49 exact bodies before one in-flight
proposal carried wall time to 345 s, but exposed overlap-lineage bookkeeping:
splitting one pre-existing parent-neighbour overlap into child-neighbour report
pairs was misreported as an incremental mismatch/new overlap. The ledger now
compares overlap blocker presence (terminal reports retain exact pair counts),
exact child IDs inherit certified parent-overlap lineage, and elapsed exits are
classified as `elapsed_time`. A focused toy-car run committed the overlapping
first body as three pieces in 22.3 s, retained the same 0.0285133 mm3 overlap,
and stopped correctly at `action_budget` without rollback. The final corrected
bounded repeat then preserved all 13 actions, 103 active bodies, 93 emitted
decomposition pieces, and 49 exact bodies. It stopped correctly at
`elapsed_time` after 343.7 s (one non-interruptible OCCT proposal carried the
300 s boundary to 341.6 s), used 134,048 KiB peak RSS, and wrote the partial
STEP. There was no incremental mismatch, false introduced-overlap diagnostic,
or rollback. The original 0.0285133 mm3 overlap is represented exactly as two
0.0142567 mm3 child-neighbour pairs and remains an MCNP blocker pending the
configured ownership decision.

The representative 68 MiB ITER shielding assembly qualification contains
1,272 source solids and approximately 30,000 faces. The latest bounded run
loaded STEP in 85.8 s, completed its lean initial analysis/overlap baseline in
175.9 s of coordinator time, accepted four certified BSP actions, produced ten
pieces, and wrote the partial STEP without shatter. Terminal reconciliation
now preserves stable, locally exact child certificates and agrees with the
incremental state; the run stops honestly at `elapsed_time`, not
`internal_error`. End-to-end wall time was 614.5 s, peak RSS 2,094,884 KiB,
and the terminal state retained 1,148 residuals and all MCNP overlap blockers.
This is a safe throughput checkpoint, not a complete conversion or rollout
speed pass.

Long-operation visibility is now part of the contract. CLI and Pi stream STEP
load, initial analysis/overlap completion, exact-certificate/candidate target
selection, accepted actions, terminal verification, finish state, and
30-second heartbeats. The core and CLI default `max_elapsed_ms` to zero
(unlimited). Pi exposes optional `maxElapsedMs`, no longer imposes its former
hidden 15-minute process timeout when omitted/zero, and instructs the agent not
to invent a time budget for overnight or multi-day work.

Remaining before this plan is complete:

- finish the differential and performance qualification matrix, especially
  `large_pi` and additional representative ITER port components;
- demonstrate the stated rollout speed/memory gates and document production
  defaults from stored benchmark artifacts.

Phase status: Phases 1-7 are implemented; Phase 8 qualification is in
progress. Qt preview retains the parent/children overlay and plane supports and
shows exact partition/overlap evidence with a recommended next action.

## 1. Evidence and baseline

### 1.1 Reference model

The comparison model is:

```text
/home/giovanni/projects/cad_examples/geouned_steps/geo_large/Triangle.stp
SHA-256: e660b7a9b1ef1da724c1a037fc14bae329cba6c9ff5df9db506174e2c1223c32
```

The copy under `large_pi` has the same digest. Measurements below were made
against csgify source revision `8037748` plus the current working-tree changes.
They must be captured again in a checked benchmark artifact before rollout.

### 1.2 GEOUNED observed result

GEOUNED 1.6.4 with FreeCAD 1.0.0 completed the complete STEP-to-output run in
approximately 42.5 seconds:

- 229 original components;
- 52 components split and 177 left as one geometric solid;
- 328 geometric pieces after decomposition;
- at most 8 pieces for one component;
- approximately 37.6 seconds in material-cell translation and 4.9 seconds in
  void generation;
- total decomposition volume delta approximately `6.60e-9` relative;
- maximum observed per-component delta approximately `1.67e-6`;
- `forceNoOverlap` was disabled;
- one approximately `0.006 mm^3` degenerate fragment was discarded.

The last three points are important: the performance result is real, but its
safety contract is not identical to csgify's production MCNP contract.

### 1.3 csgify observed result

On the same model, the bounded csgify diagnostic spent approximately 140
seconds evaluating four BSP candidates for `solid:150` without accepting an
action. GEOUNED decomposed the corresponding component into four pieces in
approximately 0.34 seconds.

The comparison is not a like-for-like timing of a single OCCT call. It exposes
the architectural difference:

```text
Current csgify candidate
  copy complete CadModel
  -> mutate target in the copy
  -> coalesce all eligible source groups
  -> analyze complete model
  -> run complete overlap detection
  -> recompute half-space complexity for every residual
  -> compare global score

Required candidate
  obtain one immutable target shape
  -> split target locally
  -> analyze/certify only its children
  -> check only affected spatial neighbours
  -> update cached global totals algebraically
  -> commit target replacement if score improves
```

### 1.4 Existing capabilities that must be reused

This plan must not duplicate facilities already present:

- `decompose_solid_by_planes` already submits multiple plane tools to one OCCT
  split operation.
- the BSP proposal code already deduplicates identical planar supports and
  analyzes candidate children.
- `coalesce_decomposed_pieces` already performs certified OCCT fusion.
- overlap detection already uses deterministic sweep-and-prune before exact
  OCCT intersections.
- the persistent project store already has an R*Tree and definition-level
  recognition/conversion cache infrastructure.
- the fixed-point coordinator already provides bounded actions, candidate and
  splitter budgets, progress events, cancellation, partial commit, decision
  logs, compact recipes, and replay.
- MCNP emission already groups pieces by `source_solid_id`, builds one Boolean
  union, and simplifies it where possible.

The missing work is primarily locality, cache invalidation, face-plane batch
generation, and a two-level certificate.

## 2. Design principles and invariants

### 2.1 Non-negotiable invariants

Every committed exact decomposition must satisfy:

1. **Valid children:** each child is a non-null, positive-volume OCCT solid and
   passes the selected shape-validity checks.
2. **Local containment:** no child adds positive volume outside its parent
   beyond the exact decomposition tolerance.
3. **Local coverage:** the union of children covers the parent within the exact
   decomposition tolerance.
4. **Sibling disjointness:** distinct children have no positive-volume
   intersection above the configured overlap thresholds.
5. **Volume conservation:** summed child volume preserves parent volume within
   the per-action limit.
6. **No unsupported regression:** an exact decomposition does not introduce a
   new unsupported-surface or user-decision requirement.
7. **Strict progress:** replacing the parent certificate with child
   certificates strictly improves the existing fixed-point score.
8. **Bounded execution:** plane batches, recursion, pieces, Boolean calls,
   candidate evaluations, memory, and elapsed time obey hard limits.
9. **Stable ownership:** every child retains the root `source_solid_id`,
   semantic metadata, definition/occurrence provenance, and material identity.
10. **Final global certificate:** MCNP and the final production STEP are emitted
    only after a fresh document-wide overlap, volume, exactness, and optional
    modeled-domain coverage pass.

### 2.2 Local checks are not a weaker final contract

Local validation is an optimization of trial evaluation, not permission to
emit based only on cached evidence. The coordinator will have two levels:

- **commit certificate:** proves that one parent was replaced by a valid local
  partition and that no affected neighbour relationship became unsafe;
- **terminal certificate:** rebuilds the authoritative complete report and
  repeats the global MCNP safety gate immediately before output.

If the terminal certificate disagrees with cached state, emission fails, the
failure is reported as `incremental_certificate_mismatch`, and the last locally
certified geometry remains available for inspection. No automatic fallback may
downgrade this error to a warning.

### 2.3 Exact and approximate paths remain separate

This plan describes the exact decomposition path. Numerical fragment removal,
surface fitting, and overlap ownership remain governed by
`ApproximationPolicy` and its cumulative ledger. A local decomposition trial
must not silently consume an approximation budget or inherit GEOUNED's 1%
warning threshold. If an exact trial fails coverage or volume conservation, it
is rejected; an explicitly authorized approximation operation may later handle
the residual through the existing budgeted APIs.

## 3. Target architecture

### 3.1 Body-level certificate cache

Introduce an internal cache keyed by stable active-body identity plus geometry
revision:

```cpp
struct BodyRevisionKey {
    std::string body_id;
    std::uint64_t geometry_revision = 0;
};

struct BodyProgressCertificate {
    BodyRevisionKey key;
    std::string source_solid_id;
    std::string geometry_digest;
    TopoDS_Shape shape;
    BBox bbox;
    double volume = 0.0;
    std::size_t face_count = 0;
    std::size_t unsupported_face_count = 0;
    std::size_t halfspace_violation_count = 0;
    bool exact_csg_supported = false;
    bool needs_decomposition = false;
    bool needs_user_decision = false;
    bool valid = false;
};
```

The cache is internal and session-owned. It must not be serialized as trusted
geometry evidence. The decision log records certificate digests and measured
values, but replay regenerates every certificate from the current geometry.

Invalidation rules:

- inserting or removing one active body invalidates only that body and its
  affected spatial relationships;
- changing conversion tolerances or exactness policy invalidates all body
  certificates;
- changing only output formatting does not invalidate geometry certificates;
- coalescing invalidates the consumed pieces and creates a new certificate for
  each fused result;
- undo/redo and recipe replay rebuild or restore cache state from geometry, not
  from untrusted log values.

### 3.2 Algebraic document progress ledger

Add an internal `DecompositionProgressLedger` that contains:

- a map from active body ID to `BodyProgressCertificate`;
- running totals corresponding exactly to `DecompositionProgressState`;
- baseline and current total volume;
- known overlap pairs and unknown-overlap pairs;
- active body count, residual count, and root-source grouping;
- a deterministic spatial index of current active bodies;
- a monotonically increasing ledger revision.

Replacing parent `P` with children `C[]` becomes:

```text
trial_state = current_state
trial_state -= contribution(P)
for child in C:
    trial_state += contribution(child)
trial_state.total_volume += sum(volume(C)) - volume(P)
trial_state.overlap_error_count += local overlap-set delta
```

The result must match a full `collect_progress_state` within tolerance. Add a
debug/qualification mode that computes both and asserts equivalence after each
accepted action.

### 3.3 Immutable local trial

Replace `AcceptedTrial::model = working` during candidate evaluation with:

```cpp
struct LocalDecompositionTrial {
    std::string target_id;
    std::string source_solid_id;
    std::uint64_t base_ledger_revision = 0;
    TopoDS_Solid parent;
    BodyProgressCertificate parent_certificate;
    std::vector<TopoDS_Solid> children;
    std::vector<BodyProgressCertificate> child_certificates;
    LocalPartitionCertificate partition;
    LocalOverlapDelta overlap_delta;
    DecompositionProgressState projected_state;
    DecompositionCandidate candidate;
    std::string strategy;
    std::string log_op;
    std::vector<DecisionParam> params;
};
```

The trial owns no mutable reference into `CadModel`. It is safe to discard and
cannot leave inactive pieces, diagnostics, or ID counters behind.

At commit time, verify that `base_ledger_revision` still equals the live
ledger revision, reserve deterministic piece IDs, suppress the parent, append
the children, update the ledger and spatial index, and record one atomic
decision. A stale trial is regenerated rather than rebased.

### 3.4 Local geometry operations

Refactor mutation-oriented geometry functions into two layers:

```cpp
LocalSplitResult split_solid_by_face_local(...);
LocalSplitResult split_solid_by_plane_local(...);
LocalSplitResult split_solid_by_planes_local(...);
LocalSplitResult decompose_prismatic_local(...);
LocalSplitResult decompose_revolution_local(...);
LocalSplitResult decompose_shell_difference_local(...);

bool commit_local_split(CadModel&, DecompositionProgressLedger&,
                        const LocalDecompositionTrial&);
```

Existing public/session operations remain compatible and become thin wrappers:
resolve the target, call the local operation, certify it, then commit. This
avoids maintaining two different split implementations.

## 4. Parallel face-plane batch strategy

### 4.1 Why it is needed

The current multi-plane candidate is a three-axis bounding-box fallback. The
normal BSP path still evaluates existing faces largely one at a time. For
rails, corrugations, channels, thin walls, and repeated slots, a single face
plane often peels off a sliver without making a child convex. A coherent group
of parallel planes can partition the whole repeated profile in one splitter
call and expose meaningful progress.

### 4.2 Plane extraction and canonicalization

For each planar face of the target:

1. obtain its `gp_Pln` support;
2. normalize the direction and choose a canonical sign so `n` and `-n` share
   one family;
3. compute signed offset `d = n dot p`;
4. group normals using `angular_tolerance` with a safe floor;
5. merge coincident offsets using `linear_tolerance` with a scale-aware floor;
6. retain contributing face IDs, face area, concave-edge participation, and
   whether the plane is the current convexity-failure support.

Avoid decimal-string geometry keys. Use deterministic quantization only for
hash bucketing and confirm equality with geometric tolerance before merging.

### 4.3 Group ranking

Rank plane families deterministically using:

1. contains the convexity-failure support;
2. number of distinct useful offsets crossing the target interior;
3. number of concave/reflex edges incident on contributing faces;
4. expected interval partition count;
5. summed contributing face area;
6. lexicographic canonical plane signature as final tie-breaker.

Reject planes that only touch the boundary or cannot divide the parent's
interior. Cap both planes per batch and families per target before invoking
OCCT.

### 4.4 Batch construction

Generate bounded candidates in this order:

1. all useful offsets in the best parallel family;
2. a reduced subset containing convexity-failure and concavity-associated
   offsets;
3. other ranked parallel families;
4. a bounded combination of two compatible orthogonal/dominant families when
   the single-family result makes no progress;
5. existing single-face BSP candidates;
6. existing bbox fallback only when policy permits it.

The default batch should contain support planes derived from the target's own
faces, not arbitrary regularly spaced planes. Duplicate, coincident, exterior,
and near-zero-separation tools are removed before the OCCT call.

### 4.5 Recursive local work queue

One batch may leave non-convex children. Use a bounded deterministic queue local
to the root source:

```text
queue = [parent]
while queue not empty:
    body = highest-priority residual
    if exact: retain body
    else choose next unused ranked plane family for body
    split body locally
    certify its partition
    enqueue residual children
stop on exact result, no useful family, or a local hard budget
```

Priority is unsupported-last, then descending residual volume, then descending
half-space violations, then stable local ID. The queue must respect:

- maximum recursion depth;
- maximum local pieces;
- maximum local splitter calls;
- maximum planes per splitter;
- maximum local elapsed time;
- maximum tolerated sliver count;
- cancellation between OCCT calls.

An incomplete local run may be proposed as a fixed-point action only if the
certified children strictly improve the global projected score. It must not be
reported as a completed source conversion.

## 5. Local partition certificate

### 5.1 Certificate data

```cpp
struct LocalPartitionCertificate {
    bool valid_children = false;
    bool contained_in_parent = false;
    bool covers_parent = false;
    bool siblings_disjoint = false;
    bool volume_within_tolerance = false;
    double parent_volume = 0.0;
    double child_volume_sum = 0.0;
    double relative_volume_delta = 0.0;
    double outside_parent_volume = 0.0;
    double uncovered_parent_volume = 0.0;
    double sibling_overlap_volume = 0.0;
    std::size_t exact_boolean_calls = 0;
    std::string certificate_digest;
    std::string failure_code;
};
```

### 5.2 Fast checks

Perform inexpensive rejection checks first:

- non-null solid type;
- OCCT validity;
- positive finite volume;
- finite bbox contained in the parent bbox with tolerance;
- child count and piece budgets;
- sum-volume delta;
- no unsupported-surface regression;
- minimum piece fraction.

These checks can reject a bad result but cannot alone certify it.

### 5.3 Exact local checks

For trials that pass fast checks:

- test sibling pairs only when their padded bboxes intersect;
- compute exact positive-volume sibling intersections;
- prove child containment using `child - parent` or an equivalent exact OCCT
  Boolean and measure any remainder;
- prove coverage using `parent - compound(children)` and measure any remainder;
- treat an OCCT Boolean failure as `unknown`, never as zero;
- use the existing absolute/fraction thresholds consistently, while exact
  decomposition volume limits remain independently enforceable.

The coverage/containment checks may be optimized when OCCT exposes reliable
splitter ancestry for the exact result, but the first implementation should use
measured local Booleans as the reference oracle.

### 5.4 Certificate digest

Hash a canonical record containing:

- parent geometry digest;
- ordered child geometry digests;
- OCCT version;
- relevant tolerances and policy digest;
- all measured volumes and Boolean-known flags;
- splitter strategy and canonical plane signatures.

The digest is audit evidence and a cache key. It is not accepted without
revalidating geometry when the OCCT version, policy digest, or source digest
changes.

## 6. Incremental overlap safety

### 6.1 Baseline

Run one complete overlap pass when the ledger is created. Store canonical pair
keys and whether each result is clear, overlapping, or unknown. Production
conversion remains blocked by pre-existing overlap unless an explicit
ownership/approximation decision resolves it.

### 6.2 Affected-neighbour query

For a trial replacing parent `P`:

1. query the in-memory spatial index with the union bbox of the children,
   expanded by linear tolerance;
2. remove `P` and bodies belonging exclusively to the same replaced local
   partition from the neighbour set;
3. test each new child only against the returned active neighbours;
4. compare the resulting overlap relationships to the baseline relationships
   involving `P`;
5. reject any new positive-volume or unknown relationship.

If child containment is certified, an exact child cannot introduce a real
overlap with a body that did not overlap the parent. The neighbour pass still
runs initially as defence against kernel/tolerance defects and to produce
explicit evidence. A later measured optimization may skip some exact Booleans
only when this implication is encoded and regression-tested.

### 6.3 Source-group handling

Artificial sibling interfaces are permitted to touch but not interpenetrate.
Pieces sharing `source_solid_id` are checked as siblings during local
certification and are not treated as independent engineering-component
overlaps after commit. Bodies with different source owners always remain
normal overlap candidates.

### 6.4 Final global check

Before MCNP emission, rebuild active entries from the actual `CadModel`, run
the deterministic sweep-and-prune plus exact intersections, and compare the
result with the ledger. Any mismatch blocks emission. `--allow-overlaps` may
continue to exist for diagnostic workflows, but neither the default CLI nor
`pi_csgify` may use it for a production conversion recipe.

## 7. Local coalescing

The current `coalesce_decomposed_pieces` scans every source group. Add:

```cpp
LocalCoalesceResult coalesce_local_partition(
    const std::string& source_solid_id,
    const std::vector<TopoDS_Solid>& pieces,
    const ConversionOptions& options);
```

Run it only on the new local partition before child analysis is finalized.
Every accepted fusion must retain the existing validity, summed-volume, and
convexity certificates. The coordinator must not rescan unrelated source
groups after each candidate.

At terminalization, an optional full coalescing pass may verify idempotence. A
non-zero result indicates an incremental coalescing bug or an opportunity that
crossed action boundaries; record it and rerun the final global certificate.

## 8. Fixed-point coordinator changes

### 8.1 Initialization

Replace the initial `collect_progress_state` usage with a constructor that:

1. runs one authoritative analysis and overlap pass;
2. builds one `BodyProgressCertificate` per active body;
3. initializes the progress ledger and in-memory spatial index;
4. verifies ledger totals against the authoritative report;
5. captures the baseline volume and policy digest.

### 8.2 Candidate evaluation

For each target:

- obtain its body certificate and immutable OCCT shape;
- try exact structural recognizers through local-returning APIs;
- try definition-conversion reuse;
- generate parallel face-plane batches;
- fall back to bounded single-face BSP;
- use targeted shatter only after every supported target fails the preceding
  strategies and policy explicitly enables it;
- locally coalesce, certify, project ledger state, and compare scores;
- keep only the best bounded trial object, not a copied `CadModel`.

Candidate generation must stop early on a locally certified zero-residual
partition. Confidence remains a search/ranking hint; certificate evidence is
the authority.

### 8.3 Commit

Commit consists of one transaction:

1. verify ledger and document revisions;
2. allocate stable piece IDs in deterministic child order;
3. suppress the parent or old piece;
4. append children and provenance;
5. update only affected cache and spatial entries;
6. update progress totals algebraically;
7. append one decision-log action containing the local certificate digest;
8. publish an `action_accepted` progress event.

If any step before log publication fails, restore vector sizes, active flags,
ID counters, ledger entries, and spatial entries. Prefer an explicit scoped
mutation journal over copying the complete model.

### 8.4 Checkpoints

Run a complete analysis/overlap reconciliation:

- at initialization;
- in debug mode after every accepted action;
- in production after a configurable number of accepted actions or when cache
  inconsistency is suspected;
- unconditionally before final STEP/MCNP publication;
- after cancellation only if an output/report requesting a final certificate
  is being produced.

The default production interval may be greater than one, but the terminal pass
is never optional.

### 8.5 Stop reasons

Retain existing reasons and add precise local outcomes:

- `no_local_partition_progress`;
- `local_partition_invalid`;
- `local_partition_unknown`;
- `parallel_plane_batch_exhausted`;
- `local_piece_budget`;
- `local_boolean_budget`;
- `incremental_certificate_mismatch`;
- `final_global_certificate_failed`.

User-facing text for ordinary residuals should continue to recommend targeted
shatter or review rather than imply that no candidates exist.

## 9. Public policy, CLI, Lua, Qt, and Pi

### 9.1 Policy additions

Extend `FixedPointDecompositionPolicy` additively:

```cpp
bool use_incremental_trials = true;
bool run_parallel_plane_batches = true;
bool verify_incremental_state_each_action = false;
std::size_t max_parallel_families_per_target = 8;
std::size_t max_planes_per_batch = 64;
std::size_t max_local_pieces = 256;
std::size_t max_local_splitter_calls = 128;
std::size_t max_local_boolean_calls = 2048;
std::uint64_t max_local_elapsed_ms = 0;
std::size_t full_certificate_interval = 0; // 0 = terminal only
```

Keep legacy copied-model trials temporarily behind
`use_incremental_trials=false` as a qualification oracle. Remove that path only
after parity and performance acceptance criteria have held across the corpus.

### 9.2 CLI

Expose only options useful for bounded operation and diagnosis:

```text
--legacy-global-trials
--no-parallel-plane-batches
--max-parallel-families N
--max-planes-per-batch N
--max-local-pieces N
--max-local-splitter-calls N
--max-local-boolean-calls N
--max-local-elapsed-ms N
--full-certificate-interval N
--verify-incremental-state
```

Default `decompose-apply` should use incremental trials once qualification is
complete. The old behavior remains available during one compatibility cycle.

### 9.3 Lua/API

Add the same bounded fields to `s:decompose_to_fixed_point`. Return:

- local trials generated/accepted/rejected;
- plane batches evaluated;
- cache hits/misses;
- local and global Boolean calls;
- full reconciliations;
- final certificate status and digest.

No Lua- or LLM-supplied certificate measurement is trusted.

### 9.4 Qt viewer

For a locally rejected or unresolved target, provide a review item containing:

- parent and proposed children overlay;
- plane-family supports;
- local coverage/outside/overlap volumes;
- projected progress before and after;
- resource-limit or OCCT-unknown failure;
- recommended next action: alternate batch, targeted shatter, approximation
  review, or source-CAD repair.

Exact locally certified progress remains auto-applicable and only needs to be
reported. Unsafe/approximate geometry follows the existing user/authorized-LLM
decision policy.

### 9.5 `pi_csgify`

Update extension capability guidance after the new core is qualified:

1. start with fixed-point incremental decomposition and shatter disabled;
2. inspect structured progress and final certificate directly through the
   extension API;
3. request targeted shatter only for reported residual targets;
4. never enable overlap bypass for production MCNP;
5. preserve explicit user approximation budgets and ownership decisions;
6. report automatic exact splits and automatic budgeted removals in the final
   recipe.

The shipped prompt must not prescribe direct whole-document shatter for a
large assembly.

## 10. Reporting and observability

Extend `PerformanceMetrics` with additive fields:

```cpp
double local_analysis_ms;
double local_certificate_ms;
double local_overlap_ms;
double full_reconciliation_ms;
std::size_t local_trials_generated;
std::size_t local_trials_certified;
std::size_t local_trials_rejected;
std::size_t parallel_plane_batches;
std::size_t local_overlap_candidates;
std::size_t local_exact_overlap_tests;
std::size_t local_partition_boolean_calls;
std::size_t body_certificate_cache_hits;
std::size_t body_certificate_cache_misses;
std::size_t full_reconciliations;
std::size_t whole_model_trial_copies;
std::size_t peak_local_pieces;
```

Progress JSON and the final report should identify time in:

- proposal generation;
- OCCT split;
- child analysis;
- local coalescing;
- local partition proof;
- neighbour overlap proof;
- global reconciliation;
- final emit/export.

Add the certificate mode (`incremental+terminal-global` or legacy), final
certificate digest, policy digest, OCCT version, peak RSS when available, and
whether any approximation ledger entry was consumed.

This instrumentation is required before optimizing further; otherwise a fast
splitter can remain hidden behind repeated analysis or Boolean work.

## 11. Implementation phases

### Phase 0 — Reproducible benchmark and profiling harness

Deliverables:

- `benchmarks/decomposition/` runner with wall time, CPU time, peak RSS,
  splitter/candidate counters, output volume, residual count, overlap status,
  and certificate result;
- manifest support for external/licensed STEP inputs identified by SHA-256;
- Triangle baseline entry without copying the external STEP into the repo;
- comparable cold and warm runs;
- optional GEOUNED reference command kept outside normal tests;
- report artifact containing machine, compiler, OCCT, FreeCAD/GEOUNED, and
  csgify version information.

Exit criteria:

- the current approximately 140-second/one-target behavior can be reproduced
  or its updated baseline documented;
- timing categories sum plausibly to wall time;
- benchmark failure cannot be mistaken for a performance result.

### Phase 1 — Body certificates and algebraic progress ledger

Deliverables:

- `BodyProgressCertificate` and deterministic contribution function;
- active-body enumeration shared by analysis, overlap, and coordinator code;
- ledger build, replace projection, commit update, and invalidation;
- debug reconciliation against `collect_progress_state`;
- unit tests covering original solids, decomposed pieces, Boolean bodies,
  synthetic bodies, removal, and coalescing.

Exit criteria:

- ledger and full-state results agree on all fixtures;
- changing one target does not reanalyze unrelated targets;
- counters prove cache hits on a 100+ component synthetic assembly.

### Phase 2 — Local split result and mutation journal

Deliverables:

- geometry-returning local wrappers for face, plane, and multi-plane splits;
- immutable `LocalDecompositionTrial`;
- deterministic commit ordering and ID assignment;
- scoped mutation journal with rollback tests;
- existing public operations routed through the shared local core.

Exit criteria:

- evaluating a rejected candidate makes no `CadModel` changes;
- successful public operations retain current report/log/replay behavior;
- `whole_model_trial_copies` is zero in incremental mode.

### Phase 3 — Local partition certificate

Deliverables:

- fast validity/bbox/volume gates;
- exact containment, coverage, and sibling-disjointness checks;
- explicit unknown handling;
- canonical certificate digest;
- Boolean-call and elapsed-time budgets;
- adversarial tests for missing pieces, duplicate pieces, overhanging pieces,
  touching siblings, tiny positive overlaps, invalid solids, and Boolean
  failure injection.

Exit criteria:

- every deliberately corrupt partition is rejected;
- all existing exact split fixtures pass;
- cancellation preserves the last committed model.

### Phase 4 — Incremental neighbour overlap checks

Deliverables:

- in-memory active-body spatial index using the same bounds semantics as the
  project R*Tree/sweep prescreen;
- affected-neighbour query and canonical overlap delta;
- known/overlap/unknown state tracking;
- comparison with complete overlap detection in debug mode;
- tests with distant assemblies, touching faces, pre-existing overlap, new
  numerical overhang, and repeated source groups.

Exit criteria:

- local and full overlap sets agree after every mutation in the test corpus;
- a sparse 229-body assembly tests only local neighbours per candidate;
- unknown intersections remain blocking.

### Phase 5 — Local coalescing and coordinator integration

Deliverables:

- `coalesce_local_partition`;
- structural recognizers adapted to return local geometry;
- candidate projection using the ledger;
- atomic commit path;
- periodic and terminal reconciliation;
- legacy/global trial oracle switch.

Exit criteria:

- existing fixed-point, replay, cancellation, Qt, and union-emission tests
  remain green;
- incremental and legacy modes reach equivalent exact terminal geometry or
  produce an explained deterministic difference;
- no unrelated source group is coalesced or analyzed during a local trial.

### Phase 6 — Parallel face-plane batches

Deliverables:

- canonical plane-family extraction and ranking;
- bounded single-family and optional two-family candidates;
- recursive local queue;
- strategy evidence in reports and decision logs;
- tests for slotted rails, corrugated profiles, thin-wall channels, rotated
  geometry, coincident faces, noisy near-parallel planes, and no-op planes.

Exit criteria:

- a shape requiring a chain of sliver-like single cuts makes progress through
  one bounded batch without shatter;
- candidate ordering is deterministic across runs;
- plane and piece budgets stop combinatorial cases safely.

### Phase 7 — Reports, CLI, Lua, Qt, and Pi extension

Deliverables:

- policy fields and CLI/Lua bindings;
- additive performance/report JSON;
- progress event extensions;
- Qt local-partition visualization and evidence;
- `pi_csgify` capability, tool text, shipped prompt, and tests;
- documentation updates in decomposition, autonomy, and recipe guides.

Exit criteria:

- an LLM can run the fast safe default without reading JSON through ad-hoc
  Python;
- all automatic exact decisions appear in the final compact recipe;
- unsafe decisions still route through the configured human/LLM policy.

### Phase 8 — Qualification and default rollout

Qualification corpus:

- existing box/L-shape/pipe/extrusion/revolution/FWTBM1 fixtures;
- toy-car assembly;
- Triangle/large assembly by external digest;
- at least one ITER-port representative model;
- adversarial overlap and corrupted-partition fixtures.

Required checks:

- input/output and per-source volumes;
- residual and exact-body counts;
- positive-volume overlap and unknown overlap count;
- MCNP emission readiness;
- final STEP reload and reinspection;
- recipe replay equivalence;
- deterministic action ordering and certificate digests;
- wall time, CPU time, peak RSS, Boolean calls, and splitter calls;
- incremental-versus-legacy parity.

Rollout:

1. land incremental mode behind an opt-in switch;
2. run CI correctness tests and external nightly benchmarks;
3. enable it by default for `decompose-apply` and `pi_csgify` after acceptance;
4. retain `--legacy-global-trials` for one compatibility cycle;
5. remove the legacy trial copier only after no unexplained parity regressions
   remain.

## 12. Test matrix

### 12.1 Unit tests

- contribution addition/subtraction and floating-point bucketing;
- cache invalidation on every geometry-changing operation;
- canonical normal sign and plane-family grouping;
- deterministic plane signatures and ordering;
- local partition certificate failure modes;
- overlap-pair delta and unknown propagation;
- mutation-journal rollback;
- policy parsing and invalid-bound rejection;
- certificate digest changes on geometry, tolerance, policy, or OCCT version.

### 12.2 Integration tests

- fixed-point exact convergence without full model copies;
- partial progress followed by targeted shatter;
- cancellation between local OCCT calls;
- definition occurrence reuse under rigid transforms;
- coalescing followed by ledger reconciliation;
- final STEP reload preserves active geometry and volume;
- MCNP union emission retains one source component's semantic identity;
- pre-existing overlap remains blocking;
- explicit budgeted overlap ownership still produces one owner per space;
- replay regenerates rather than trusts stored certificates.

### 12.3 Differential tests

For each accepted local action in debug mode:

1. compute projected ledger state;
2. apply the action to a temporary reference model;
3. run the legacy complete analysis and overlap pass;
4. compare every progress field and overlap pair;
5. compare final active geometry by per-source symmetric difference where
   practical and by volume/topology certificates otherwise.

Randomized constructions should include separated boxes, touching boxes,
overlapping boxes, nested channels, rotated extrusions, repeated definitions,
and plane families perturbed around tolerance boundaries.

## 13. Performance acceptance criteria

Correctness gates take priority; a result failing any gate is not a valid
performance measurement.

For `Triangle.stp` on the qualification machine:

- no eager whole-document shatter;
- zero whole-model copies per ordinary local candidate;
- final overlap count and unknown-overlap count both zero unless the input
  baseline proves otherwise;
- final relative total-volume delta no worse than the configured exact budget;
- final STEP is written and reloads successfully;
- MCNP readiness is determined by the final global certificate;
- median of three warm csgify runs at most 90 seconds as the first rollout
  gate;
- stretch target at most 60 seconds, close to the observed 42.5-second GEOUNED
  complete run;
- no single ordinary component consumes more than 10 seconds without a
  progress event or explicit local budget stop;
- peak RSS remains bounded and is materially below the earlier unbounded
  all-plane failure mode.

The 90/60-second targets should be normalized or rebaselined when hardware,
OCCT, compiler, or build type changes. CI should enforce correctness and
operation-count ceilings; wall-time thresholds belong in a controlled nightly
environment to avoid flaky failures.

## 14. Risks and mitigations

### OCCT Boolean instability

- Treat failure as unknown and reject the local trial.
- Bound retries/tolerance escalation; never loop indefinitely.
- Record the operation and tolerance that failed.
- Preserve targeted shatter or human review as a fallback.

### Cache divergence

- Use geometry and policy revisions in every key.
- Provide debug reconciliation after every action.
- Run an unconditional terminal global certificate.
- Block emission on mismatch.

### Plane-batch explosion

- Deduplicate supports and offsets.
- Bound families, planes, pieces, depth, splitter calls, Boolean calls, time,
  and memory.
- Prefer one ranked family before combinations.

### Excessive local certification cost

- Apply bbox and volume rejection before exact Booleans.
- Sweep sibling bboxes rather than all-pairs Boolean testing.
- Query only affected external neighbours.
- Instrument every Boolean category before considering proof-preserving skips.

### Non-deterministic IDs or recipes

- Sort plane signatures, children, neighbours, and overlap pairs.
- Allocate public piece IDs only at commit.
- Include source digest and policy digest in audit evidence.

### Thread safety

- Keep one document's OCCT mutation/coordinator serial initially.
- Continue using bounded worker processes for parallel component/job execution.
- Do not add shared-thread OCCT parallelism until a separate measured and
  isolated design proves it safe.

## 15. File-level implementation map

Expected primary changes:

- `include/csgify/fixed_point.hpp`: additive policy, result, stop-reason, and
  telemetry fields.
- `src/session.cpp`: replace full-model trial validation with ledger projection,
  local trials, atomic commit, and reconciliation.
- `src/session_impl.hpp` / `src/session_view.hpp`: session-owned transient cache
  and revision access where appropriate.
- `src/cleanup.cpp`: geometry-returning split adapters, face-plane family
  generation, and bounded recursive local decomposition.
- `src/coalesce.cpp`: source-local coalescing entry point.
- `src/analyze.cpp` and `src/engine_internal.hpp`: shared active-body enumeration
  and reusable exact-overlap primitives.
- new internal `src/decomposition_ledger.*`: body certificates, algebraic state,
  invalidation, projection, and reconciliation.
- new internal `src/local_partition.*`: partition and neighbour certificates.
- `include/csgify/engine.hpp`, `src/report.cpp`, and Lua serialization: additive
  performance evidence.
- `src/main.cpp`: bounded CLI options and summary output.
- `src/viewer_qt.*`: rejected local-partition review visualization.
- `/home/giovanni/projects/pi_csgify`: capability contract, prompt, workflow,
  and tests after the core API is stable.
- `tests/`: focused unit, integration, differential, replay, and overlap tests.
- `benchmarks/decomposition/`: external-model manifest and reproducible runner.

Avoid placing the ledger or local-certificate types in the public engine model
unless an external caller truly needs them. Public reports should expose
evidence values; internal OCCT handles and mutable cache state remain private.

## 16. Definition of done

The work is complete only when all of the following hold:

1. ordinary candidate evaluation no longer copies and fully analyzes the
   complete assembly;
2. parallel face-plane batches are selected before targeted shatter for
   suitable analytic residuals;
3. local commit certificates prove validity, containment, coverage, sibling
   disjointness, volume conservation, and affected-neighbour safety;
4. cached projected state matches authoritative full analysis across the test
   and qualification corpora;
5. final STEP and MCNP publication always run a fresh global certificate;
6. overlap or unknown-overlap remains blocking for production MCNP;
7. exact decomposition decisions are autonomous, logged, and replayable;
8. approximation/removal/ownership decisions remain within explicit policy and
   the cumulative approximation ledger;
9. `pi_csgify` uses the incremental fixed-point stage before targeted shatter;
10. Triangle meets the correctness gates and rollout performance target;
11. FWTBM1, toy-car, ITER representative, and the complete existing test suite
    show no unexplained correctness regression;
12. documentation clearly distinguishes local commit evidence from the final
    global MCNP safety certificate.

## 17. Recommended execution order

The critical path is:

```text
benchmark instrumentation
  -> body certificate ledger
  -> immutable local split trials
  -> exact local partition certificate
  -> incremental neighbour overlap proof
  -> coordinator integration
  -> parallel face-plane batches
  -> UI/API/Pi exposure
  -> qualification and default rollout
```

Do not begin by tuning shatter, adding broad concurrency, or relaxing volume
tolerances. The measured bottleneck is repeated whole-document trial
certification. Removing that cost first creates the correct foundation for all
later decomposition strategies.

# Approximation Budgets and MCNP Geometry Safety

## Status

- **Implementation (2026-08-30):** Phases A-F are implemented. The common
  approximation ledger, transactional OCCT measurement gate, hard production
  overlap gate, explicit modeled-domain coverage, CLI/Lua/autonomy/replay,
  Qt evidence display, and STEP+MCNP output are active.
- **Qualification:** 44/44 automated tests pass. FWTBM1 exact shatter completed
  with 106 coalesced pieces, zero measured changed volume, and about 122 MB
  peak RSS. A toy-car run assigned its 0.0285133 mm^3 input overlap to one
  owner (0.00264% of the losing body), stayed near 88 MB peak RSS, committed
  certified partial decomposition, and stopped at the configured time bound
  rather than emitting an incomplete deck.
- **Overlap ownership extension:** an explicit approximation budget now permits
  deterministic single-owner resolution of small overlaps. The lower stable
  component ID owns the region by default; OCCT subtracts it from the other
  component, charges exact removed volume, records the decision, then
  decomposition and the hard final overlap gate run normally. Budget failure,
  Boolean uncertainty, protected geometry, or residual overlap still blocks.

- **Purpose:** allow explicitly bounded geometric approximation while making
  overlap and cell-coverage safety independent, non-negotiable final-emission
  conditions.
- **Primary workflow:** exact decomposition first, bounded approximation only
  after exact strategies stop, then mandatory MCNP geometry validation.
- **User-facing tolerances:** values such as `0.1%`, `1%`, or `2%`, supplied by
  the user and never inferred by an LLM.
- **Default:** exact geometry (`0%` approximation), no overlapping emitted
  cells, and no unknown overlap result.
- **Scope:** C++ policy and metrics, OCCT trial validation, shatter fallback,
  autonomy, CLI/Lua, JSON reports, Qt review, decision logs, recipes, Pi tools,
  and regression/performance tests.

## 1. Problem statement

Exact decomposition can stop with residual geometry because the remaining
pieces are numerically fragile, too small, unsupported, or too expensive to
split. For many engineering conversions a user may accept a controlled loss or
addition of volume—for example 0.1%, 1%, or 2%—to obtain a tractable MCNP cell
model.

Volume approximation and MCNP geometry validity are different policies:

- approximation changes the physical model and is acceptable only within an
  explicit, auditable user budget;
- overlap makes cell membership ambiguous and can produce geometry errors and
  lost particles, so no approximation budget may authorize it;
- an unclassified gap inside a declared modeled domain can likewise lose a
  particle, while an intentional void represented by an MCNP cell is valid.

The current controls are not sufficient:

- `allow_shatter_volume_drift` is a boolean rather than a measured budget;
- signed total-volume delta can hide compensating additions and removals;
- equal volume does not imply similar geometry—a translated or reshaped solid
  can have zero net volume delta;
- `allow_overlaps` can currently downgrade overlap errors and permit emission;
- gap/coverage validation has no explicit modeled-domain contract;
- approximation evidence is not accumulated consistently across operations,
  recipes, autonomy, and replay.

## 2. Policy decisions

### 2.1 Exact-first routing

The coordinator must try, in order:

1. exact fixed-point decomposition;
2. OCCT coalescing/unification;
3. exact bounded shatter on unresolved targets;
4. coalescing again;
5. approximate fallback only if the user supplied a non-zero policy;
6. mandatory MCNP geometry-safety validation;
7. STEP and MCNP emission only when their respective gates pass.

Approximation must not make exact candidates rank lower merely because an
approximate candidate is cheaper.

### 2.2 Percentage syntax

The CLI accepts percentages with an obligatory `%` suffix:

```sh
--approx-volume 1%
--approx-body-volume 2%
--approx-removed-volume 0.1%
```

This avoids ambiguity between `1` meaning 1% or 100%. Machine-readable APIs
and JSON store normalized fractions (`0.01`, `0.02`, `0.001`). Invalid,
negative, non-finite, or greater-than-100% values are rejected. Lua uses
normalized fractions and field names ending in `_fraction`.

### 2.3 Separate approximation metrics

Each trial records all of the following:

```text
before volume
after volume
signed volume delta
absolute net volume delta
removed volume = volume(before - after)
added volume   = volume(after - before)
changed volume = removed + added = symmetric-difference volume
relative values against the source/body baseline
maximum and RMS surface deviation when computable
topology and containment changes
```

The user's main percentage limits **changed volume**, not only signed net
delta. This rejects a same-volume replacement in the wrong location. Net,
added, and removed volume remain separately bounded and reported.

### 2.4 No cancellation of error

Global usage is accumulated from non-negative changed volume. Adding 1% in one
operation and removing 1% in another consumes 2%; it does not become zero.
Track the ledger per stable source-solid lineage so splitting and coalescing do
not reset a body's budget.

### 2.5 Overlap is independent and hard

For final MCNP emission:

- every positive-volume intersection between material cells is blocking;
- `overlap_unknown` is blocking;
- no volume-approximation setting relaxes either rule;
- touching faces/edges within OCCT numerical tolerance are not positive-volume
  overlap;
- pre-existing overlap is not grandfathered into a final deck;
- approximate trials that introduce or enlarge overlap roll back atomically.

A source overlap may be repaired, but never merely waived. With an explicit
approximation budget, csgify may assign the region to exactly one component and
subtract it from the other. The operation must measure as removal-only, fit all
action/body/document budgets, respect protected geometry, and pass the same
zero-overlap final gate. Stable-ID ordering supplies a deterministic default;
an LLM or human may instead propose an explicit owner, which is fully audited.

Retain `allow_overlaps` only for inspection, intermediate STEP work, and legacy
compatibility. Deprecate its ability to authorize production MCNP emission.
If an emergency unsafe-deck facility is retained for developers, it must use a
separate name, be unavailable to autonomy, stamp the deck/report as unsafe, and
never be the default or part of a recipe without an explicit unsafe marker.

### 2.6 Gap and coverage semantics

General gap detection is impossible without defining the domain that must be
covered. Add an explicit modeled-domain contract:

- user-selected container solid or generated bounding universe;
- list of material cells and intentional void cells;
- optional exterior/graveyard region.

Inside that domain, validate that every point/region belongs to exactly one
cell:

```text
membership count 0 -> unclassified gap (blocking)
membership count 1 -> valid
membership count >1 -> overlap (blocking)
```

Without a declared domain, csgify can certify non-overlap but must report
`coverage_status=not_configured`; it must not claim gap-free geometry. A void
is not a gap when csgify emits an explicit void cell covering it.

### 2.7 Protected geometry

Volume percentage alone cannot protect thin shielding walls or containment
boundaries. Support protected targets/materials and topology constraints:

- no removal or perforation of a protected wall;
- no new connected path across a closed barrier;
- no change to containment relationships without human review;
- independently bound removed material, because loss of shielding can be more
  consequential than addition;
- user/assembly metadata may mark materials or components as protected.

## 3. Public data model

Add a focused header, `include/csgify/approximation.hpp`.

### 3.1 `ApproximationPolicy`

```cpp
struct ApproximationPolicy {
    bool enabled = false;

    double max_global_changed_volume_fraction = 0.0;
    double max_body_changed_volume_fraction = 0.0;
    double max_action_changed_volume_fraction = 0.0;
    double max_global_added_volume_fraction = 0.0;
    double max_global_removed_volume_fraction = 0.0;
    double max_body_added_volume_fraction = 0.0;
    double max_body_removed_volume_fraction = 0.0;
    double max_surface_deviation = 0.0; // model units; 0 means exact only

    bool allow_surface_replacement = false;
    bool allow_sliver_removal = false;
    bool allow_bbox_replacement = false;
    bool allow_numerical_shatter_loss = false;
    bool allow_overlap_resolution = false;
    bool require_containment_preservation = true;
    bool require_topology_preservation = true;

    std::vector<std::string> protected_target_ids;
    std::vector<int> protected_material_ids;
};
```

All zero defaults preserve current exact behavior. Operation permissions are
separate because a user accepting 1% sliver removal has not necessarily
accepted a 1% bounding-box replacement.

### 3.2 `ApproximationMetrics`

```cpp
struct ApproximationMetrics {
    double baseline_volume = 0.0;
    double before_volume = 0.0;
    double after_volume = 0.0;
    double signed_volume_delta = 0.0;
    double absolute_net_volume_delta = 0.0;
    double added_volume = 0.0;
    double removed_volume = 0.0;
    double changed_volume = 0.0;
    double changed_volume_fraction = 0.0;
    double added_volume_fraction = 0.0;
    double removed_volume_fraction = 0.0;
    double max_surface_deviation = 0.0;
    double rms_surface_deviation = 0.0;
    bool symmetric_difference_certified = false;
    bool containment_preserved = false;
    bool topology_preserved = false;
};
```

Symmetric-difference computation uses OCCT Boolean cut in both directions.
Boolean failure produces `approximation_unknown` and cannot auto-apply.

### 3.3 Ledger and per-lineage accounting

```cpp
struct ApproximationLedgerEntry {
    std::string source_solid_id;
    double baseline_volume = 0.0;
    double cumulative_changed_volume = 0.0;
    double cumulative_added_volume = 0.0;
    double cumulative_removed_volume = 0.0;
    std::size_t accepted_actions = 0;
};

struct ApproximationLedger {
    double document_baseline_volume = 0.0;
    std::vector<ApproximationLedgerEntry> bodies;
    double cumulative_changed_volume = 0.0;
    double cumulative_added_volume = 0.0;
    double cumulative_removed_volume = 0.0;
};
```

The session owns the ledger. Atomic rollback restores geometry and ledger
together. Undo removes the corresponding ledger contribution.

### 3.4 MCNP safety result

```cpp
enum class CoverageStatus { not_configured, certified, gap, unknown };

struct McnpGeometrySafetyReport {
    bool safe_to_emit = false;
    std::size_t overlap_count = 0;
    std::size_t overlap_unknown_count = 0;
    double total_overlap_volume = 0.0;
    CoverageStatus coverage_status = CoverageStatus::not_configured;
    double uncovered_volume = 0.0;
    std::size_t multiply_classified_samples = 0;
    std::size_t unclassified_samples = 0;
    std::vector<Diagnostic> diagnostics;
};
```

## 4. OCCT measurement and validation

### 4.1 Trial measurement

For each approximate candidate:

1. copy the working model;
2. retain the original target shape or union of source-lineage pieces;
3. apply the candidate on the copy;
4. validate OCCT shape validity;
5. compute `before - after` and `after - before` with `BRepAlgoAPI_Cut`;
6. validate both Boolean results and calculate volumes;
7. compute containment/topology evidence;
8. coalesce where appropriate;
9. run global overlap detection against all active neighbors;
10. evaluate per-action, per-body, and cumulative ledger limits;
11. commit geometry and ledger atomically only if every gate passes.

Use bounding-box prescreens before Boolean differences and neighbor overlap
checks. Cache baseline/source-lineage compound shapes and spatial indexes.

### 4.2 Numerical uncertainty

Do not call failed Boolean measurement zero error. Classify it as unknown.
Separate kernel tolerance from user approximation:

- kernel tolerance answers whether a result is numerically distinguishable;
- approximation tolerance answers whether a measured physical change is
  acceptable.

Scale numerical thresholds from model units and body dimensions, but never
scale them up to the user's percentage budget.

### 4.3 Containment and barrier checks

At minimum detect:

- source child no longer contained where containment was previously present;
- new intersection with a containing or neighboring solid;
- shell count/cavity count changes;
- closed shell becoming open/invalid;
- protected thin-wall removal or new through-path indication.

Uncertain containment evidence routes to human review rather than automatic
acceptance.

## 5. Shatter integration

Shatter remains exact by default. Replace the boolean interpretation of
`allow_shatter_volume_drift` with measured outcomes:

- deprecate `--allow-shatter-volume-drift`;
- add `--approx-volume PERCENT` and operation-specific opt-ins;
- exact split candidates continue using strict conservation tolerances;
- after exact shatter exhausts its bounded search, optionally trial:
  - removal of certified numerical slivers;
  - dropping Boolean debris whose symmetric-difference contribution is known;
  - approved analytic surface replacement;
  - approved primitive/bbox replacement as the last strategy;
- every approximate shatter action uses the common ledger and overlap gates;
- never accept unexplained volume drift merely because net volume is below the
  requested percentage.

Add terminal reasons:

```text
exact
approximate_within_budget
approximation_budget_exhausted
approximation_measurement_unknown
protected_geometry
overlap_blocked
coverage_blocked
resource_budget
unsupported
```

The report distinguishes exact pieces, approximate pieces, and residuals.

## 6. MCNP geometry-safety gate

Implement one shared `validate_mcnp_geometry(...)` function and call it from
every emission path: direct CLI, session API, Lua, autonomy, replay, Qt, and Pi.

### Stage A: source geometry

- analyze every active material/void body;
- exact pairwise intersection with broad-phase acceleration;
- positive-volume intersections are errors;
- Boolean failures are `overlap_unknown` errors;
- confirm each body is valid and emit-representable.

### Stage B: emitted CSG representation

- construct/evaluate the final Alea cell expressions before writing the deck;
- test pairwise membership for candidate intersecting cell bounds;
- verify representative boundary-offset points on both sides of each surface;
- compare emitted-cell membership to OCCT source/accepted approximation using
  deterministic sampling plus exact checks where supported;
- fail closed on inconsistent surface senses or unknown classification.

### Stage C: domain coverage, when configured

- build `domain - union(all explicit cells)` to measure uncovered volume;
- ensure intentional empty regions are represented as void cells;
- run deterministic stratified and boundary-focused membership sampling;
- reject uncovered or multiply classified regions above numerical tolerance;
- record the seed/grid/settings so replay performs the same verification.

The deck is written to a temporary sibling and atomically renamed only after
all stages pass, so a failed gate never leaves an apparently valid partial
MCNP file.

## 7. CLI, Lua, JSON, and compatibility

### 7.1 CLI

Shared options:

```text
--approx-volume 1%
--approx-body-volume 1%
--approx-action-volume 0.1%
--approx-added-volume 1%
--approx-removed-volume 0.1%
--max-surface-deviation 0.05
--allow-approx-surface
--allow-approx-sliver-removal
--allow-approx-bbox
--protect solid:4
--protect-material 7
--mcnp-domain solid:1
--require-domain-coverage
```

Provide presets only as syntax sugar:

```text
--approx-preset conservative  # 0.1%
--approx-preset moderate      # 1%
--approx-preset permissive    # 2%
```

The report always expands a preset to explicit numeric fields. Never select a
preset automatically.

### 7.2 Lua/session API

Expose a typed approximation policy when loading/configuring a document and a
read-only ledger accessor. All approximate mutation methods return metrics and
the remaining budgets. Remove ad-hoc `max_rel_delta` interpretations over time
in favor of the common policy.

### 7.3 JSON report

Add top-level objects:

```json
{
  "approximation": {
    "enabled": true,
    "policy": {},
    "ledger": {},
    "within_budget": true
  },
  "mcnp_geometry_safety": {
    "safe_to_emit": false,
    "overlap_count": 1,
    "overlap_unknown_count": 0,
    "coverage_status": "not_configured"
  }
}
```

Each solid/piece and each decision record includes its approximation metrics,
source lineage, operation, and budget usage before/after.

### 7.4 Compatibility

- keep exact defaults unchanged;
- warn when `--allow-shatter-volume-drift` is used and map it to no automatic
  approximation until a numeric value is supplied;
- deprecate final-emission use of `--allow-overlaps` in one release, then make
  it inspection/intermediate-only;
- recipes containing an overlap override remain readable but fail the new
  production-emission safety postcondition unless explicitly run in a separate
  unsafe developer mode.

## 8. Autonomy and Pi extension

Extend `AutonomyPolicy` and `csgify_context` with the normalized numeric policy,
remaining ledger budgets, protected targets/materials, and safety state.

Rules:

- an LLM cannot invent or enlarge approximation limits;
- a request may spend only a policy already supplied by the user;
- exact operations remain preferred;
- approximate automatic application requires fresh OCCT symmetric-difference
  evidence, all limits passing, no protected/topology/containment concern, and
  zero new overlap;
- medium/unknown measurement, barrier changes, or coverage changes require
  human review;
- overlap and `overlap_unknown` are never auto-authorized; a budgeted
  single-owner subtraction is a repair and must end with zero overlap;
- final MCNP emit requires `mcnp_geometry_safety.safe_to_emit=true`.

Add operations/evidence fields for:

```text
approximation_budget_before/after
changed/added/removed volume fractions
surface deviation
containment/topology status
overlap status
coverage status
recommended next action
```

The Pi summary should say explicitly whether a run is exact, approximate within
the user budget, blocked by overlap, blocked by coverage, or awaiting review.

## 9. Qt viewer

Add an approximation-policy panel:

- exact / 0.1% / 1% / 2% / custom controls;
- separate added and removed limits;
- allowed operation checkboxes;
- protected target/material list;
- domain/coverage selection;
- consumed and remaining budget gauges.

For candidate review render:

- unchanged geometry in neutral gray;
- removed volume in amber;
- added volume in blue;
- overlap in opaque red;
- uncovered domain/gaps in magenta;
- protected geometry conflicts with a distinct warning overlay.

The approval dialog shows exact numeric evidence and requires a rationale when
automatic gates fail. The user may change the policy, but this becomes a
separate audited policy decision rather than silently approving the candidate.

## 10. Audit, compaction, and replay

Every accepted approximate action records:

- user-supplied policy and its origin;
- before/after source fingerprints;
- full `ApproximationMetrics`;
- ledger totals before and after;
- protected/containment/topology evidence;
- overlap and coverage result;
- actor, rationale, approval mode, and policy rule.

Compacted recipes retain the numeric policy and terminal postconditions:

```text
maximum cumulative changed/added/removed volume
maximum per-body changed/added/removed volume
maximum surface deviation
zero overlaps
zero overlap_unknown diagnostics
required coverage status
expected protected geometry state
```

Replay recomputes metrics on revised STEP geometry. It does not trust recorded
measurements. If the revised model consumes more budget or violates safety, it
stops before writing outputs and produces a review report.

## 11. Test strategy

### 11.1 Unit tests

- percent parser, including `0.1%`, `1%`, `2%`, malformed and non-finite input;
- changed/added/removed volume accounting;
- no signed-error cancellation;
- per-body lineage accounting across split/coalesce;
- ledger commit, rollback, and undo;
- policy boundary equality and floating-point bucketing;
- JSON and recipe round-trip.

### 11.2 Geometry fixtures

- exact box replacement: zero usage;
- clipped box with known 0.1%, 1%, and 2% removed volume;
- equal-volume translated box: net delta zero but large symmetric difference;
- tiny sliver whose removal is just below/above policy;
- thin protected wall where a small volume change opens a path;
- approximate replacement that introduces a tiny neighbor overlap;
- touching boxes with zero intersection volume;
- Boolean intersection failure producing `overlap_unknown`;
- container with an explicit void cell (valid);
- container with an unclassified gap (blocked);
- multiply classified region (blocked).

### 11.3 Workflow tests

- exact fixed-point/shatter ignores an available approximation budget until
  exact strategies stop;
- 0.1% fails while 1% succeeds for the same known candidate;
- per-body limit blocks an action even when global budget passes;
- cumulative changed volume blocks repeated small approximations;
- partial STEP is preserved after certified approximation progress;
- MCNP is never written with overlap, unknown overlap, or required-domain gap;
- autonomy cannot enlarge a user policy or authorize overlap;
- Qt accepts/rejects with the same policy evaluator as headless mode;
- compaction and replay reproduce the ledger and safety gate;
- cancellation/resource exhaustion preserves the last certified state.

### 11.4 Qualification models

- FWTBM1 exact decomposition remains volume-preserving and consumes zero
  approximation budget;
- toy-car assembly exercises exact shatter, optional approximate residual
  cleanup, and the known tiny inter-solid overlap as a hard MCNP blocker;
- a large assembly measures broad-phase overlap and symmetric-difference memory
  use under 64 GiB RAM.

## 12. Performance and memory controls

- reuse the fixed-point action, splitter, piece, candidate, depth, and elapsed
  budgets;
- add maximum symmetric-difference Boolean calls and cached-shape memory;
- broad-phase spatial indexing before pairwise overlap Boolean operations;
- stream progress at safe OCCT boundaries;
- cancellation preserves the last certified geometry and ledger;
- never hold all trial shapes for all candidates simultaneously;
- report peak active pieces, Boolean calls, cache size, elapsed time, and peak
  resident memory when available.

## 13. Implementation phases

### Phase A — policy and measurement foundation

1. Add `approximation.hpp`, policy, metrics, ledger, and percent parser.
2. Implement OCCT symmetric-difference measurement with fail-closed errors.
3. Integrate ledger transactionality with session mutation rollback/undo.
4. Add JSON reporting and unit/geometry tests.

**Exit criterion:** known fixtures measure added, removed, and changed volume
correctly; failed measurements cannot auto-apply.

### Phase B — common approximate mutation gate

1. Route surface, bbox/primitive replacement, sliver removal, and permitted
   shatter cleanup through one evaluator.
2. Add protected target, topology, and containment checks.
3. Remove operation-specific tolerance inconsistencies.
4. Preserve exact-first candidate ranking.

**Exit criterion:** every approximate mutation consumes one common ledger and
rolls back geometry plus accounting on any failed gate.

### Phase C — hard MCNP overlap gate

1. Add `McnpGeometrySafetyReport` and shared validation entry point.
2. Make positive overlap and unknown overlap block every production emitter.
3. Restrict/deprecate `allow_overlaps` for final MCNP output.
4. Write decks transactionally only after validation.

**Exit criterion:** no public production path can leave an MCNP deck when
source or emitted cells overlap or overlap status is unknown.

### Phase D — domain coverage and void cells

1. Add modeled-domain selection/generation.
2. Represent intentional voids explicitly.
3. Implement exact residual-domain measurement plus deterministic membership
   sampling.
4. Report `not_configured` honestly when no domain exists.

**Exit criterion:** configured domains certify exactly-one-cell coverage or
block emission with measured gap/multiple-membership evidence.

### Phase E — shatter/coordinator integration

1. Keep exact shatter first and bounded.
2. Add numeric approximate fallback and new terminal reasons.
3. Coalesce after each stage and update the shared ledger.
4. Produce partial STEP/review state when approximation or safety gates stop.

**Exit criterion:** user-selected 0.1%, 1%, and 2% policies behave predictably,
while the same overlap blocks all three.

### Phase F — autonomy, Qt, Pi, recipe, and replay

1. Expose immutable policy/budget context to LLMs.
2. Add Qt policy controls and geometry-difference overlays.
3. Extend audit records and compacted recipe postconditions.
4. Recompute all evidence during replay.
5. Update Pi summaries and workflow prompt guidance.

**Exit criterion:** CLI, Lua, Qt, autonomy, Pi, compact, and replay make the same
accept/review/block decision from the same geometry and policy.

### Phase G — qualification and rollout

1. Run exact and approximate regression matrices.
2. Qualify FWTBM1, toy car, and a large assembly under memory/time budgets.
3. Deprecate boolean drift and final overlap overrides with migration messages.
4. Document safe defaults, user responsibility, and MCNP failure modes.

**Exit criterion:** exact behavior remains backward compatible, approximation
is never implicit, and overlap/unknown/required-coverage failures cannot emit a
production MCNP deck.

## 14. Definition of done

This project is complete when:

1. users can explicitly request 0.1%, 1%, 2%, or a custom approximation;
2. the percentage is measured as changed volume with added/removed breakdowns;
3. per-action, per-body, and cumulative budgets are enforced without signed
   cancellation;
4. protected topology and containment changes route to review;
5. every approximate action is visible in reports, the viewer, logs, recipes,
   and replay;
6. no LLM can infer or enlarge an approximation budget;
7. no approximation setting can authorize MCNP cell overlap;
8. unknown overlap fails closed;
9. configured modeled domains reject unclassified gaps and multiple coverage;
10. failed validation never leaves a misleading MCNP artifact;
11. exact workflows consume zero approximation budget and remain the default;
12. all public interfaces share one policy evaluator and one final MCNP safety
    gate.

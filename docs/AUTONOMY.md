# Certified LLM Autonomy

## Boundary

An LLM never calls an unrestricted CAD mutation interface. It submits a
versioned action request; csgify recomputes the relevant proposal and geometry
evidence, evaluates a deterministic policy, and either applies the action
through the atomic `Session` API or escalates it to a human.

```text
LLM JSON -> fresh proposal -> policy + geometry certificate
         -> auto apply | Qt human review | deny
```

Atomic session validation still performs the final overlap and invalid-geometry
postconditions. A failed mutation rolls back even after policy approval.

## Request contract

```json
{
  "schema": "csgify.autonomy-actions",
  "schema_version": 2,
  "request_id": "request:fix-solid-1",
  "source_digest": "dff5aa386cae4b29b05d6da4ac0c2f8f",
  "expected_revision": 0,
  "actor": "llm:planner",
  "actions": [
    {
      "id": "fix-solid-1",
      "op": "apply_decomposition_candidate",
      "target": "doc:1/solid:1",
      "candidate": "best",
      "rationale": "The exact split resolves the convexity blocker."
    }
  ]
}
```

The request is untrusted. Schema 2 requires a stable `request_id`, the source
content digest supplied by `csgify context`, the document revision observed in
that context, and a nonempty `actions` array. The command rejects a request if
the STEP contents or loaded-document revision no longer match. Every action
must be an object with a nonempty, unique string `id` and `op`. Duplicate JSON
object keys, unknown request/action fields, unknown parameter fields, and wrong
field types are rejected before a queue is created. Requests are limited to 256
actions and rationales to 4096 bytes.

Arguments are checked against the operation contract. Solid and face operations
require a target; document operations reject one. Candidate operations require
a candidate, and operations that do not use candidates reject one. A
`medium_request` is accepted only by `apply_contained_medium`, where its object,
seed, and port fields are also closed to unknown keys. Unknown operations and
registered operations without an autonomy adapter remain parseable so they can
produce an explicit denied audit item.

Candidate metrics supplied by an LLM are ignored; csgify regenerates them.
Actor values may be `llm` or namespaced `llm:*` only, so a request cannot
impersonate a human.

Operation parameters are typed and normalized before evaluation. Currently
`max_depth` is accepted for budgeted shatter, must be an integer from 1 through
64, and defaults to 8. The normalized value is persisted as
`effective_parameters` and is used by both execution and restart.

## Modes

- `explain`: evaluates evidence but never mutates.
- `propose`: every otherwise-safe action enters human review.
- `auto`: certified actions execute; uncertain actions enter human review.

For example:

```sh
csgify autonomy model.step --actions actions.json --mode auto \
  --project model.csgify \
  --queue-report queue.json --log decisions.json \
  --out converted.i --out-step reviewed.step
```

With `--project`, project schema 8 atomically reserves `request_id` against the
submitted payload, source, authority policy, and preparation/conversion policy.
The resulting schema-4 queue is published as an immutable project artifact and
the request row transitions to `awaiting_decision`, `completed`, or `failed` in
the same SQLite transaction.

Submitting the same request again returns the stored queue and its previous
exit class without loading CAD, applying geometry, or opening a new decision
log. Reusing the ID with different bound input is rejected. A second caller
that reaches a still-running reservation receives an in-progress result rather
than executing the request concurrently. Running reservations carry a five
minute owner lease that is renewed every 30 seconds while the CLI owns active
work. An exact retry may reclaim an expired lease after a crashed process; the
former owner can no longer publish a result. Explicit `--resume`
claims a new lease before advancing an `awaiting_decision` result under newly
granted authority. Artifact indexing and the terminal request transition share
one SQLite transaction, so an interrupted transition leaves the request
recoverable and does not expose a partially indexed result.

Queue completion records the logical mutation result; it does not claim that a
caller-specific STEP or MCNP path was delivered. An exact retry of a completed
project request that supplies `--out` or `--out-step` reconstructs the stored
operations and writes the requested files without claiming a new request or
opening a duplicate decision log. A failed export leaves the completed queue
available for another delivery attempt.

Decision-log paths are validated before CAD is loaded. Opening the log,
writing its manifest, and every later append are checked. If an audit sink
fails before the first durable queue checkpoint, the project releases the
request claim immediately: a fresh reservation is removed, while a claimed
review/failed request is restored to its preceding state. No output is emitted
from an autonomy execution whose audit append reports failure.

Add `--review` to open the persistent multi-item queue in the Qt geometry
viewer. The queue window displays every proposal, its evidence, policy gate,
and state. `Review selected` opens the OCCT before/preview/applied geometry
review. **Evaluate certified changes...** first regenerates the automatic-policy
evidence, reports the exact applicable count, and asks for confirmation before
execution. It is not a blanket approval. After every accepted geometry
change, unresolved items are regenerated against the new document revision.

The Qt queue makes the trust boundary explicit. Its persistent header maps
`explain`, `propose`, and `auto` to **Advisory**, **Assisted**, and
**Delegated** authority and states what the agent can change. Selecting an item
shows three separate sections:

- **Agent explanation** — escaped, untrusted interpretation with actor and
  rationale;
- **Engine evidence** — regenerated usability, exactness, certificate,
  geometry metrics, and evaluated/current revisions;
- **Policy result** — disposition, immutable rule, and reason.

A pending item whose evaluated revision differs from the current session is
shown as `stale — re-evaluate`. Review is disabled rather than transferring an
old approval to new geometry. **Re-evaluate evidence** runs outside the Qt UI
thread, blocks close until its current OCCT operation reaches a safe boundary,
and only then replaces and persists the queue evidence.

A GUI acceptance is logged with `actor=human`, `proposal_actor=llm:*`, and
`approval_mode=manual`. A rejection is also durable: it records the rationale
in the queue and appends a non-mutating `kind=review` audit record when a
decision log is attached.

Exit status is `0` when the queue completed, `2` when human review remains, and
`1` for denied or failed actions. MCNP is not written while unresolved or
denied actions remain. A STEP may be written when certified partial fixed-point
progress was actually committed, so bounded shatter/review can continue from
it; a zero-progress attempt writes neither STEP nor MCNP.

### Contained-medium actions

A planner may request an already user-configured fluid region with a typed
payload:

```json
{
  "id": "water-segment-a",
  "op": "apply_contained_medium",
  "rationale": "Fill the declared cooling-water segment.",
  "medium_request": {
    "id": "medium:water-segment-a",
    "material_id": "material:7",
    "fill_mode": "full",
    "require_exact_geometry": true,
    "host_component_ids": ["inst:pipe-a"],
    "ports": [
      {"port_id": "port:1", "disposition": "cap_here"},
      {"port_id": "port:2", "disposition": "interface_to_external_model"}
    ]
  }
}
```

The LLM cannot authorize or define the material. Automatic execution requires
an immutable invocation/project policy such as:

```sh
csgify autonomy model.step --actions actions.json --mode auto \
  --allow-contained-medium material:7:liquid:0.997 \
  --queue-report queue.json --out model.i --out-step model.step
```

The material ID must currently end in its positive MCNP material number.
Without the option, with a different material ID, with unresolved ports, or
when fresh validity/leak/overlap certification fails, the action cannot be
automatically committed. The typed request and configured material policy are
stored in the durable queue for deterministic resume.

## Durable queues and resume

The schema-4 queue report is an executable-workflow checkpoint, not only a
summary. It stores the request ID, expected source revision, and a binding
digest over the source identity and normalized actions, alongside the source
STEP path/content digest, authority-policy digest, conversion-policy digest,
import preparation, normalized operation parameters, proposal origin,
regenerated evidence, execution state, and human rejection state. Resume a
stopped or reviewed run without asking the LLM to recreate its request:

```sh
csgify autonomy model.step --resume queue.json --mode auto \
  --queue-report queue.json --log decisions.json --out converted.i
```

Previously applied actions are reconstructed on a fresh session before pending
items are evaluated. Reconstruction replays the exact persisted candidate
through the atomic session API; it does not ask an already human-approved action
to pass the automatic policy again, and it does not duplicate the historical
decision-log record. Live application and reconstruction use the same typed
operation dispatcher, including approximation measurement and effective
parameters. Rejected items remain terminal and are never retried.
The request ID and digest remain unchanged on resume, completed items remain
terminal, and reconstruction is deliberately excluded from the new decision
log. This makes a queue resume one logical request rather than a second
mutation request.

When the queue is project-backed, callers do not need to retain its filesystem
path for later exact retries: the project database locates the immutable queue
artifact by `request_id`. The explicit queue report remains useful for human
review and portable audit exchange.

Legacy queues without these identities, an edited normalized action, changed
STEP contents at the same path, stale source revisions, edited
policy/preparation data, and conflicting resume-time preparation options all
require fresh evaluation. `--mode auto` remains an explicit
authority decision and may be supplied when resuming a proposal queue.
Preparation options supported by autonomy are `--units`, `--scale`,
`--normalize-units`, and `--no-heal`; when omitted during resume, their
persisted values are restored.
Queues can also be handled without a GUI:

```sh
csgify queue status queue.json --out -
csgify queue reject queue.json proposal-id \
  --rationale "intersects a protected region" --log decisions.json
```

## LLM reasoning context

`context` produces bounded, versioned, read-only input for a planning model:

```sh
csgify context model.step --target solid:1 \
  --history decisions.json --out context.json
```

The `csgify.llm-context` object contains document and selected-solid or
selected-face summaries, owning-solid and component-occurrence identity,
same-definition occurrence neighbours, material/protection semantics,
recognized pipe-port interfaces, local overlap neighbours,
diagnostics, current policy limits, freshly regenerated usable candidates, the
bounded autonomy vocabulary, and a bounded tail of prior decisions. Solids,
same-definition occurrences, each candidate family, and local overlaps carry
explicit offset-page state;
solid face lists carry explicit truncation state. Review history retains target,
candidate, actor, rationale, and policy rule. `context_limits` exposes the
count limits and serialized byte ceiling; an oversized result is not published.
Focused decomposition pieces are explicitly labeled `residual_piece`, retain
their piece ID for actions, and resolve owner semantics through their source
solid.
Each
context document includes the source digest and revision, and its
`action_contract` declares the required action schema version and binding
fields. Each advertised operation includes its argument schema, Qt
geometry-review support,
restart support, and a recovery action. The context currently exposes eight
operations with both a deterministic evaluator and restart implementation;
other registered engine mutations are denied through the untrusted autonomy
protocol. The context does not expose a mutation handle and does not trust
candidate metrics returned by the model. A planner responds with
`csgify.autonomy-actions`; csgify then recomputes all geometry evidence before
policy evaluation.

## Default policy

Autonomous decomposition requires all of:

- registered `apply_decomposition_candidate` operation;
- fresh candidate exists and is usable;
- high confidence;
- every piece exact CSG;
- regenerated preview count and volume match the proposal;
- no residual or sliver pieces;
- relative volume delta at most `1e-8`;
- minimum piece volume fraction at least `1e-8`.

Intermediate progress remains human-only unless explicitly enabled:

```sh
--allow-certified-intermediate 1e-8 \
--max-intermediate-pieces 16 \
--max-intermediate-residuals 4
```

This is not a general medium-confidence override. An intermediate split must
regenerate successfully, have medium or high confidence, introduce no slivers,
produce at least one newly exact piece, stay inside both piece and residual
budgets, contain no spline residual, and remain within the dedicated volume
delta. Its audit rule is `decomposition.certified_intermediate_progress`.
Orchestration must additionally monitor cumulative volume drift and global
residual growth; a locally valid split is not evidence that an unlimited chain
will converge.

The session-level fixed-point coordinator implements that additional global
boundary. It regenerates candidates on every working revision, trials them on a
copied model, coalesces with OCCT, and accepts only strict global progress under
explicit resource and cumulative-volume budgets. Its accepted operations use
the audit rule `decomposition.global_certified_progress`; speculative rejected
trials never appear as mutations in the decision log.

The queue distinguishes an attempted fixed-point pass from an applied mutation.
`evidence.mutation_applied` is true only when geometry changed, and
`requires_followup` is true when residuals need bounded shatter or human review.
A no-progress stop therefore reports `applied=0`, retains the stop reason and
residual count, returns status 2, and never unlocks final artifact emission.

Exact, high-confidence surface candidates may apply when their volume delta is
inside the same budget. Approximate surfaces require the explicit
`--allow-approximate-surface MAX_DEVIATION` option.

Approximate autonomous actions additionally require an immutable human-supplied
symmetric-difference budget. The LLM action schema has no budget field and
therefore cannot raise it:

```sh
csgify autonomy model.step --actions actions.json --mode auto \
  --approx-volume 1% --approx-shatter --queue-report queue.json
```

Supported directly budgeted autonomy operations are
`shatter_with_approximation`, `remove_residual_with_approximation`, and
`resolve_overlap_with_approximation`. Residual removal accepts decomposition
`piece:N` targets only and also requires `--approx-remove-residuals` plus
`--allow-topology-change`. Non-exact surface candidates require both
`--allow-approximate-surface DEV` and `--approx-volume P%`; OCCT must
materialize a closed owner solid so symmetric difference can be certified.
Unknown measurement routes to review or rolls back. The queue persists the
expanded policy and measured evidence, while MCNP overlap and coverage gates
remain non-overridable.

`resolve_overlap_with_approximation` uses the proposal's `target` as the
component that loses the shared region and `candidate` as the owner. It can be
auto-applied only when the immutable user policy enables
`allow_overlap_resolution`; the geometry gate then measures the cut and may
still reject it. Autonomy never emits overlapping cells: successful ownership
is a repair followed by a fresh zero-overlap check, not an overlap waiver.

Removal is human-only by default. It requires the explicit
`--allow-removal MAX_VOLUME_FRACTION` option and is still limited to recognized,
high-confidence removal candidates. Hole filling and every registered operation
without a dedicated safety evaluator are escalated.

`--max-auto-actions` limits mutations per queue. Every item is re-evaluated
against the revision produced by earlier items, preventing stale approvals.

## Audit and replay

Autonomous and manually approved decisions add an `audit` object to their
normal decision-log record:

```json
{
  "actor": "llm:planner",
  "proposal_actor": "llm:planner",
  "proposal_id": "fix-solid-1",
  "rationale": "...",
  "policy_rule": "decomposition.high_exact_certified",
  "approval_mode": "automatic"
}
```

The mutation remains a normal operation record, so compaction and replay use
the same deterministic recipe machinery as CLI, Lua, and human decisions.

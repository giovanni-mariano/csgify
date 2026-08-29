# Decision Log and Recipe

This document describes how `csgify` should persist preprocessing
decisions so that a conversion can be reproduced, audited, and re-applied
to a revised CAD model. It defines two artifacts that serve different
audiences:

- The **decision log** is the append-only journal of every command that
  was issued during a session. It records what happened, including
  dead-ends, undos, and exploratory steps. It is the source of truth for
  audit and review.
- The **recipe** is a minimal, deterministic, replayable script derived
  from the log by dropping any step that has no observable effect on the
  final model. It is the artifact consumers actually run.

Both artifacts live next to the STEP file in the user's repository. They
are tracked by the user's own `git`; `csgify` does not embed a VCS.

## Goals

- Make every human or agent decision durable and replayable.
- Allow re-conversion of a revised STEP file without redoing every choice.
- Produce a compact, hand-readable artifact that diffs cleanly across CAD
  revisions.
- Keep storage and format dependencies minimal: plain JSON for the log,
  Lua for the recipe (reusing the existing `lua_runner`).

## Non-Goals

- Do not embed a VCS, merge engine, or content store inside `csgify`.
- Do not invent a new scripting language for recipes. The Lua session API
  is sufficient.
- Do not silently apply uncertain matches across CAD revisions. Soft
  matches must be flagged in the report.

## Identity Across Revisions

The hard problem of replay is not file format; it is matching a logged
decision to a solid in a *different* STEP file. Every decision target
carries a layered identity:

1. **Primary**: `component_entry` — the XCAF instance label entry. Cheap
   and exact when the STEP tree is unchanged. See
   `project-solid-id-invariant` and `project-domain-mcnp` in agent memory
   for why the component entry, not the prototype, must be used.
2. **Fallback**: `fingerprint` — a tolerance-rounded tuple
   `(volume, surface_area, bbox_diagonal, face_count, centroid)`.
   Matches across revisions when labels moved but the part did not.
3. **Soft match**: if exactly one solid in the new model has a
   fingerprint within tolerance of a logged decision and no primary
   match was found, the decision is applied and flagged in the report.

Replay never silently applies a soft match without flagging it, and never
applies an op when more than one candidate matches within tolerance —
those go into a `needs_review` list.

## Decision Log

The log is a single JSON file, append-only. Each line of the array is one
decision record. The file is not rewritten when a decision is undone;
instead an inverse decision is appended.

### File header

The first element of the array is a manifest record, written once when
the log is created:

```json
{
  "kind": "manifest",
  "seq": 0,
  "csgify_version": "unversioned",
  "occt_version": "8.0.0",
  "step_path": "reactor_v3.step",
  "step_digest": "fnv1a128:a4f7...",
  "created_at": "2026-05-20T10:14:00Z"
}
```

The STEP digest uses FNV-1a-128, prefix-tagged (`fnv1a128:<hex>`) so a
future swap to SHA-256 (`sha256:<hex>`) is unambiguous. `csgify_version`
is currently the literal `"unversioned"` until the build exposes one.

On replay, the manifest is read first. A mismatching `step_digest` puts
replay into soft-match mode.

### Decision record

Every subsequent record has the same shape:

```json
{
  "kind": "decision",
  "seq": 7,
  "target": {
    "kind": "solid",
    "shape_id": "solid:5",
    "component_entry": "0:1:1:3:2",
    "fingerprint": {
      "volume": 12.473,
      "surface_area": 41.882,
      "bbox_diagonal": 6.221,
      "face_count": 8,
      "centroid": [0.0, 1.224, -0.501]
    }
  },
  "op": "replace_solid_fitted_cylinder",
  "params": { "max_rel_delta": 0.01 },
  "result": "applied",
  "produced": ["solid:5"],
  "issued_at": "2026-05-20T10:18:42Z"
}
```

Fields:

- `seq`: monotonically increasing integer assigned at write time. Used
  to deduplicate, reference, and undo. The manifest is `seq: 0`.
  (Causal ordering is currently implicit in `seq`; a content-hash `id`
  and explicit `parent_id` are a future addition once compaction needs
  to reorder records.)
- `target.kind`: `solid` | `face` | `model`. Model-scoped ops (`load`,
  `emit_mcnp`, `set_option`) carry no shape identity.
- `target.shape_id`: the id valid *at the moment the decision was
  issued*. Not stable across replay; informational only.
- `target.component_entry`, `target.fingerprint`: stable across replay,
  see "Identity Across Revisions" above. Both are present so the
  fingerprint can disambiguate when an entry collides.
- `op`: a vocabulary string from the session command set. The set is
  closed and versioned; unknown ops fail replay loudly.
- `params`: op-specific. Numbers must be serialized with full precision.
- `result`: `applied` | `failed` | `skipped`. Failed and skipped records
  are kept for audit.
- `produced`: shape ids the op created (decomposition pieces, synthetic
  bodies). Empty for ops that only modify or remove.

### Undo

To undo decision with `seq: N`, append a new record:

```json
{
  "kind": "undo",
  "seq": 12,
  "undoes_seq": 7,
  "issued_at": "..."
}
```

The log is never edited. Compaction handles cancellation.

## Recipe

The recipe is a Lua script produced by compacting the log. It uses the
same session API agents and humans call. Recipes are executable directly
via `csgify lua recipe.lua`.

### Generated structure

```lua
-- csgify recipe
-- generated from: decisions.log
-- log_digest:  fnv1a128:e1c8...
-- step_path:   reactor_v3.step
-- step_digest: fnv1a128:a4f7...
-- csgify:     unversioned
-- occt:        8.0.0
-- (version assert will be added once csgify.version() is exposed)

local s = csgify.session()
local doc = s:load_step("reactor_v3.step")

-- solid:5  (component_entry 0:1:1:3:2)
s:replace_solid_fitted_cylinder(doc, "solid:5",
                                { max_rel_delta = 0.01 })

-- solid:17  (component_entry 0:1:1:4:1)
s:remove_solid(doc, "solid:17")

-- solid:23  (component_entry 0:1:1:5:2) -> 3 pieces
s:decompose_planar(doc, "solid:23")
s:replace_solid_fitted_sphere(doc, s:piece(doc, "solid:23", 2),
                              { max_rel_delta = 0.005 })

s:emit_mcnp(doc, "reactor_v3.i")
```

Each command is preceded by a one-line comment showing the
`component_entry` so the script is human-readable and `git diff`-friendly
across CAD revisions.

### Compaction Rules

The compactor walks the log in `seq` order and applies these rules
(causal ordering will move to a `parent_id` chain once records can be
reordered for storage; today `seq` order is sufficient):

1. **Undone ops drop.** If a `decision D` is followed by `undo(D)`, both
   are removed from the recipe.
2. **Superseded ops drop.** If two ops target the same shape and the
   later op's effect supersedes the earlier (e.g., `replace_bbox` then
   `replace_fitted_cylinder` on the same solid), the earlier drops.
3. **Orphaned ops drop.** Any op whose target shape does not exist in
   the final model (e.g., a face tweak on a solid that was later
   removed) drops.
4. **Intermediate ops drop.** Ops on a decomposition piece that was
   itself later replaced wholesale drop.
5. **`load_step` and `emit_*` always survive.** They are the entry and
   exit points.

### Deterministic Ordering

Two recipes derived from runs that arrive at the same final model must
produce byte-identical Lua. The compactor sorts ops by:

1. Causal depth (topological order from `load_step`).
2. Within the same depth, by canonical hash of `(op, target_fingerprint,
   params)` ascending.

`load_step` is always first, `emit_*` always last.

### Verification

When writing a recipe, the compactor runs it once against the original
STEP and compares the resulting model's full geometric fingerprint to the
final state recorded in the log. If they differ, recipe writing fails
loudly. A recipe that diverges from its log is a bug, not a warning.

## Replay Against a Revised STEP

`csgify lua recipe.lua` is the normal happy path. For CAD revisions a
dedicated replay command handles soft matching and reporting:

```sh
csgify replay recipe.lua --step reactor_v4.step \
        --report replay-v4.json --out reactor_v4.i
```

The replay command:

1. Reads the recipe and overrides `step_path` with the `--step` argument.
2. For each op, attempts primary match (`component_entry`). If absent,
   attempts fingerprint match within tolerance.
3. Categorizes outcomes into `applied`, `soft_applied`, `unmatched`,
   `ambiguous`. Soft matches are applied; ambiguous and unmatched are
   skipped.
4. Lists new solids in the revised STEP that no recipe op targets, so
   the reviewer can decide whether they need new decisions.
5. Writes the categorization to `--report` and the converted deck to
   `--out`. Non-empty `ambiguous` or `unmatched` sections set the exit
   status to non-zero so CI fails until a reviewer regenerates the
   recipe.

## Backwards Compatibility

The op vocabulary is versioned together with `csgify`. When an op is
renamed or removed, the manifest's `csgify_version` lets replay produce
a clear error rather than silently misapplying. New ops always extend
the vocabulary; existing ops never change parameter semantics in a
breaking way without a version bump.

## Implementation Order

Build in this order so each step ships value on its own:

1. **Append-only log.** Every CLI and Lua command writes a decision
   record. No compaction yet. Already enables audit and exact replay
   against the same STEP.
2. **Recipe compactor.** Pure function on the log. Enables
   reproducibility, CI, and clean diffs across CAD revisions when the
   tree is unchanged.
3. **Soft-match replay.** Fingerprint fallback and replay report.
   Enables survival across CAD revisions.

Each step is useful even if the next is deferred. Step 3 is the hardest
and should not block 1 and 2 from landing.

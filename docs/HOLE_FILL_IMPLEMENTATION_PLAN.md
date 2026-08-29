# Hole Fill Implementation Plan

This plan covers the missing operation needed after semantic cleanup:
sometimes a component is removed because it is irrelevant for the neutronics
model, but the cavity around it should not remain as void. A common example is
removing a small bolt while filling the bolt hole with the surrounding host
material.

The important distinction is:

```text
remove-solid removes a CAD component.
fill-hole adds or repairs material that the simplified neutronics model should
contain.
```

These must not be the same hidden operation. Removing a bolt and filling its
hole is a modeling decision, not just a geometric side effect.

## Goals

- Detect small hole-fill opportunities after component removal.
- Let a human or agent decide whether a hole represents real void or material
  that should be restored.
- Start with simple cylindrical through-holes, because they cover many bolts,
  pins, and fasteners.
- Keep every fill operation explicit, logged, replayable, and volume-checked.
- Prefer adding a validated synthetic plug body for the first implementation,
  instead of modifying the host solid with expensive OCCT booleans.
- Integrate with existing decision requests, decision packets, Lua session
  workflow, STEP export naming, and future Alea emission.

## Non-Goals

- Do not build a general CAD feature-suppression system first.
- Do not silently fill every small hole in the model.
- Do not assume every removed fastener hole is irrelevant. Some holes are
  coolant, streaming, inspection, alignment, or air gaps.
- Do not require a GUI for the first version.
- Do not use OpenMP for this phase. The first performance wins should come from
  narrowing the search and avoiding unnecessary booleans.

## Design Rule

The first version should fill holes by adding explicit plug bodies:

```text
host solid with hole + synthetic fill plug
```

not by rewriting the host into:

```text
new fused host solid
```

Reasons:

- It fits the CSG target. A plug inside a void can become a normal Alea cell.
- It is faster than fusing every host with OCCT.
- It is easier to validate: the plug should have near-zero overlap with the
  host and should occupy the detected cavity.
- It keeps provenance visible: "this plug exists because solid:12 was removed."
- It is reversible in the decision log.
- It avoids changing host face IDs until we really need CAD-side healing.

A later optional phase can fuse the plug into the host for users who need a
single healed STEP solid, but that should not be the default neutronics path.

## User Model

When the tool proposes removing a component, the user should see four possible
semantic outcomes:

- `keep`: component matters; leave it unchanged.
- `remove`: component does not matter and the resulting cavity should remain
  void.
- `remove_and_fill`: component does not matter and the resulting cavity should
  become host material.
- `replace`: component matters only approximately; replace it with a simpler
  body such as a cylinder, box, sphere, cone, or CSG combination.

This makes the behavior clear. A bolt can be removed and its shaft hole filled,
while a coolant hole or streaming path can be preserved as void.

## Workflow Shape

The preferred agent workflow should be session/Lua first:

```lua
local s = csgify.session()
local doc = s:load_step("model.step")

local report = assert(s:analyze_table(doc))

local removals = assert(s:propose_removals(doc))
-- user/agent decides that doc:1/solid:12 is a bolt to remove
assert(s:remove_solid("doc:1/solid:12"))

local holes = assert(s:propose_hole_fills(doc, {
    near_removed = "doc:1/solid:12",
    max_radius = 5.0,
    max_volume_fraction = 1e-4
}))

-- user/agent reviews the candidate, or policy accepts high confidence
assert(s:fill_hole(doc, holes.candidates[1].id, {
    material = "host",
    max_host_overlap_fraction = 1e-8,
    max_other_overlap_fraction = 1e-8
}))

assert(s:emit(doc, "model.i"))
```

The CLI should expose the same primitive operations:

```sh
csgify hole-fill-suggest model.step \
    --near-removed solid:12 \
    --report hole-fills.json

csgify decision-pack model.step \
    --target hole-fill:1 \
    --out decisions/

csgify simplify model.step \
    --remove-solid solid:12 \
    --fill-hole hole-fill:1 \
    --out model.i \
    --report report.json \
    --log decisions.json
```

The exact CLI syntax can change during implementation, but the operation
vocabulary should remain stable:

- `propose_hole_fills`
- `fill_hole`
- `remove_and_fill` as a recipe-level combined intent

## Data Model

Add a small proposal model independent from OCCT-heavy view data:

```cpp
enum class HoleFillKind {
    cylindrical_through,
    cylindrical_blind,
    coaxial_stack,
    planar_wire_patch,
    unknown
};

struct HoleFillCandidate {
    std::string id;
    std::string host_solid_id;
    std::vector<std::string> seed_removed_solid_ids;
    std::vector<std::string> face_ids;
    HoleFillKind kind = HoleFillKind::unknown;

    Vec3 axis_origin;
    Vec3 axis_direction;
    Vec3 base;
    Vec3 height;
    double radius = 0.0;
    double top_radius = 0.0;
    double length = 0.0;
    double plug_volume = 0.0;

    double host_volume = 0.0;
    double plug_volume_fraction_of_host = 0.0;
    double estimated_host_overlap_fraction = 0.0;
    double estimated_other_overlap_fraction = 0.0;

    std::string confidence;
    double score = 0.0;
    std::vector<std::string> reason_codes;
    std::vector<std::string> blockers;
    std::string suggested_action; // fill, review, reject
};

struct HoleFillProposal {
    bool ok = false;
    std::vector<HoleFillCandidate> candidates;
    std::vector<Diagnostic> diagnostics;
};
```

The existing `SyntheticBody` can represent the first fill bodies. Add only the
minimal provenance needed for reporting and export:

```cpp
struct SyntheticBody {
    ...
    std::string role;              // replacement, hole_fill, user_added
    std::string source_solid_id;   // removed component when known
    std::string host_solid_id;     // host whose material is used
};
```

If adding fields to `SyntheticBody` creates too much churn, the first version
can encode this in `label`, but a real provenance field is cleaner once
decision packets and export naming use it.

For STEP export, synthetic hole fills should be named from the host:

```text
<host STEP name> / fill:hole
<host STEP name> / fill:hole from <removed STEP name>
```

The fill should remain a separate STEP solid in phase 1. Fusing it into the
host is a later optional operation.

## Detection Algorithm: Phase 1

Phase 1 should detect cylindrical through-holes related to a removed component.
This is the highest value case and the easiest to validate.

### Candidate narrowing

Do not scan every solid pair with booleans. Use cheap filters first:

1. Start from a removed solid when available.
2. Expand the removed solid bounding box by a search tolerance.
3. Find host solids whose bounding boxes intersect that expanded box.
4. Ignore hosts whose volume is smaller than the removed component unless the
   user explicitly asks.
5. Only inspect candidate host faces after this narrowing.

For standalone hole detection, require explicit CLI/Lua options such as
`--all-hosts` or `max_radius`. Standalone detection should produce review
candidates, not automatic fills.

### Face classification

For each candidate host:

1. Traverse faces with `TopExp_Explorer`.
2. Use `BRepAdaptor_Surface` to find cylindrical faces.
3. Group cylindrical faces by axis and radius within tolerance.
4. Reject external bosses by validation, not by fragile naming:
   - build the candidate plug;
   - compute overlap with the host;
   - a real cavity plug should have near-zero overlap with host material.
5. Estimate axial extent by projecting the cylindrical face bounds onto the
   cylinder axis.
6. Prefer candidates whose axis or bounding box intersects the removed solid.

The first version can use the cylindrical face bounding box projected on the
axis for length. Later versions can refine the aperture planes from adjacent
planar annular faces.

### Candidate construction

For each accepted cylindrical group:

1. Build a synthetic cylinder plug:
   - `base`
   - `height`
   - `radius`
2. Use the host material mode:
   - `material = host` when material metadata exists;
   - otherwise `default_material_id` plus a warning diagnostic.
3. Record linked evidence:
   - host solid ID;
   - removed solid ID when known;
   - cylindrical face IDs;
   - radius, length, volume, and confidence.

### Confidence

A candidate can be `high` confidence only when all of these are true:

- The hole is cylindrical.
- Radius is below the configured small-hole limit.
- Plug volume fraction of host is below the configured limit.
- Candidate plug has near-zero overlap with the host.
- Candidate plug has near-zero overlap with remaining non-removed solids.
- The removed solid intersects or closely follows the plug axis/bounding box.
- The removed solid has a hardware-like name or the user explicitly selected it
  for removal.

If any semantic signal is missing, confidence should drop to `medium` and the
decision packet should ask the user.

## Validation

Applying a fill must run validation before mutating the model:

- Plug shape can be built as an OCCT solid.
- Plug volume matches analytic volume within tolerance.
- Plug-host overlap is below `max_host_overlap_fraction`.
- Plug-other-solid overlap is below `max_other_overlap_fraction`.
- Plug volume fraction of host is below policy limit unless the user overrides.
- No unsupported spline/torus surface is introduced by the plug.
- The final report records the added volume and source decision.

The important volume checks are:

```text
host volume before fill
plug volume
plug volume / host volume
total active model volume before removal
total active model volume after remove
total active model volume after remove and fill
```

Do not use only total model volume as the decision guard. Removing a steel bolt
and filling the shaft with host material may change material composition even if
the total volume looks acceptable.

## Decision Requests

Add `DecisionRequest` entries of kind `hole_fill`.

Example JSON shape:

```json
{
  "id": "decision:hole-fill:1",
  "kind": "hole_fill",
  "target_id": "hole-fill:1",
  "reason": "removed component may leave a small cylindrical cavity",
  "recommended_action": "fill_hole",
  "summary": "Fill cylindrical hole in solid:3 after removing solid:12",
  "metrics": [
    { "name": "radius", "value": 2.5, "unit": "model" },
    { "name": "length", "value": 18.0, "unit": "model" },
    { "name": "plug_volume_fraction_of_host", "value": 0.00003, "unit": "ratio" }
  ],
  "candidates": [
    {
      "id": "hole-fill:1",
      "action": "fill_hole",
      "confidence": "high",
      "reason": "small cylindrical cavity aligned with removed solid"
    },
    {
      "id": "leave_void:1",
      "action": "leave_void",
      "confidence": "medium",
      "reason": "hole may represent real void or streaming path"
    }
  ]
}
```

Decision requests should be produced by `hole-fill-suggest` and by session APIs.
Normal `inspect` should not run expensive hole-fill detection unless requested.

## Decision Packets

Decision packets for hole fills should include:

- Overview: host and removed component highlighted in assembly context.
- Isolated host: cylindrical hole face highlighted.
- Candidate overlay: transparent plug body inside the hole.
- After view: host plus fill plug, with removed component hidden.
- Difference metrics: removed volume, plug volume, radius, length, volume
  fraction, overlap checks, and material mode.

The packet should ask one clear question:

```text
After removing this component, should csgify fill the detected hole with host
material or leave it as void?
```

The agent can show the packet to the user. If a project policy says "auto-fill
high-confidence bolt holes below X radius", the agent can apply the fill without
asking, but the decision log must still record it.

## Apply Operation

The first `fill_hole` implementation should:

1. Recompute or load the selected candidate.
2. Re-run validation.
3. Add a synthetic body with role `hole_fill`.
4. Assign material from host when available.
5. Append diagnostics and cleanup metrics.
6. Return an updated report.
7. Record the decision in the log/recipe.

It should not remove or rewrite the host solid.

The resulting active model contains:

```text
host solid with original hole
synthetic plug in the hole
other active solids
removed bolt hidden by removed_solid_ids
```

This is a correct CSG representation as long as the plug does not overlap host
material or remaining solids.

## Optional Later Operation: Fuse Fill Into Host

A later command can be added if users need a single repaired STEP solid:

```sh
csgify simplify model.step \
    --fill-hole-fuse hole-fill:1 \
    --out-step repaired.step
```

This would:

1. Build the plug.
2. Fuse plug and host with `BRepAlgoAPI_Fuse`.
3. Heal with `ShapeFix_Shape`.
4. Unify same-domain faces with `ShapeUpgrade_UnifySameDomain`.
5. Validate resulting volume.
6. Replace the host with a repaired solid.

This is not phase 1 because OCCT booleans are expensive and can destabilize
face identity.

## Material Handling

Hole fill material should default to `host`.

Until the material workflow is complete:

- Use `ConversionOptions::default_material_id`.
- Record `material_mode = host`.
- Emit a warning if the host has no known material metadata.
- Keep provenance so a later material-map pass can update the fill body.

When material assignment exists, `fill_hole` should copy:

- material ID;
- density;
- optional material label;
- source host ID.

If several host solids touch the hole, the fill should require review.

## Lua API

Add these session methods:

```lua
local proposal = s:propose_hole_fills("doc:1", {
    near_removed = "doc:1/solid:12",
    host = "doc:1/solid:3",
    max_radius = 5.0,
    max_volume_fraction = 1e-4
})

local report, err = s:fill_hole("doc:1", "hole-fill:1", {
    material = "host",
    max_host_overlap_fraction = 1e-8,
    max_other_overlap_fraction = 1e-8
})
```

`propose_hole_fills` should return a Lua table and JSON string, following the
current report style:

```lua
proposal.ok
proposal.candidates
proposal.diagnostics
proposal.json
```

`fill_hole` should return the normal analysis table after mutation.

## C API

Expose the minimum C ABI needed by future bindings:

```c
int csgify_session_propose_hole_fills_json(
    csgify_session_t* session,
    const char* document_id,
    const char* options_json,
    char** out_json,
    char** out_error);

int csgify_session_fill_hole(
    csgify_session_t* session,
    const char* document_id,
    const char* candidate_id,
    const char* options_json,
    char** out_report_json,
    char** out_error);
```

The C ABI should take JSON options to avoid constantly changing the ABI while
the hole-fill policy evolves. Internally the engine should still use typed C++
structs.

## CLI API

Add proposal command:

```sh
csgify hole-fill-suggest input.step \
    [--host solid:N] \
    [--near-removed solid:M] \
    [--max-radius value] \
    [--max-volume-fraction value] \
    [--report holes.json]
```

Add apply operation:

```sh
csgify simplify input.step \
    --fill-hole hole-fill:N \
    --out output.i \
    [--report report.json] \
    [--log decisions.json]
```

For multi-step command-line use, support remove plus fill in one invocation:

```sh
csgify simplify input.step \
    --remove-solid solid:12 \
    --fill-hole hole-fill:1 \
    --out output.i \
    --report report.json \
    --log decisions.json
```

If the current `simplify` parser cannot cleanly support two operations, prefer
adding a small `preprocess` command rather than making `simplify` confusing.

## Reporting

Reports should expose fill bodies clearly:

- `report.synthetic_bodies` or a future `report.fill_bodies`.
- `report.decision_requests` for unresolved fill choices.
- diagnostics with codes:
  - `hole_fill_candidate`
  - `hole_fill_needs_review`
  - `hole_fill_material_unknown`
  - `hole_fill_overlap_host`
  - `hole_fill_overlap_other_solid`
  - `hole_fill_applied`

The report should make it obvious when a model contains a synthetic fill body.
Agents should not need to infer that from the name.

## Recipe and Replay

The decision log should record `remove_and_fill` as two explicit decisions or
as one high-level intent with two produced effects.

Preferred log shape:

```json
{
  "kind": "decision",
  "op": "remove_solid",
  "target": { "shape_id": "solid:12", "component_entry": "..." },
  "result": "applied"
}
```

```json
{
  "kind": "decision",
  "op": "fill_hole",
  "target": { "shape_id": "hole-fill:1" },
  "params": {
    "host": "solid:3",
    "near_removed": "solid:12",
    "radius": 2.5,
    "axis": [0.0, 0.0, 1.0],
    "material": "host",
    "max_host_overlap_fraction": 1e-8
  },
  "result": "applied",
  "produced": ["synthetic:7"]
}
```

Replay should not rely only on `hole-fill:1`, because candidate IDs are
proposal-local. It should rematch using:

- host component entry/fingerprint;
- removed solid component entry/fingerprint when present;
- hole radius;
- axis direction;
- center/base point tolerance.

If replay finds multiple possible holes, it should stop with `needs_review`.

## Performance Plan

The performance rule is: narrow first, boolean last.

Implementation details:

- Use bounding boxes to find candidate hosts near removed components.
- Cache face classification from analysis when possible.
- Only inspect cylindrical faces for phase 1.
- Do not compute `BRepAlgoAPI_Common` for candidates that fail bbox/radius
  filters.
- Limit candidates by `--max-candidates`.
- Add performance counters:
  - `hole_fill_detect_ms`
  - `hole_fill_validate_ms`
  - `hole_fill_candidates_evaluated`
  - `hole_fill_boolean_calls`
- In a session, reuse the already-loaded OCCT document. Do not reload the STEP
  file between remove, suggest, decision-pack, and apply.

For 100 MB STEP files, the intended workflow is:

```text
load once in session
analyze once
remove/select candidate solids
run hole-fill detection only near those solids
apply explicit fills
emit
```

No OpenMP is needed for the first useful version.

## Implementation Phases

### Phase 1: Proposal Contract

Add the data structures, JSON serialization, Lua table conversion, and CLI
proposal shell.

Acceptance:

- `hole-fill-suggest` can return an empty valid proposal.
- Lua can call `propose_hole_fills`.
- Reports and decision requests can represent `hole_fill`.
- Existing tests still pass.

### Phase 2: Cylindrical Through-Hole Detection

Implement detection for simple cylindrical through-holes near a removed solid.

Acceptance:

- A block-with-hole fixture produces one candidate.
- Candidate includes host ID, removed ID, cylindrical face IDs, radius, length,
  plug volume, and confidence.
- External cylindrical bosses are rejected by overlap validation.

### Phase 3: Apply As Synthetic Plug

Implement `fill_hole` by adding a synthetic cylinder body.

Acceptance:

- Removing a bolt and filling the hole creates an active synthetic plug.
- Plug is emitted as CSG.
- Plug appears in reports and STEP export with meaningful provenance.
- Volume checks are reported.

### Phase 4: Decision Packet Integration

Add visual evidence for fill decisions.

Acceptance:

- `decision-pack` writes a packet for `hole_fill`.
- SVG highlights host, removed solid, hole face, and plug overlay.
- `decision.md` asks the user whether to fill or leave void.

### Phase 5: Coaxial Stacks

Support counterbores, countersinks, and stepped holes as a union of simple
analytic fill bodies.

Acceptance:

- Coaxial cylindrical/conical segments are grouped.
- Candidate can represent a stack of cylinders/cones.
- User can fill the whole stack or only the shaft.

### Phase 6: Blind Holes

Support blind cylindrical holes.

Acceptance:

- Candidate detects one opening and one bottom face.
- Plug depth is bounded by the bottom face.
- Validation rejects plugs that overlap host material.

### Phase 7: Optional Host Repair

Add explicit fuse-based host repair only if users need clean single-solid STEP
output.

Acceptance:

- `fill_hole_fuse` creates a valid repaired host.
- It is clearly slower and more fragile than plug mode.
- It preserves host naming and records provenance.

## Test Plan

Add generated fixtures:

- block with through cylindrical hole plus removable bolt cylinder;
- block with through hole plus bolt head/washer to prove only the shaft is
  filled;
- block with external cylindrical boss to prove it is not treated as a hole;
- blind cylindrical hole;
- counterbore/countersink later.

Add tests:

- `hole_fill_suggest_smoke.sh`
- `hole_fill_apply_smoke.sh`
- Lua workflow test for remove plus fill.
- STEP export smoke test for fill body names.
- Decision packet smoke test for `hole_fill`.
- Replay test where the same hole is rematched from host/removed fingerprints.

Important assertions:

- Candidate count is deterministic.
- Plug volume matches analytic cylinder volume.
- Host overlap is below tolerance.
- Fill body has role `hole_fill`.
- Existing emit tests still pass.

## Risks

- A small hole may be physically important. This is why automatic filling must
  be policy-driven and logged.
- Threaded holes may look like complicated spline or helical geometry. Phase 1
  should not try to understand threads; it should propose a cylinder only when
  the fit is clearly within tolerance.
- OCCT boolean overlap checks can be expensive. Keep them behind bbox and
  radius filters.
- Material assignment is not fully solved yet. Fill bodies must carry host
  provenance so material can be corrected later.
- STEP export as separate plug solids may surprise users expecting one fused
  host. The report and names must make this explicit.

## First Development Slice

The first useful patch should be small:

1. Add `HoleFillCandidate` and `HoleFillProposal`.
2. Add JSON and Lua table serialization.
3. Add `hole-fill-suggest` that returns empty proposals plus diagnostics.
4. Add decision request kind `hole_fill`.
5. Add fixture generator for block, hole, and bolt.

Then implement detection and apply in the following patches. This keeps the
public contract visible before the geometry code grows.

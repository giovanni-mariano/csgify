# Alea Boundary and Human Feedback

This note records an architectural decision: `csgify` should not become a
second GEOUNED-style CSG system. `csgify` is the CAD front-end. Alea is the CSG
backend.

The practical pipeline is:

```text
OCCT STEP
  -> assembly and shape analysis
  -> semantic cleanup proposals
  -> spline/surface replacement proposals
  -> decomposition into CSG-convertible pieces
  -> Alea surface and cell construction
  -> Alea simplification, void generation, and export
```

## Responsibility Split

`csgify` owns the parts that require CAD knowledge:

- STEP import through OCCT 8.
- Assembly, name, hierarchy, volume, bounding-box, and face analysis.
- Human/agent decisions about removing useless components.
- Replacement of CAD details with simplified bodies.
- Spline and unsupported-surface treatment.
- Decomposition of solids into CSG-convertible pieces.
- Decision reports, diagnostics, replay logs, and user-facing evidence.

Alea owns the CSG model:

- Primitive/surface registration.
- Primitive deduplication.
- Halfspace nodes.
- CSG operations.
- Cell storage.
- Cell simplification.
- Surface IDs for export.
- Void generation.
- MCNP/OpenMC/Serpent export.

This means `csgify` should not build a permanent global surface registry. It
may keep a transient CAD-side map during emission:

```text
OCCT face id -> Alea surface index -> halfspace sense
```

That map is only for diagnostics, reports, replay, and explaining what was
emitted. Alea remains the authoritative owner of primitive identity and surface
deduplication.

## Consequence for GEOUNED Ideas

GEOUNED remains useful as a source of algorithms, but not as an architecture to
copy wholesale.

Ideas worth borrowing:

- Plane-first decomposition.
- Analytic second-order surface decomposition.
- Extra planes for partial cylinders, cones, spheres, and torus faces.
- Torus-specific annex surfaces or split planes.
- Conservative volume checks after decomposition or replacement.
- Complexity thresholds to stop automatic processing and ask for a decision.

Ideas that should stay on the Alea side, not in `csgify`:

- A permanent surface registry.
- Final CSG boolean storage.
- Final cell simplification.
- Void generation.
- Export-specific surface numbering.

The boundary should stay simple: if geometry is not ready, `csgify` refuses to
emit it. If geometry is ready, emission into Alea should be boring.

## Human Feedback

JSON reports are necessary for agents, but they are not enough for human
decisions. When a choice is semantic or approximate, the user needs visual
evidence.

The rule should be:

```text
Ask the human only when the automatic choice is not obvious, and when asking,
show pictures plus the metrics.
```

Examples where visual feedback is needed:

- Removing a component because it looks like a bolt, washer, logo, label, or
  irrelevant support detail.
- Replacing a solid with a box, cylinder, sphere, cone, or other simplified
  body.
- Choosing between several decomposition candidates with similar scores.
- Accepting a spline replacement with measurable geometric deviation.
- Accepting a torus or blend treatment.
- Accepting a volume change above a project default tolerance.

Examples where visual feedback is usually not needed:

- A solid is below a user-approved volume threshold and has a clear hardware
  name.
- A spline face fits a plane/cylinder/sphere/cone within a strict tolerance.
- A decomposition candidate is high confidence, volume-preserving, and all
  produced pieces are directly convertible.

## Decision Packets

For every non-obvious decision, the engine should be able to write a decision
packet directory:

```text
decision-solid-7/
  manifest.json
  decision.md
  overview.svg
  target.svg
  candidate-001.json
  candidate-001.svg
  candidate-001-after.svg
  candidate-001-overlay.svg
  candidate-002.json
  candidate-002.svg
  candidate-002-after.svg
  candidate-002-overlay.svg
  preview.html
```

The packet should be usable by both humans and agents:

- `manifest.json` is the stable machine-readable contract.
- `decision.md` is a compact human summary.
- Image files show the evidence. SVG is the preferred first implementation
  because it is easy to produce headlessly; PNG can be added later as an
  optional renderer.
- `preview.html` gives a local browser view without requiring a CAD program.

The first version does not need a full GUI. Static images plus HTML are enough.

## Required Images

A useful packet should include at least these views:

- **Overview:** full model or assembly context, with the target highlighted.
- **Isolated target:** target solid or face alone, with axes and scale.
- **Candidate overlay:** proposed replacement or split plane over the original.
- **After view:** resulting simplified/decomposed pieces.
- **Difference view when applicable:** original transparent, replacement solid
  opaque, with volume and deviation metrics in the metadata.

For removal decisions:

- Highlight the candidate component.
- Show neighboring solids for context.
- Show its assembly path/name if available.
- Show volume fraction and bounding-box fraction.

For replacement decisions:

- Show original solid.
- Show simplified body overlay.
- Show volume delta and fitted primitive parameters.

For decomposition decisions:

- Show the selected face or plane.
- Show split pieces in different colors.
- Show which pieces are now exact CSG convertible.

For spline decisions:

- Show the original face.
- Show the fitted analytic surface.
- Report max deviation, RMS deviation if available, and area/volume impact.

## CLI Shape

The interface should stay command-line first. A future command could be:

```sh
csgify decision-pack model.step \
    --target solid:7 \
    --kind decomposition \
    --out decisions/solid-7
```

Or for all unresolved choices:

```sh
csgify decision-pack model.step \
    --from-report report.json \
    --out decisions/
```

The normal workflow becomes:

```sh
csgify inspect model.step --report report.json
csgify decision-pack model.step --from-report report.json --out decisions/
# human or agent reviews the packets
csgify preprocess model.step --recipe accepted.lua --out simplified.step
csgify emit simplified.step --out model.i
```

An agent can read `manifest.json`, show the images to the user if its interface
supports images, and write a replayable recipe after the user chooses.

## Report Contract

Decision reports should point to visual evidence without requiring the agent to
guess filenames:

```json
{
  "decision_id": "decision:solid:7:decomposition",
  "target_id": "doc:1/solid:7",
  "reason": "multiple_medium_confidence_candidates",
  "recommended_action": null,
  "packet_dir": "decisions/solid-7",
  "images": {
    "overview": "overview.png",
    "target": "target.png"
  },
  "candidates": [
    {
      "id": "candidate:1",
      "action": "decompose_by_face",
      "confidence": "medium",
      "image": "candidate-001-overlay.png",
      "after_image": "candidate-001-after.png",
      "volume_rel_delta": 2.0e-7,
      "pieces": 3,
      "exact_pieces": 3
    }
  ]
}
```

The report should remain useful without images, but images should be available
whenever the user is expected to judge geometry.

## Implementation Direction

The first implementation should be deliberately small:

1. Add an internal `ViewRequest` structure that can name a target solid/face,
   optional highlighted faces, optional split planes, and optional replacement
   bodies.
2. Add a renderer wrapper that can produce static image files from OCCT shapes.
3. Add `decision-pack` for decomposition proposals first, because that is where
   the current engine already has candidate data.
4. Reuse the same packet format for removal and replacement proposals.
5. Add a `preview.html` writer only after PNG generation is working.

Rendering should not be mixed with geometry decisions. The decision code should
produce facts and candidate geometry; the renderer should only draw those facts.

## Performance Notes

For large STEP files, decision packets must be bounded:

- Render only unresolved decisions by default.
- Limit the number of candidate images per target.
- Prefer isolated target views over full assembly views.
- Cache imported documents inside the session.
- Do not re-run full analysis for every image.
- Allow `--max-packets` and `--max-candidates-per-packet`.

The goal is to make human review possible without turning preprocessing into a
slow visual export job.

## Architectural Rule

When in doubt:

```text
csgify explains and prepares geometry.
Alea owns CSG identity, cells, voids, and export.
Humans see pictures when judgment is required.
Agents get structured data plus links to those pictures.
```

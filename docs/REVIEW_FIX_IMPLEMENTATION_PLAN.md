# Review Fix Implementation Plan

This plan implements the high-risk issues found in the deep review of the
current working tree.

## 1. Target identity and removed solids

- Add one active-solid resolver in `cleanup.cpp`.
- Reject any root solid whose local `solid:N` id is present in
  `CadModel::removed_solid_ids`.
- Use the resolver for solid removal, solid replacement, and decomposition.
- Preserve piece lookup for active decomposed pieces.
- Add smoke coverage for "remove then mutate same solid".

## 2. Complete final reports

- Keep `convert_model()` as the emitter-facing conversion result.
- Build the public report returned by `emit_document()` from a fresh
  `analyze_model()` pass after conversion/export diagnostics are known.
- Preserve conversion counters from the conversion report.
- Make `simplify --report` describe the final active model, including
  surviving solids and synthetic replacements.

## 3. Scoped session reports

- Scope all report references, not only primary nodes.
- Rewrite overlap pair ids and decision request target ids through the same
  `scoped_shape_id()` helper.
- Keep local reports local when callers request `ReportIdMode::local`.

## 4. Checked replacement replay and recipes

- Preserve checked replacement guard parameters when emitting recipes.
- Replay checked operations through `apply_checked_replacement_operation()`.
- Fail replay when guards are missing or exceeded.

## 5. Overlap boolean failures

- Stop treating OCCT boolean exceptions as "no overlap".
- Emit an `overlap_unknown` error diagnostic when a boolean intersection
  cannot be evaluated.
- Let `allow_overlaps` downgrade the diagnostic to a warning.

## 6. False-passing tests and synthetic metadata

- Make `tests/id_stability_smoke.sh` propagate the Python validator status.
- Populate bboxes for synthetic spheres and cones.
- Add or extend smoke assertions for stable reports and synthetic bounds.

## 7. C API and decision log

- Make C API command status respect `ConversionReport::ok()`.
- Change the decision log writer to append newline-delimited JSON records so
  a crash leaves prior complete records readable.

---
name: csgify-ui-design
description: Design, prototype, implement, or review csgify's Qt Widgets CAD-to-CSG workspace, including its OCCT viewport, assembly tree, geometry decisions, and project jobs. Use for csgify UI/UX work, not unrelated websites or engine-only changes.
---

# csgify UI design

Design for an engineer converting CAD into traceable CSG. The main interaction
is selecting geometry, understanding its conversion status, inspecting a
proposed change, and choosing the next action. Keep the geometry and its
evidence easy to compare.

## Ground the design in the application

Work from the csgify repository. Read `docs/QT_AGENT_WORKSPACE.md` for the
workspace and review semantics, and the relevant checkpoint in
`plans/UX_FIRST_HUMAN_LLM_WORKSPACE_IMPLEMENTATION_PLAN.md` for current gaps.
Inspect `src/viewer_qt.cpp`, the relevant `src/qt/` components, and
`src/operation_registry.cpp` before proposing controls. Treat source as the
authority on implemented capabilities; roadmap examples may describe future
behavior. When those paths are unavailable, request the project location.

For visual direction, use `frontend-design` when available. Translate its
principles into a desktop engineering workspace: legibility, alignment,
information hierarchy, and deliberate color. Let the user's brief decide the
style. Keep this companion usable without that skill.

## Design the workflow and states

Identify the user's immediate task and the relevant window: project overview,
geometry inspection, candidate review, autonomy queue, or coverage review.
For a substantial redesign, sketch the layout and the important state changes
before implementing. For a narrow change, work directly in the existing UI.

Account for the states relevant to the task: empty project, loading, selected
geometry, unresolved geometry, proposal generation, preview, stale evidence,
running/cancelling work, failure, partial result, and verified export.
Distinguish an empty selection from an unavailable operation. Explain a
blocker with a concrete next action. Queue completion and conversion readiness
are different facts and should have separate labels.

Use realistic model names, units, counts, material labels, and failure messages
in prototypes. Label simulated capabilities and results so a mockup does not
imply that a missing engine or review adapter works.

## Workspace and visual hierarchy

Give the OCCT viewport the primary continuous working area. Use a resizable
assembly tree, an inspector for the current selection or proposal, and
collapsible history/job details. Preserve selection context while the user
switches between evidence and alternatives. Avoid modal navigation for routine
inspection; use dialogs when a scoped decision needs focused confirmation.

Keep frequent actions close to the selection they affect. Prefer standard Qt
trees, tables, forms, splitters, docks, and toolbars. Group controls by the
user's task; show advanced tolerances and diagnostics on demand. Keep units
visible beside editable tolerances and measured values. Align comparable
numbers and use consistent precision that reflects the evidence.

Use a small coherent palette and spacing/type scale. Derive current viewport
colors from the implementation before changing them: cyan selection, green
exact preview pieces, orange residuals, magenta slivers, and translucent blue
support planes already have meanings. Preserve those distinctions or update
the renderer, legend, and tests together. Explain states in text or icons as
well as color. Check focus, disabled controls, contrast, and long labels.
Keep overlays and the reference grid subordinate to the geometry.

## Decision and authority semantics

Present the affected target, intended change, alternatives, engine evidence,
uncertainty, budget impact, and next action in that order when applicable.
Keep agent rationale visually distinct from engine measurements and policy.
Name authority modes consistently: Advisory, Assisted, and Delegated.

Retain Before/Preview/Applied distinctions. A preview does not mutate the live
document. Stale evidence must visibly disable acceptance until regenerated.
Missing geometric proof cannot be overridden by dismissing a warning.
Removal uses its existing scoped confirmation; preserve deliberate keyboard
acceptance behavior and do not add unmodified mutation shortcuts.

Check review support in the operation registry. For an operation without an
adapter, expose a supported alternative, source repair, retention of a
residual, or suspension as appropriate. A design change alone does not supply
the missing implementation. Route all implemented mutations through Session
and existing policy checks, journal, and decision log.

## Implement and verify proportionately

Keep the existing Qt Widgets and native OCCT integration unless a framework
change is part of the request. Use layouts and size policies instead of fixed
positions. Keep expensive geometry work in the existing job layer and ensure
progress, cancellation boundaries, and close behavior remain understandable.
Do not expose kernel implementation details unless they help an engineering
decision.

For layout or visual changes, inspect a screenshot or live rendering when
available, including a smaller window and long content. Browser prototypes
help evaluate navigation and layout but do not validate the native viewport.
Offscreen Qt tests validate widget behavior, not OpenGL appearance. State the
validation limit when visual inspection was unavailable.

For behavior changes, run the relevant existing Qt tests, typically
`csgify_qt_decision_widget` or `csgify_qt_cad_job_controller`; include project
workspace qualification when that flow changes. Add tests only for meaningful
new behavior or regressions. Deliver the design artifact or implementation,
explain its workflow benefit, and identify any unsupported states that remain.

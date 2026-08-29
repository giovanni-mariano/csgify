# Qt Agent Workspace

## Goal

The GUI should become a CAD conversion workspace where the user, the agent,
and the engine share one visible state. The agent should not be a chatbot
bolted onto the viewer, and it should not mutate CAD state secretly. It should
produce structured operation proposals that the Qt app can preview, validate,
queue, approve, execute, and audit.

The core loop is:

```text
user intent -> context bundle -> agent proposal -> action queue
    -> user/policy approval -> Session mutation -> report refresh
```

`csgify::Session` remains the source of truth. The Qt app orchestrates the
workflow. The agent proposes; the session mutates.

## Product Shape

Build an optional Qt application beside the existing CLI:

```text
csgify      CLI/headless workflow
csgify-gui  Qt human-agent workspace
```

The GUI should use Qt Widgets with an embedded OCCT viewer first. QtQuick can
come later if needed, but the first application needs dockable panels, trees,
forms, logs, and a native OpenGL/OCCT viewport more than it needs animated UI.

Suggested layout:

```text
+---------------------------------------------------------------+
| Toolbar: Analyze | Propose | Apply | Emit | Export STEP       |
+----------------------+----------------------------+-----------+
| Assembly Tree        | 3D OCCT Viewer             | Inspector |
| - status badges      | - selection                | - target  |
| - names/hierarchy    | - overlays                 | - metrics |
| - removed/synthetic  | - before/after preview     | - actions |
+----------------------+----------------------------+-----------+
| Agent Console + Action Queue + Transcript                    |
+---------------------------------------------------------------+
```

The user should be able to work without typing for common tasks, but should
also have a natural-language console for intent and refinement.

## Agent Console

The console is not only a chat transcript. It has three parts:

1. Prompt box: user writes intent.
2. Structured action queue: agent output becomes proposed operations.
3. Transcript: explanations, user refinements, command results.

Example prompt:

```text
Make this model emit-ready. Ask before removing anything above 0.5% volume.
```

Example action queue:

```text
[Approve] auto_fit_splines(doc:1, max_deviation=1e-6)
          resolves: unsupported_spline_face
          confidence: high

[Review ] apply_decomposition_candidate(doc:1/solid:16, candidate:2)
          resolves: nonconvex_solid_requires_decomposition
          confidence: high

[Review ] remove_solid(doc:1/solid:42)
          reason: tiny hardware
          volume_fraction: 0.00003
```

The user can reference GUI context naturally:

```text
Fix the selected solid.
Remove tiny bolts, but leave anything named shield or detector.
Why can't this emit?
Apply all high-confidence actions.
Show me what changed since the last export.
```

The console should also expose quick commands:

```text
[Explain selected] [Make emit-ready] [Propose removals]
[Auto-fit splines] [Decompose selected] [Check overlaps]
[Emit MCNP] [Export STEP]
```

## Context Bundle

The agent should receive a curated context bundle every turn. Do not dump the
whole model or rely on hidden viewer state. The Qt app should explicitly build
the context from session/report/view state.

Minimum context:

```text
Document:
  source path
  units
  total solids
  emit-ready summary

Current UI:
  selected shape id
  selected face/solid diagnostics
  active mode
  pending queue item, if any

Report:
  blocking diagnostics
  overlaps
  spline faces
  non-convex solids
  decision requests
  candidate proposals

History:
  decision log summary
  operations already applied
  pending action queue

Policy:
  max volume delta
  max spline deviation
  allow removals
  allow overlaps
  auto-apply threshold
```

Example machine context:

```json
{
  "selected": "doc:1/solid:16",
  "task": "make selected solid emit-ready",
  "blockers": [
    {
      "code": "nonconvex_solid_requires_decomposition",
      "target": "doc:1/solid:16"
    }
  ],
  "available_tools": [
    "analyze",
    "propose_decomposition",
    "apply_decomposition_candidate",
    "emit",
    "export_step"
  ],
  "policy": {
    "allow_overlaps": false,
    "allow_removal_without_approval": false,
    "max_spline_deviation": 1e-6
  }
}
```

## Agent Output

The agent should return structured proposals, not just prose.

```json
{
  "message": "solid:16 needs decomposition. candidate:2 is high confidence.",
  "actions": [
    {
      "op": "apply_decomposition_candidate",
      "target": "doc:1/solid:16",
      "params": {
        "candidate": "candidate:2"
      },
      "confidence": "high",
      "reason": "nonconvex_solid_requires_decomposition",
      "requires_user_approval": true
    }
  ]
}
```

The GUI renders this into the action queue and shows evidence:

- target highlighted in the viewer
- diagnostics resolved
- candidate preview or overlay
- volume delta
- overlap result
- confidence and reason codes
- exact recipe/decision-log operation

## Modes

Support three agent modes.

### Explain Mode

No mutations and no queued operations. The agent only explains:

- why a target is not emit-ready
- what diagnostics mean
- what operation classes may resolve blockers
- what risks remain

### Propose Mode

The agent creates queue items. Nothing mutates until the user approves or the
active policy auto-approves the item.

### Auto Mode

The agent may apply operations that match policy:

- high-confidence spline fits
- safe decomposition
- removals under explicit volume/name thresholds
- no overlap override unless explicitly allowed

Auto mode still routes through the same queue and session machinery. It just
allows some queue items to pass approval automatically.

## Boundaries

Hard rule:

```text
Agent proposes.
Qt validates.
User or policy approves.
Session mutates.
Report verifies.
Decision log records.
```

Avoid:

```text
Agent secretly mutates CAD state.
Viewer owns geometry state.
GUI has a parallel model of decisions.
Agent controls mouse/keyboard instead of session tools.
```

The Qt app may run the agent in a worker thread or a separate process, but all
mutations must go through the same session operations used by CLI, Lua, replay,
and C API.

## Suggested Components

```text
Qt GUI
  MainWindow
  OcctViewerWidget
  AssemblyTreeModel
  DiagnosticsPanel
  AgentConsole
  ActionQueuePanel
  OperationInspector

Agent Layer
  AgentController
  ContextBuilder
  PolicyBuilder
  OperationParser
  ToolDispatcher

Engine Layer
  csgify::Session
  analyze / propose / apply / emit / export
  decision log / recipe
```

## First Milestone

Build the GUI without an LLM first:

1. Qt main window with embedded OCCT viewer.
2. Load STEP into `csgify::Session`.
3. Run analyze and populate assembly tree / diagnostics.
4. Select shape in tree or viewer.
5. Show target diagnostics and candidate actions.
6. Apply one approved action through the session.
7. Refresh report and viewer.
8. Export MCNP and simplified STEP.

Then add the agent as a producer of queue items. This keeps the workflow
testable before introducing language-model variability.

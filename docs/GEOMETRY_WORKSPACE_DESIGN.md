# Geometry workspace redesign

The workspace supports an engineer inspecting imported CAD and comparing
certifiable geometry changes. Its primary visual is the model, with selection
and evidence kept adjacent. This design extends the existing Qt Widgets
decision window and Session workflow.

```text
Model filename                            Revision / geometry status
Assembly                 Geometry                         Inspector
Search names / IDs       Fit · Isolate · Context · All     Selection / next step
Hierarchy / status       Before · Preview · Verified       Alternatives | Evidence
                         OCCT viewport                    Proposal actions
                         Geometry legend                  Certified resolution
                         Navigation hints                 Close / export
History and job progress (expand on demand)
```

Use a cool technical palette: canvas `#edf2f5`, panel `#ffffff`, text
`#203442`, secondary text `#526775`, selection wash `#d8f3f7`, action
`#126b80`. Keep native geometry preview colors and cyan selection intact.
Use Noto Sans when installed, with system sans-serif fallback, a readable
body size, medium-weight section labels, and tabular alignment for metrics.
Spacing follows a small 4/8/12/16 scale; outlines indicate panel boundaries
and controls rather than decorating every metric.

The assembly is searchable without changing geometry visibility or selection.
Viewport controls belong above the viewport; the legend and navigation hints
belong below it. Alternatives and evidence share inspector space through tabs,
with evidence available even when no proposal exists. The inspector scrolls
when window height is limited. History starts collapsed and can be opened from
the workspace header. Active jobs still open the existing progress dock.

Inspection shows the selected shape and next action. Candidate generation
enters the existing busy state, then exposes alternatives and guarded apply.
Preview remains non-mutating; verified result retains its existing meaning.
Warnings, cancellation, partial results, and emission failures retain the
existing controller and worker behavior. Header counts describe decomposition
status rather than claiming readiness for export; the engine safety report
remains the authority for emission.

This is an engineering workspace, so the visual emphasis is the geometry and
its comparison modes. Avoid introductory marketing content and decorative
cards. Standard Qt layouts keep keyboard focus, resizing, and dense tables
usable. Retain object names used by the real-widget interaction regressions.

## Implemented and verified, 2026-09-28

Implemented in `src/viewer_qt.cpp`. The normal OCCT renderer remains embedded
in the central panel. Search preserves the selected target; a regression
checks unmatched queries, status matching, and restoration after clearing the
query. Inspection evidence now retains its selected-shape recommendation
instead of being overwritten by generic guidance. Completed preview jobs open
the evidence tab, including failed previews.

The current preset builds successfully. The focused Qt decision-widget,
CAD-job-controller, and project-workspace qualification tests pass (3/3).
The real-widget capture pass inspected both inspection and candidate review at
1500×900 and 1100×720, including a long imported model label. Capture artifacts
are in `/tmp/csgify-workspace-redesign/`; temporary artifacts may be cleaned by
the operating system. These captures omit native OCCT rendering, so they
validate widget layout rather than OpenGL appearance.

Reproduce the layout captures using the existing interaction regression:

```sh
QT_QPA_PLATFORM=offscreen \
  CSGIFY_WIDGET_CAPTURE_DIR=/tmp/csgify-workspace-redesign \
  ./build/csgify_qt_decision_widget tests/fixtures/l_shape.step
```

**Codebase review and OCCT adoption assessment — 10 September 2026**

The largest opportunities are in geometry correctness, execution policy, and reuse. csgify already uses OCCT 8.0.0, XDE import, assembly definitions, overlap prescreening, local partition certificates, and a substantial session/provenance layer. Changing the version number alone will not address the most consequential weaknesses below.

This review covers the current working tree, including its extensive uncommitted and untracked implementation. It concentrates on import, recognition, decomposition, overlap certification, approximation, session mutation, STEP export, persistence, build configuration, and viewer integration. It is not an exhaustive audit of every line or a security audit. Findings marked “reproduced” have direct execution evidence; other findings identify source-level defects, risks, or proposed experiments. No application implementation was changed.

**Release baseline**

OCCT 8.0.0 is released. OCCT 8.0.1 was announced on 30 July 2026, building on the 8.0.0p1 API/ABI baseline. The maintenance fixes include periodic B-spline Boolean hangs, healing crashes, validation of faces with many holes, and STEP writer robustness. Those areas map directly to this application's workloads. Qualify 8.0.1 now with a clean rebuild and representative fixtures. Upstream performance examples are not measurements of csgify. [Official 8.0.1 announcement](https://github.com/Open-Cascade-SAS/OCCT/discussions/1415).

The transition from original 8.0.0 to 8.0.0p1 included ABI changes. Treat it as a rebuild boundary, including anything that embeds OCCT types. The local installed version header identifies 8.0.0; its version macro alone does not establish the exact source revision or patch provenance. [Official hot-patch compatibility announcement](https://github.com/Open-Cascade-SAS/OCCT/discussions/1301).

8.1 remains future development. The live milestone page inspected during this review lists 15 January 2027, but a milestone date is a planning target. The maintainer explicitly warns that BRepGraph interfaces will change in 8.1/development builds. Maintain an experimental compatibility build; do not make delivery depend on an anticipated API. [Milestones](https://github.com/Open-Cascade-SAS/OCCT/milestones), [BRepGraph design and compatibility note](https://github.com/Open-Cascade-SAS/OCCT/discussions/1291).

**1. High priority: sampled spline residuals are presented as exactness and maximum-deviation evidence — reproduced**

Evidence: [spline_fit.cpp](../src/spline_fit.cpp), `sample_face_surface()` and `finalize_fit_deviations()`; [session.cpp](../src/session.cpp), `add_fit_surface_candidate()` near line 656 and `apply_budgeted_surface_replacement_candidate()` near line 2431; [cleanup.cpp](../src/cleanup.cpp), `replace_face_with_fit()` near line 684.

The fitter evaluates a fixed 13 × 13 UV grid, skips evaluation exceptions, fits its candidates, and reports the largest residual among those same samples. The proposal assigns `exact = relative_max_deviation <= 1e-10`. Application checks the reported sample maximum against the requested deviation budget. Neither the sampling nor this threshold proves an upper bound over the face.

A standalone probe compiled the current `src/spline_fit.cpp` against installed OCCT. It constructed a degree-one B-spline surface with U knots at 0, .04, .045, .05, 1. Its height is zero except for a narrow triangular spike reaching one unit at U=.045. The regular sampling grid misses the spike entirely:

```text
plane_available=1 sampled_max=0 relative_max=0 true_peak_z=1
```

This reproduces incorrect fit evidence, rather than an end-to-end invalid MCNP export. Downstream volume checks are useful but do not establish a maximum surface-distance bound: a sufficiently narrow feature can have small volume and large displacement.

Introduce separate evidence for sampled residual, bounded deviation, and recognized analytic identity. Remove the exactness claim based solely on sampled residual. Sample knot spans, trim boundaries, seams, and adaptive interior points; treat failed or nonfinite evaluations as incomplete evidence. For a certified upper bound, use a conservative control-net/subdivision argument where applicable or another independently validated bound. Merely increasing the regular grid does not fix the guarantee.

OCCT opportunity: use `GeomGridEval_Surface` to make denser, span-aware sampling affordable. Its batch surface and derivative evaluations are available locally and documented publicly. It is an accelerator, not a deviation certificate. [Grid evaluator API](https://occt3d.com/dev/doc/refman/html/class_geom_grid_eval___surface.html).

**2. High priority: shared-shape snapshots and concurrent Booleans lack an enforced non-destructive policy — source-confirmed risk**

Evidence: [session_impl.hpp](../src/session_impl.hpp), shared-shape revision snapshots near line 48; [analyze.cpp](../src/analyze.cpp), `intersection_volume()` near line 963 and overlap workers near line 1173; [decompose.cpp](../src/decompose.cpp), `run_splitter()` near line 152.

The session explicitly copies OCCT shape handles for rollback. Multiple overlap jobs may operate on the same body's underlying topology simultaneously. No `SetNonDestructive` calls were found in application sources. The inspected OCCT `BRepAlgoAPI_BuilderAlgo` constructor initializes non-destructive mode to false; the option is passed to the intersection machinery.

Consequently, read-only analysis and proposal generation do not enforce the immutability their surrounding design assumes. When OCCT updates input tolerances or representations, shared historical state may change; simultaneous pairs sharing a body can also contend on the same data. This is a concrete contract mismatch, not a reproduced race in this review.

Default-construct operations, set arguments/tools and non-destructive mode before execution, and centralize ownership rules. Audit healing, unification, meshing, and any other mutators separately. Where preservation cannot be established, copy the affected geometry explicitly. Validate with before/after topology, tolerances, representation checks, rollback tests, and shared-body concurrency stress. Non-destructive Boolean mode is necessary here; it is not a blanket guarantee that every OCCT API becomes thread-safe.

**3. High priority for performance: many Boolean operations execute twice — confirmed in application and OCCT source**

Examples: [analyze.cpp](../src/analyze.cpp) near line 967, [local_overlap.cpp](../src/local_overlap.cpp) near line 168, [local_partition.cpp](../src/local_partition.cpp) near line 141, [approximation.cpp](../src/approximation.cpp) near line 81, and [coalesce.cpp](../src/coalesce.cpp) near line 143.

The common pattern is `BRepAlgoAPI_Common(a,b); common.Build();`, with equivalent Cut and Fuse sequences. OCCT's two-shape constructors already call `Build()`. The subsequent call clears the previous run and builds again. Coalescing additionally sets fuzzy tolerance only after the first execution.

Use one explicit construction/configuration/execution path. This also allows exception handling, cancellation, history policy, diagnostics, and metrics to cover the actual operation. In `local_partition.cpp`, the two counted Boolean calls can currently cause four executions; constructor exceptions also occur outside the helper's catch boundary. This weakens the meaning of both budgets and metrics.

Do not promise a twofold whole-application speedup. The removed work is real, but input mutation, caching, and non-Boolean work affect total time. Instrument actual executions and compare results and wall time on the existing benchmark corpus.

**4. High priority for long-lived sessions: temporary XCAF documents remain registered — reproduced API behavior**

Evidence: [import.cpp](../src/import.cpp) near line 408 and [step_export.cpp](../src/step_export.cpp) near line 332. Both create documents through the singleton XCAF application; neither closes them on completion or early return.

The OCCT implementation of `NewDocument()` adds the document to the application session. A local handle going out of scope does not unregister it. The standalone probe produced:

```text
documents_before=0 after_handle_scope=1
```

Repeated imports and exports therefore accumulate registered documents and their retained data. This matters particularly for the Qt workspace and repeated library calls, even when short CLI processes hide the effect.

Use a scope-bound document owner that calls the application `Close()` on success, failure, cancellation, and exception paths. Keep any needed shapes/metadata through explicit ownership. Test document count and memory across repeated import/export cycles. STEP export also changes global auto-naming and redirects `std::cout`; review those process-global effects before allowing concurrent in-process exports.

**5. Medium priority: cutter deduplication can identify different planes as equal — source-level counterexample**

Evidence: [decompose.cpp](../src/decompose.cpp), `surface_signature()` near line 203 and `split_solid_by_all_surfaces()` near line 367.

The signature canonicalizes a plane's normal but computes its distance using the original normal. Consider planes x+y+z=3 and x+y+z=-3, located respectively at (1,1,1) and (-1,-1,-1), with opposite normals. Both produce the same canonical positive normal and positive distance. The second cutter is discarded. Conversely, reversing a representation of the same offset plane can prevent deduplication.

The fixed six-decimal string precision can also collapse geometrically distinct supports independently of `linear_tolerance`. The comment that correctness cannot depend on this deduplication is too strong: discarded distinct tools can reduce decomposition completeness, even if subsequent certificates prevent acceptance of a bad partition.

Canonicalize normal and signed distance together. Use a tolerance-aware candidate key followed by a geometric equivalence check before discarding a cutter. Locally available `GeomHash_SurfaceHasher` is worth evaluating as infrastructure, but geometry hashing does not automatically capture the application's equivalence of unbounded supports, trims, or occurrence transforms. Keep application equality explicit.

**6. Medium priority: the supported build contract is inconsistent — reproduced**

Evidence: [CMakeLists.txt](../CMakeLists.txt) lines 1, 12, and 167; [CMakePresets.json](../CMakePresets.json).

The project declares CMake 3.20 but links `SQLite3::SQLite3`, a target introduced by CMake 4.3. Running the existing build with available CMake 3.31.6 failed during generation because that target does not exist. The older module supplies `SQLite::SQLite3`. Provide a compatibility alias or deliberately raise the minimum. [CMake's documented compatibility recipe](https://cmake.org/cmake/help/v4.3/module/FindSQLite3.html).

The OCCT `8.0.0 EXACT` requirement rejects maintenance version 8.0.1. Replace it with an explicit tested-version policy and an experimental opt-in for future versions. Do not simply accept arbitrary newer kernels. Keep independent build directories and record the selected kernel build identity.

The current generated engine flags contain only `-std=c++17`; `CMAKE_BUILD_TYPE` is empty and the preset supplies no build type. Establish a Release or RelWithDebInfo benchmark build before interpreting performance. The preset also embeds a personal install path. Provide a portable configuration mechanism for OCCT prefixes.

**7. Medium priority: cancellation stops between operations rather than inside costly OCCT work**

Evidence: [session.cpp](../src/session.cpp), solid-boundary cancellation near line 818 and fixed-point coordinator; [decompose.cpp](../src/decompose.cpp), bare `splitter.Perform()`; Boolean sites above. No OCCT `Message_Progress` integration was found in application sources.

The existing progress and cancellation architecture is valuable, but a single expensive Boolean can exceed an elapsed-time budget or make cancellation appear unresponsive. Bridge the application's cancellation token to a `Message_ProgressIndicator` and pass ranges through supported Build, Perform, and Transfer APIs. Throttle progress publication and marshal GUI updates to their proper thread.

Use process isolation for hard deadlines or non-cooperative kernel failures. Preserve the current commit/rollback boundary: interrupted geometry must not become a successful result. These progress APIs predate OCCT 8; this is an existing capability that the new integration should exploit.

**8. Medium priority: healing changes geometry after metadata and definition identities are collected**

Evidence: [import.cpp](../src/import.cpp), metadata/assembly extraction in `read_step_shape()`, followed by `ShapeFix_Shape::Perform()` in `load_and_prepare_shape()` near line 487.

When healing changes topology or tolerances, the returned shape and previously collected traversal IDs, fingerprints, or definition geometry digests can describe different states. This is conditional on a material healing change; it was not reproduced on a fixture during this review.

Separate source identity from prepared-geometry identity. Carry healing history/replacement mappings into metadata reconciliation, recompute affected geometry digests, and record before/after validity and tolerance changes. Add a fixture whose repair alters topology; verify that names, instances, materials, and cached evidence still refer to the intended shapes.

**Where OCCT creates additional opportunities**

| Opportunity | Existing implementation and proposed use | Availability and adoption criterion |
|---|---|---|
| Batch geometry evaluation | Replace scalar UV loops in spline fitting with span-aware `GeomGridEval_Surface`; share samples across candidate fits and validation passes. | Confirmed in local OCCT 8.0.0. Compare points/derivatives on located, trimmed, periodic, rational, and singular surfaces. Preserve the adaptor's transformation and lifetime. |
| Better exact recognition before fitting | Test spline planarity through `GeomLib_IsPlanarSurface` and conservative control-net checks; investigate supported revolution normalization before approximate fitting. | Existing OCCT capabilities, not new in 8. Enable only recognizers whose geometry and tolerance contracts are verified. A new evaluator for a quadric does not mean imported splines are automatically recognized as that quadric. |
| Reuse Boolean intersections | Related operations on identical immutable argument sets can share a `BOPAlgo_PaveFiller`; local region selection may benefit from `BOPAlgo_CellsBuilder`. Medium construction currently chains many fuses/cuts. | APIs available locally. Pilot bounded local workloads; measure peak memory and fragmentation. A global General Fuse over a whole assembly could be substantially worse. Keep independent certification where reuse would otherwise make validation circular. |
| Controlled parallelism | Keep existing parallel overlap pairs; consider kernel parallelism for a single large splitter/Boolean. | Application pair-level threading already exists. Choose a shared thread budget; avoid multiplying outer workers by inner kernel threads. Resolve ownership issue 2 first. |
| Tighter broad-phase bounds | Compare cached `BRepBndLib::AddOptimal` boxes and selective OBB rejection with existing AABB sweep. | Available locally. Existing sweep and pair caches should be retained unless measurements justify replacing them. OBBs help rotated slender bodies; a point KD-tree is not a drop-in index for extended solids. |
| Feature suppression | Add `BRepAlgoAPI_Defeaturing` as a candidate producer for selected fillet/chamfer/hole faces, complementing custom hole filling. | Existing API, not new in 8. Apply volume, deviation, topology, and overlap policy afterwards. Require recognizable failure evidence and preserve materials/provenance. |
| Curved-face topology | Extend the planar-only edge-concavity path in `topology_analysis.cpp` using oriented normals and differential properties on curved faces. | OCCT 8 release notes describe GeomProp/BRepProp. Those package headers were not found in the local installed/source trees inspected. Feature-probe the chosen distribution rather than assuming a release summary matches this build. Local normal samples remain heuristics, especially at seams and singularities. |
| Persistent topology/occurrence representation | Prototype BRepGraph as an analysis sidecar for one shared definition, parent/child traversal, and cache invalidation. | BRepGraph headers exist locally; its public interface is still evolving. Require semantic roundtrip tests before storing authoritative state in it. |
| Incremental display | `viewer_qt.cpp::rebuildScene()` removes every presentation and recreates all `AIS_Shape` objects. Retain unchanged presentations and update only changed geometry/status. | Application architecture improvement available today. Measure review latency and tessellation memory on repeated assemblies before selecting more elaborate instancing. |

The OCCT 8 release also brings lower-level evaluation, bounding, and Boolean improvements that existing wrappers can already receive. Replacing every `Value()` call with a differently named API is not automatically a speedup. The meaningful changes are batching, avoiding repeated work, better ownership, and using supported specialized operations. [Official OCCT 8 release](https://github.com/Open-Cascade-SAS/OCCT/releases/tag/V8_0_0).

**BRepGraph and 8.1: a bounded architectural investment**

There is a strong conceptual match between BRepGraph and this project's needs: shared definitions, concrete occurrences, topology relations, explicit mutation, and revision-aware caches. Start with an adapter around one definition and its occurrences. Compare graph construction cost plus repeated analysis against the current `TopExp` maps and definition reuse.

Retain application IDs at the API boundary. Internal node identifiers and graph history do not establish durable identity across unrelated STEP imports, arbitrary Boolean topology changes, deduplication, or serialization policies. Require explicit bindings and history handling. Keep TopoDS at the modeling-algorithm boundary while the prototype matures. These are review recommendations, not promises of automatic behavior from BRepGraph. [Maintainer's architectural explanation](https://github.com/Open-Cascade-SAS/OCCT/discussions/1291).

Avoid rebuilding the SQLite job queue, approximation ledger, or review authority system inside an OCCT document/graph. They represent application policy rather than geometric topology. The application already has useful separation in the Alea bridge, local certificates, typed reports, and session API; preserve it.

**Cache and benchmark policy before upgrading**

The in-memory recognition cache in `analyze.cpp` already reuses results for repeated definitions, and overlap checking already has sweep prescreening and pair reuse. Extend these with geometry revision, preparation policy, tolerance policy, recognizer version, and kernel build identity. `project_store.cpp::sync_definition_recognition_cache()` currently synchronizes certificates from a completed report; its existence does not imply that cold analysis loads and trusts persisted recognition results before computation.

The persistent recognition key near line 1176 includes geometry and policy digests but no explicit OCCT build identity. The current path compares freshly produced certificates, so this is not proof of stale cross-version reuse today. It is a requirement before turning that store into a computational cache. Preserve old artifacts for provenance while invalidating their authority under changed geometry or kernel policy.

The existing benchmark harness records input digests, outputs, timings, and performance counters. Add source-tree identity (including dirty state), compiler and optimization flags, OCCT source/patch/build identity, loaded library paths, machine/thread configuration, units, tolerance settings, and peak memory. Compare baseline, Boolean cleanup, evaluator changes, and kernel upgrade separately so attribution is possible.

Use the existing Triangle, FWTBM1, toy-car, ITER shielding, and L-shape cases, with smaller shareable regressions for each failure mode. Acceptance must compare validity, represented geometry, exact/residual volume, approximation budgets, overlaps, occurrence/material mapping, and output coverage. Timing alone is insufficient; geometrically equivalent outputs need not have identical traversal order or tessellation.

**Recommended implementation order**

1. Restore a reproducible build; add the spline counterexample; correct fit evidence, document lifetime, and cutter equality. Establish immutable Boolean inputs and one execution per operation.
2. Consolidate an internal OCCT operation service carrying tolerance, history, diagnostics, cancellation, thread policy, and measured execution counts. Preserve all existing commit and certification gates.
3. Qualify 8.0.1 in an independent optimized build. Compare geometry and performance against a recorded 8.0.0 baseline. Maintain the experimental next-version build separately.
4. Implement batch/span-aware evaluation, exact planarity recognition, revision-aware analysis reuse, and incremental display. Measure each on representative models.
5. Prototype local CellsBuilder/intersection reuse, kernel defeaturing, curved topology classification, and BRepGraph independently. Promote only the experiments that improve coverage or measured cost without weakening evidence.

Keep the first changes small and reviewable. `session.cpp`, `main.cpp`, and `viewer_qt.cpp` are large coordination units, but splitting them solely by line count is less valuable than extracting the kernel execution and evidence contracts that currently vary across files.

**Validation and limitations**

- Initial CTest enumeration found 70 tests. CMake regeneration then failed on the SQLite target under CMake 3.31.6; Qt development packages were also not found.
- A run of available tests after that attempt selected 67 tests: 31 passed and 36 failed in about three seconds. The existing CLI requires unavailable `Qt_6.10`; numerous smoke failures are consequently runtime-environment failures. These results are not a current-source regression verdict. Qt/workspace tests were excluded, and no fresh full build or OCCT-version comparison succeeded.
- A separately compiled probe against the current fitter and installed OCCT reproduced the missed spline spike and XCAF document registration behavior. Probe source: `/tmp/cad2csg-review-probe.cpp`; executable: `/tmp/cad2csg-review-probe`. Existing-binary CTest log: `/tmp/cad2csg-review-ctest.log`.
- Boolean constructor/default behavior and document ownership were checked against the local OCCT source, rather than inferred from API names. The plane-sign issue is a source-level mathematical counterexample. No race, healed-metadata mismatch, large-model speedup, or BRepGraph migration was claimed as dynamically demonstrated.
- The review adds this document only to application sources. Build checks regenerated some build-directory metadata and test artifacts. No application fixes, dependency upgrade, commit, or benchmark claims are included.

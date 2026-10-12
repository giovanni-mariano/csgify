**csgify: code, product, and LLM workflow review — 2026-09-24**

The intended product is a workspace for turning engineering CAD into useful CSG, with an LLM helping interpret intent and choose operations, and with optional human review. My assessment is that the architecture is a good foundation for that product. The strongest existing design choice is the separation between proposals, geometry evidence, policy, and transactional mutation. The largest gaps are in the strength of some evidence, durable workflow identity, and the information and decisions presented to the planner and reviewer.

This is an implementation review and product assessment, not a qualification for production simulation. I inspected current source, built all configured targets, ran the existing tests, and made small independent diagnostic probes. I did not run an actual language model, benchmark an industrial assembly, or validate an exported model in a transport solver. No application code was changed for this review.

**What already works in the design**

- OCCT computes geometry; the LLM requests named operations. The autonomy evaluator regenerates candidates rather than trusting model-supplied measurements.
- The session supplies a shared mutation boundary for CLI, GUI, and scripting. Revisions, rollback, decision logs, and replay are important foundations for both automation and review.
- Exact decomposition, residual handling, budgeted approximation, material/assembly metadata, overlap checks, Alea handoff, and domain coverage are already substantial capabilities.
- The UI distinguishes agent explanations from engine evidence and policy results. It disables review on stale revisions and supports before/preview/applied inspection.
- The approximation ledger measures added and removed geometry and cumulative changed volume. This is substantially stronger than checking only net volume differences.
- The coverage documentation explicitly distinguishes structural void construction from sampled ownership diagnostics. Preserve this distinction throughout the product.

These capabilities establish an engine and review system that an LLM can use. The inspected application does not yet contain a complete inference-provider connection and autonomous planning loop. The autonomy tests supply hand-written action JSON; they are not evidence of LLM decision quality.

**Priority findings in the current implementation**

1. **High: sampled fit accuracy is promoted to exactness, and exactness authorizes automatic replacement.**

   [spline_fit.cpp](../src/spline_fit.cpp), starting near line 20, samples a fixed 13-by-13 parameter grid and skips failed evaluations. [session.cpp](../src/session.cpp), near line 656, sets `candidate.exact` when the relative sampled maximum is at most `1e-10`. [autonomy.cpp](../src/autonomy.cpp), near line 345, uses that flag in the automatic surface-replacement policy.

   I constructed a degree-one B-spline surface with a narrow triangular ridge between U=0.04 and U=0.06, peaking at Z=1 at U=0.05. The sampling grid misses the ridge. Running the current fitter produced:

   ```text
   plane_available=1 sampled_max_deviation=0 relative_max_deviation=0 actual_spike_z=1
   ```

   This reproduces incorrect fit evidence, not a complete unsafe export. Subsequent geometry checks can still reject a mutation. They do not make the original exactness claim valid. The budgeted replacement path also assumes an exact partial support substitution leaves the owner geometry unchanged, near session.cpp:2491.

   Separate recognized analytic equivalence, sampled residual, and certified deviation bounds. Only evidence with the required guarantee should authorize the corresponding automatic operation. Adaptive/knot-aware samples improve detection but do not alone prove a global bound. Treat incomplete evaluation as incomplete evidence.

2. **High: saved approval is not bound to the source CAD contents.**

   [main.cpp](../src/main.cpp), near lines 1335–1426, compares the persisted source path with the invocation path, reloads that file, and reconstructs executed actions. The queue does not bind those actions to the original source digest or candidate geometry.

   Reproduction: copy `l_shape.step` to a temporary `source.step`, run automatic decomposition and persist the queue, replace that temporary file with `l_with_inner_box.step`, then resume the queue against the same filename. Resume returns success, reports one applied action, and requests no review. Both inputs were repository fixtures; no repository CAD file was modified.

   A local revision number and a traversal ID such as `solid:1` do not establish identity across reloads. Persist and verify source content identity, preparation/units policy, engine/kernel identity, policy identity, and the approved operation/candidate identity. A changed source should require an explicit rebase with fresh review where necessary. The import and artifact layers already have digest infrastructure that can be reused.

3. **High for a resumable product: execution and reconstruction implement different operations.**

   [autonomy.cpp](../src/autonomy.cpp), near line 962, executes `resolve_overlap_with_approximation`. The reconstruction dispatch in [main.cpp](../src/main.cpp), near lines 1383–1421, has no corresponding branch. A successful overlap-ownership queue on `overlapping_boxes.step`, using the existing 20% budget test scenario, fails immediately on resume with `autonomy_resume_reconstruction_failed`.

   Other paths also diverge: fixed-point reconstruction uses default policy instead of the explicitly configured original fixed-point policy; surface reconstruction does not select the original budgeted execution path. The overlap failure is dynamically reproduced; the other differences are source-confirmed and were not separately reproduced end to end.

   Use one typed execution/reconstruction contract with recorded effective parameters. Replaying a successful action should reproduce its approved result and ledger effects, with an explicit incompatibility result when it cannot. Test persist/restart for every operation and for partial progress, not only exact decomposition.

4. **Medium: the current context bundle is too weak for the proposed small-model workflow.**

   [autonomy.cpp](../src/autonomy.cpp), near lines 1144–1384, emits useful summary metrics and candidates, but the selected-solid filter does not resolve a selected face to its owner. A probe with `--target face:1.1` returned zero solids and no `faces` object. The mutation vocabulary contained 33 operations without per-operation argument schemas or current execution/review support.

   The bundle does not convey the full assembly/material/role/neighbour context already present elsewhere in the engine. History omits rejection rationale and target/candidate details. Diagnostics, history, and candidates have count limits, but the all-solids path has no solid-count or total context-size bound.

   Supply focused tasks: selected feature and owner, local neighbours/interfaces, material and protected status, relevant prior decisions, remaining budgets, and a small list of currently valid actions. Include required arguments, units, predicted benefit, evidence limits, and explicit `ask`/`stop` outcomes. Resolve owner context for faces and residual pieces. Add pagination and truncation metadata rather than silently discarding relevant information.

5. **Medium: escalation to a human is not yet a complete recovery workflow.**

   [viewer_qt.cpp](../src/viewer_qt.cpp), near lines 3884–3919, adapts decomposition, surface replacement, removal, and contained-medium operations for geometry review. Other operations can reach a human-review queue but throw `this operation has no Qt geometry-review adapter yet` when reviewed. This includes relevant fixed-point or approximation recovery scenarios.

   Each escalation needs a concrete resolution path: change an allowed parameter, choose ownership, select an alternative, edit the source, retain an unresolved residual, or suspend the task. Distinguish a discretionary tradeoff from missing geometric evidence and from an unsupported action. A human acknowledgment should not turn an unknown geometry result into a proven one.

6. **High if connected to an untrusted planner: the proposed execution boundary conflicts with the older scripting workflow.**

   [AGENT_WORKFLOW.md](AGENT_WORKFLOW.md) recommends having agents generate Lua scripts. [lua_runner.cpp](../src/lua_runner.cpp), near line 2746, calls `luaL_openlibs`, providing general scripting facilities including file/process access. That interface is useful for trusted automation, but does not provide the constrained authority described in [AUTONOMY.md](AUTONOMY.md).

   Give the product's planner only bounded read tools and typed proposal submission. Keep trusted scripting as a separately documented capability. Do not give the planner write access to its approval queue or immutable policy store; queue files currently carry authority on resume. CAD names, annotations, imported text, and model rationales are data, not instructions that can expand this authority. These are integration requirements; no deployed planner or exploit was demonstrated.

**Additional engineering risks that remain relevant**

The previous [OCCT review](OCCT_REVIEW_2026-09-10.md) is still relevant. I rechecked current source for these items:

- Session snapshots share OCCT shape handles, while Boolean sites do not establish a consistent non-destructive policy. The local OCCT implementation defaults that policy to false. This is a source-confirmed ownership/rollback risk, not a reproduced race or rollback corruption.
- Some two-argument Boolean constructors are followed by another `Build()`, even though the constructor already executes the operation. This wastes work and complicates operation accounting.
- Import/export create XCAF application documents without a matching application `Close()` in those paths. Long-lived GUI or agent sessions need explicit document lifetime management.
- Cancellation happens at operation boundaries; a configured budget is not necessarily a hard deadline inside a costly kernel call. Worker infrastructure exists, but it must actually supervise planner-triggered work when hard limits are promised.
- Action parsing is permissive: schema/version are optional, non-object array elements are skipped, and duplicate proposal IDs are not rejected there. Harden the external protocol before relying on small-model outputs. A grammar helps produce valid syntax; the application still must validate meaning and limits.

The SQLite compatibility issue was fixed earlier in this session. I did not repeat it as an open finding. The development build has no optimization configuration; its test timing should not be used as a product performance benchmark.

**The product should define what a successful simplification means**

There are three distinct claims to keep visible:

| Claim | Required evidence | Product consequence |
|---|---|---|
| A result can be represented as supported CSG | Surface/expression readiness and valid region construction | It can proceed to geometry validation. |
| The result preserves the source within the chosen geometric policy | Equivalence evidence or measured, budgeted deviations with stated limitations | The system can explain exactly what changed. |
| The result is suitable for the intended analysis | User/project semantics and application-specific validation | It can be accepted for that use case. |

An analytically exact CSG sphere replacing a different shape is still an approximation of the input. Likewise, a small changed-volume fraction does not establish that a feature is unimportant. A thin layer, channel, interface, or detector may be small relative to the assembly and still be essential to the user's task. This is why geometric tolerances alone cannot encode user intent.

The existing protected targets/materials and topology/containment settings are a useful starting point. Make these part of a first-class project policy: intended analysis, material assignments and their provenance, protected features/interfaces, units, modeled domain, allowable changes, complexity limits, and authority mode. Ensure every applicable mutation and resume path carries and enforces the same policy. Natural-language policy interpretation should become a visible structured configuration before it authorizes changes.

Keep geometry-only CSG export separate from a fully assembled simulation input. The Alea documentation already places sources, tallies, importances, and final transport-deck policy outside the current geometry handoff. Make that boundary clear in the UI and output manifest.

**Where the LLM should earn its place**

The engine should automatically do deterministic recognition and certified lossless conversion. It already has substantial fixed-point planning machinery. Calling a model to repeatedly select an obvious `best` candidate adds overhead without demonstrated value.

The LLM can be useful for translating user intent into selections and policy proposals, interpreting names and assembly relationships, choosing between permitted tradeoffs, explaining a blocker, and formulating a precise question when engineering intent is missing. It can suggest a material/role mapping, but that mapping needs provenance and the authority required by project policy.

A useful loop is:

```text
Project intent and immutable policy
    -> deterministic analysis and exact conversion
    -> focused unresolved task with engine-generated candidates
    -> planner choice, clarification request, or explicit stop
    -> deterministic policy and geometry checks
    -> commit or human review
    -> refreshed state, final validation, and provenance package
```

The LLM cannot solve a geometric operation that the engine cannot represent or check. Expand the action vocabulary in response to real benchmark failures. Avoid making raw CAD rewriting or free-form program generation the default route around missing capabilities.

A small local model is a plausible planner for this restricted task, but that remains a hypothesis until measured. Start with structured text and candidate selection; use visual evidence when it answers a specific geometric question. Keep the provider replaceable and record model version, prompt/context, output, and tool results for reproducibility. Pin and test the serving configuration. [llama.cpp supports grammars and conversion of a subset of JSON Schema to constrained output](https://github.com/ggml-org/llama.cpp/blob/master/grammars/README.md); syntax constraints do not prove that the selected action is appropriate.

Choose a model only after defining a held-out decision set and checking available memory/latency on the target computer. Do not equate a larger context window with a well-designed context bundle. No particular model size or hardware fit is established by this review.

**BRepGraph and the OCCT 8.1 direction**

The dedicated [BRepGraph section in the OCCT review](OCCT_REVIEW_2026-09-10.md) is an important part of the product architecture. It deserves explicit connection to the planner/context recommendations above. BRepGraph interfaces are already present in this computer's OCCT 8.0 installation; the [maintainer's design discussion](https://github.com/Open-Cascade-SAS/OCCT/discussions/1291) describes intended API adjustments for 8.1/development builds. Treat that interface as a versioned dependency behind an adapter.

Local headers confirm occurrence-aware parent traversal, separate entity/reference identities, history infrastructure, and version-stamped caches. My proposed product uses of those facilities are:

| Product need | Proposed use of BRepGraph |
|---|---|
| Give the planner context for a selected face | Resolve its owning solid and occurrence path, then attach csgify material, policy, and candidate evidence. |
| Distinguish one instance from all repeated parts | Map application selections to explicit definition and occurrence identities; make edit scope visible before approval. |
| Refresh evidence after a change | Track graph versions and application dependencies so affected contexts and certificates are invalidated deliberately. |
| Explain and resume edits | Bind application operations to persisted graph identity/history and source content identity, with explicit handling of split, merge, and deletion. |

The benefit for a small LLM would come from the focused queries built on this representation. For example, the planner could request the selected feature's owner, adjacent topology, repeated occurrences, and applicable constraints. Sending an entire graph to the model would still produce excessive context and leave authority undefined.

BRepGraph topology does not by itself establish geometric contact between separate bodies, physical containment, material significance, or permission to simplify. Those require csgify's geometry analysis and semantic/project policy. Likewise, graph UIDs do not prove that a replaced STEP file is the same approved input. Source digests and explicit cross-import bindings remain necessary. External TopoDS Boolean results also need reconciled history and invalidation; creating a graph beside existing code does not automatically fix shared-shape mutation or sampled-fit evidence.

I would prototype a read-only adapter for one repeated definition and its occurrences, through the public builder/view APIs. Compare selected-face context, instance selection, graph construction cost, memory, and repeated-query time against the current implementation. Then qualify one mutation and persist/reload path, including split/merge identity and metadata preservation. Initially the session remains authoritative and the graph is derived; any later ownership migration must explicitly designate one authoritative state. Keep application jobs, budgets, approvals, and audit policy in their existing application layer.

This makes BRepGraph a candidate foundation for the product's context and identity layer, alongside the correctness fixes, rather than an isolated performance experiment. It does not require making delivery dependent on an unqualified kernel upgrade.

**Human involvement should be about decisions that matter**

The before/preview/applied viewer and evidence separation are worth retaining. A reviewer should see what changes, why it matters, what is preserved, what is uncertain, the cumulative budget impact, and available alternatives. Display missing evidence separately from a permissible tolerance tradeoff.

For repeated assemblies, let users review a semantic rule or a representative group, with per-instance verification. Hundreds of nearly identical cut approvals would defeat the purpose of automation. Preserve exceptions and rejection reasons so a later model turn does not keep proposing the same unwanted change.

Use the same policy and result states for assisted and unattended operation. Unattended means operating inside previously granted authority. When intent or evidence is missing, it should persist a useful partial result and a machine-readable blocker. It should not need to invent permission to finish. Distinguish conversion completion from completion of the current action queue.

**Measure usefulness, not just successful export**

The benchmark harness and real-model case list are good foundations. Add a fixed evaluation set with curated reference decisions and accepted results. Include repeated parts, thin walls, near contacts, narrow spline features, materials, intentional voids, ambiguous names, and restart after source changes.

Compare the same cases under a deterministic baseline, a small local planner, a stronger planner, and an expert-assisted workflow. Keep the geometry gates and budgets identical. Measure:

- Fraction of models completed with accepted geometry, and explicit unresolved cases.
- Human active time, review count, and repeated/rejected proposals.
- Protected-feature and material/occurrence preservation.
- Added/removed volume and local deviations, with evidence coverage and limitations.
- Final cells/surfaces/expression complexity and downstream geometry-query or transport performance.
- Wall time, peak memory, inference cost, invalid actions, and repeatability after restart.

More decomposition is not automatically a better product result. A huge collection of tiny valid cells can be costly to inspect and use. Make output complexity and downstream performance explicit objectives, subject to preservation constraints.

Add independent downstream geometry validation where supported. As an example of the kind of separate check needed, [OpenMC documents geometry-debug runs and the limitations of plotting for finding overlaps](https://docs.openmc.org/en/stable/usersguide/troubleshoot.html). The current MCNP output needs validation in its actual intended consumer; this reference is not a claim that csgify currently exports OpenMC or that sampled checks prove global correctness.

**Recommended next increment**

First fix the exactness and persistence findings, and establish input ownership for geometry operations. Then complete one end-to-end product slice: import an analytic STEP assembly, run exact conversion, expose a small number of unresolved decisions, accept a human or structured planner response, enforce the immutable policy, validate/export, close, and resume reproducibly.

Give that slice one shared typed operation contract for execution, review support, replay, and schema validation. Add a bounded planner adapter to it. Defer model selection and broader geometry promises until comparisons show where the planner reduces human work or improves accepted completion over deterministic automation.

The value proposition I would test is: “Produce traceable CSG from CAD with less expert intervention, while preserving explicit engineering constraints.” The existing code supports that direction. General unattended simplification should follow demonstrated evidence and workflow coverage.

**Validation record**

- Fresh CMake build in `/tmp/cad2csg-build-check`, GCC 14.2, CMake 3.31.6, Qt 6.8.2, OCCT 8.0.0, existing sibling Alea archives.
- All configured targets built successfully. Existing suite: **70/70 passed**, 55.25 seconds with two test workers and a 120-second per-test timeout. Qt tests include offscreen widget/workspace checks; this is not a manual interactive OpenGL/usability assessment.
- Build log: `/tmp/cad2csg-product-review-build.log`; test log: `/tmp/cad2csg-product-review-ctest.log`.
- Independent probes and their artifacts: `/tmp/cad2csg-product-probes.EroGMc/`. The spline probe links the freshly built fitter object; queue probes use the fresh CLI. Temporary paths may disappear after system cleanup.
- Reproduced separately: missed spline ridge, changed-source resume, failed overlap-repair resume, and missing owner context for a selected face.
- No actual LLM evaluation, industrial performance comparison, or transport-physics validation was performed. Existing tests passing does not cover the independent failures above.

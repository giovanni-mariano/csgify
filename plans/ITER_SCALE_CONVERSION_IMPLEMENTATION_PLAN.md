# ITER-Scale CAD-to-CSG Conversion Implementation Plan

## Status

- **Document status:** proposed implementation plan.
- **Created:** 2026-08-30.
- **Implementation progress (2026-08-30):** the Phase 1 semantic-model
  foundation is implemented. Public interned UTF-8 strings, annotation origin
  and review state, entity metadata, material definitions, component
  definitions, component instances, and affine occurrence transforms are now
  part of the engine/report contract. STEP/XCAF import populates them while
  preserving legacy IDs. Definition/instance IDs include the source-content
  digest; definitions also carry a placement-independent local BREP digest and
  explicitly report whether it was computed. JSON reports expose the complete
  semantic model and reuse counts. Focused metadata/import determinism tests
  and the existing contract/ID tests pass; the full 46-test suite passed before
  the final source-digest tightening, followed by a passing five-test semantic
  and compatibility subset. The next implementation slice is Phase 2's
  project/artifact store and certified definition-level conversion cache.
- **Phase 2/medium foundation progress (2026-08-30):** immutable
  content-addressed artifact publication is implemented with temporary-file
  verification, atomic rename, cross-publication deduplication, validation,
  corruption detection, and path-containment checks. The SQLite project index
  is now implemented at schema version 3 with WAL mode, foreign keys,
  transactional schema migration, source/strings/metadata/annotations/
  definitions/instances/material persistence, atomic snapshot replacement,
  rollback on an invalid reference, artifact indexing, and certified
  geometry+policy conversion-cache entries. `csgify inspect --project DIR`
  persists the imported model and report artifact, and reopen/re-import tests
  pass. Independently, the contained-medium public contract is implemented:
  fill modes, geometric seeds, explicit port dispositions, material/phase
  validation, request error codes, derived-region records, commit-certificate
  fields, and JSON report sections. Geometry extraction/apply remains a later
  Phase 5 operation.
- **Bounded worker/scheduler progress (2026-08-30):** schema v3 adds durable
  conversion jobs and dependency edges. Stable-order claims are serialized by
  SQLite, blocked dependencies cannot run, attempts survive restart, workers
  heartbeat, retryable failures requeue, cancellation prevents result commit,
  and expired workers are recovered or exhausted according to `max_attempts`.
  A successful job can commit only an already indexed, digest-valid artifact
  with a non-empty certificate digest. The POSIX worker runner executes each
  job in its own process group and enforces address-space, CPU, wall-time,
  file-size, and captured-output limits while preserving bounded stdout/stderr
  evidence. Timeout, output-flood, memory-exhaustion, retry, dependency,
  cancellation, abandoned-worker, and reopen tests pass. The complete 51-test
  suite passes.
- **Spatial scaling progress (2026-08-30):** schema v4 adds a persistent SQLite
  R*Tree with transactional namespace replacement, deterministic kind-filtered
  local queries, configurable conservative expansion, bounded candidate-pair
  generation, and owner IDs. `inspect --project` now indexes analyzed instance
  solids and decomposed material pieces automatically. R*Tree results are only
  a prescreen; exact OCCT intersection remains mandatory. The explicit
  in-memory overlap pass now uses deterministic sweep-and-prune rather than an
  all-pairs bbox loop and reports candidate and exact-test counts. A sparse
  51-solid regression produces one candidate/one Boolean test instead of 1,275
  pair checks. Replacement, rollback, tolerance-neighbour, bounded-result,
  report-population, and overlap-safety tests pass. The complete 53-test suite
  passes.
- **Recognition-first progress (2026-08-30):** the topology report now exposes
  ranked recognition candidates, confidence, exactness, evidence reason codes,
  and the selected recognizer. Straight full-annulus pipes are recognized from
  coaxial full cylindrical supports and annular end faces in arbitrary
  orientation. Axis, radii, length, and two outward lumen ports are retained in
  JSON/Lua reports and indexed as project `pipe_port` spatial entities. The
  selected exact operation reconstructs `outer cylinder - inner cylinder` in
  OCCT, certifies the symmetric difference against the source, and preserves
  it as one Boolean material body. Fixed-point and single-pass preprocessing
  choose it before BSP/shatter when decomposition is required; already exact
  analytic pipes remain untouched. Replay and compact Lua recipes retain an
  explicitly applied canonicalization. A
  rotated-axis positive test, solid-cylinder negative test, report/index test,
  and emission-volume test pass; the complete 54-test suite and the rebuilt
  targeted compatibility subset pass. Phase 4 remains open for general 2D
  extrusion recovery, revolutions, elbows/reducers/tees, and definition-level
  recognition caching.
- **Extrusion-recognition progress (2026-08-30):** planar extrusion axes are
  now inferred from CAD support-plane families instead of the longest
  world-aligned bounding-box direction. Constant versus piecewise axial
  topology, end-face count, and cross-section hole count are reported in JSON
  and Lua. The bounded profile-plane-first arrangement works unchanged in any
  orientation, while an exact structural result now short-circuits generic BSP
  candidate generation. Coordinator numerical defaults were aligned with the
  engine's exact decomposition certificate (`1e-6`); explicit tighter caller
  limits remain authoritative. Rotated concave and hollow profile tests,
  curved negative tests, STEP round-trip recognition, coordinator routing, and
  reunited-union volume accounting are covered. On the real FWTBM1 STEP, the
  default fixed-point command completes in one prismatic action with zero BSP
  candidates and zero residuals: 2,823 bounded intermediate pieces coalesce to
  106 certified convex pieces, relative STEP volume delta is about `2.46e-8`,
  elapsed time is about 63 seconds, and peak RSS is about 122 MiB.
- **Revolution-recognition progress (2026-08-30):** cylindrical, conical,
  toroidal, spherical, and compatible planar supports are now classified by a
  tolerance-aware shared axis. Reports and Lua expose axis, angular extent,
  full/partial status, axial levels, analytic face counts, and radial seam
  count. Non-convex full revolutions use a bounded sequential arrangement of
  axial meridian-event planes before generic BSP; exact results short-circuit
  candidate search. The operation is available through sessions, fixed-point,
  single-pass preprocessing, replay, compact recipes, and Lua. Tests cover a
  rotated stepped shaft with exact decomposition and STEP round trip, partial
  revolution seams, rotated cone and torus recognition, and a non-coaxial
  negative case. The toy-car model remains correctly outside this strict
  recognizer because its individual feature-rich solids do not have one shared
  revolution axis; it therefore continues to the bounded general pipeline.
- **Definition-cache progress (2026-08-30):** verified placement-independent
  geometry digests now drive both recognition and conversion reuse. Within an
  analysis, topology is computed once per definition and axes, origins, pipe
  ports, and normals are transported to rigid occurrences; reports expose
  recognition hit/miss metrics. `inspect --project` atomically publishes one
  immutable recognition certificate per geometry+policy digest through the
  existing validated artifact/cache index, and warm runs reject anything
  whose geometry, policy, certificate, or artifact digest differs. Within the
  fixed-point coordinator, a certified convex-piece set or exact Boolean body
  is converted once, rigidly transformed for a repeated occurrence, checked
  for OCCT validity and volume conservation, and recorded as a replayable
  `reuse_definition_conversion` operation. Conversion hit/publication metrics
  are available in JSON and Lua. The toy-car import now persists successfully
  after making nested occurrence IDs parent-specific; its analysis records one
  same-run repeated-definition hit, while project cache runs move from six
  cold publications to six warm hits. Focused transformed-pipe, project
  reopen, recipe/replay, and Lua tests pass.
- **Contained-medium v1 progress (2026-08-30):** full-fill cylindrical and
  conical lumen generation is implemented for a recognized straight annular
  pipe occurrence. Conical recognition retains the inner and outer radii at
  both ports, handles arbitrary and reversed axis orientation, and certifies
  exact `outer frustum - inner frustum` reconstruction before coordinator use.
  The proposal reconstructs the lumen in OCCT, checks solid validity, closure,
  leakage, and positive-volume overlap against active material geometry, and
  refuses unknown Boolean results. Apply is transactional and requires explicit
  human or authorized-agent approval; it creates an exact synthetic cylinder
  or frustum, material cell, semantic metadata, and derived-region certificate.
  The fluid
  material ID must currently end in its positive MCNP material number (for
  example `material:7`); a future persistence migration should replace this
  compatibility convention with a dedicated transport-material field.
  Requests can also be staged without geometry. Unresolved staged ports survive
  analysis, reports, logs, compact Lua recipes, and replay, and appear as
  blocking Qt decision items; successful apply updates the staged request in
  place. Generated regions are included in final STEP/MCNP and rendered as cyan
  viewer overlays. Focused OCCT obstruction, authorization, material-number,
  port-decision, replay, Lua, STEP, and MCNP tests pass. Typed autonomy payloads
  preserve the host,
  material, exactness requirement, and every port disposition. Automatic
  application requires an immutable user policy that explicitly configures the
  referenced liquid/gas material; the LLM cannot create or modify this policy.
  Context JSON advertises the policy, durable queues preserve it and the typed
  request across resume, and execution always regenerates the OCCT certificate.
  Qt queue review renders a translucent cyan cylinder/frustum preview with the
  validity, closure, leak, overlap, volume, and unresolved-port evidence before
  manual approval. Its typed editor lists configured fluid materials and
  recognized pipe occurrences, rebuilds ports from current topology, permits a
  disposition choice per port, and disables continuation until the fresh
  certificate passes. The edited request is staged before final geometry
  approval. Phase 5 is complete. The complete 57-test suite passes with the
  cylindrical, conical, staged-request, typed-autonomy, and Qt editing
  implementation.
- **Contained-medium v2 progress (2026-08-31):** Phase 6 is implemented.
  Requests now describe occurrence-qualified multi-host networks, reciprocal
  port connections, open/closed valve state, optional gas-complement material,
  selected internal obstacles, gravity/level or target-fraction fill, pocket
  policy, and a circuit worker bound. Straight and conical occurrences become
  exact cylinder/frustum cells; reducers are retained explicitly and
  non-collinear junctions are classified as elbows. A face or interior-point
  seed can recover a lumen-side wall from a single complex component, stop at
  annular port faces, cap and orient the shell, and classify two-port bends,
  three-port tees, or larger manifolds. Cylindrical manifolds are recovered as
  an exact primitive union; other analytic channels use the existing
  certified shatter path. Selected obstacle components are decomposed when
  needed and emitted as Boolean subtraction cells. Partial fills use an exact
  gravity half-space (or a binary-searched target fraction); liquid and gas
  are checked for closure, positive-volume overlap, union coverage, protected
  interfaces, and disconnected pockets. Derived Boolean bodies now retain
  arbitrary multi-solid OCCT shapes for STEP while libalea receives one union
  cell per material phase. Network and phase evidence is present in reports,
  Lua, autonomy, decision logs, compact recipes, replay, and Qt review; opening
  a multi-host request in Qt no longer destroys it by selecting only the first
  host. Each request remains an isolated serial OCCT circuit job, while the
  existing project scheduler may run multiple jobs up to the request/policy
  worker bound. Focused tests cover a connected two-segment circuit, exact
  half liquid/gas coverage, a true toroidal elbow, a Boolean tee recovered
  from an interior point, selected obstacle subtraction, apply, materials, and
  authorization. The complete 58-test repository suite passes.
  `pi_csgify` version `0.4.0` exposes the complete v2 request
  directly (network hosts/peers, point/face seeds, valve state, obstacles,
  partial-fill parameters, gas complement, pocket policy, and worker bound),
  and both its development and installed copies pass all 20 extension tests.
- **libalea alignment audit (2026-08-31):** Phases 7 and 8 were corrected after
  auditing the current libalea source and project direction. libalea already
  provides adaptive octree void generation, per-region local complements,
  deterministic bounded parallel classification, transactional void commit,
  primitive/export surface deduplication, complement-aware Boolean trees,
  extensive simplification including proof-assisted simplification,
  hierarchical spatial queries, universes/fills/transforms/lattices,
  deterministic void merging, and complete point/grid/ray/slice coverage
  diagnostics. The remaining work is primarily a csgify-owned versioned bridge,
  stable provenance mapping, certificate aggregation, persistent orchestration,
  and ITER qualification. libalea changes are limited to generic public-API or
  measured scaling gaps that are useful independently of csgify.
- **Target:** large ITER port and similar engineering assemblies containing
  repeated components, thin walls, cooling channels, pipes, shielding,
  intentional material regions, and large surrounding void domains.
- **Baseline:** the exact-first decomposition, bounded approximation ledger,
  hard no-overlap emission gate, modeled-domain coverage contract, Qt review,
  autonomy policy, decision log, replay recipe, compacted final STEP, and MCNP
  emission described in the existing plans are retained.
- **Primary architectural decision:** csgify owns CAD interpretation and the
  construction of non-overlapping material regions; libalea owns generic void
  generation and MCNP-oriented Boolean/cell optimization.

## 1. Objectives

The implementation must make conversion of an ITER-scale model:

1. **Correct:** every emitted point in the modeled domain belongs to exactly
   one material or explicit void cell. Positive-volume overlap and unknown
   overlap remain blocking conditions for production MCNP output.
2. **Scalable:** repeated CAD definitions are converted once and instantiated;
   expensive work is spatially local, checkpointed, resumable, and isolated in
   bounded worker processes.
3. **Semantically useful:** names, descriptions, material intent, engineering
   role, comments, source CAD identity, and generated-region provenance survive
   through STEP, conversion reports, libalea, MCNP comments, and recipes.
4. **Controllable:** a human or policy-authorized LLM may request missing
   physical media such as water, gas, or liquid metal inside mechanical pipe
   components, with explicit boundaries and auditable decisions.
5. **Deterministic and reproducible:** stable IDs, geometry fingerprints,
   canonical ordering, versioned policies, and content-addressed artifacts
   produce the same result from the same inputs.
6. **Inspectable:** the Qt 6 viewer presents unresolved topology, proposed
   overlap ownership, approximations, derived media, and void coverage with
   sufficient evidence for a decision.

## 2. Non-goals and safety constraints

- Do not duplicate general void generation in csgify. Improve libalea when
  void construction, complement normalization, universe construction, or MCNP
  Boolean optimization needs improvement.
- Do not silently infer water, gas, or another process medium merely because a
  CAD component resembles a pipe. Medium generation requires an explicit user
  request or an LLM action authorized by an immutable user policy.
- Do not equate a requested medium with an approximation. It is a derived
  physical region and has its own provenance/certification ledger.
- Do not permit a volume approximation budget to waive MCNP overlap safety.
- Do not collapse protected walls, boundaries, materials, or safety-relevant
  topology without an operation-specific authorization.
- Do not require the entire assembly to reside in memory at once.
- Do not use unstable traversal order, process completion order, or pointer
  identity in output IDs or numbering.

## 3. Responsibility boundary

### 3.1 csgify responsibilities

csgify owns:

- STEP/XCAF import, healing, units, assembly hierarchy, names, colours, and
  source identities;
- separation of component definitions from transformed instances;
- geometry recognition, exact decomposition, OCCT unification, shatter, and
  explicitly budgeted approximation;
- construction and certification of all non-void material regions;
- requested pipe/channel medium extraction and material assignment;
- deterministic single-owner repair of small source overlaps;
- protected-boundary enforcement and approximation/derived-region ledgers;
- modeled-domain definition and export of a versioned intermediate model;
- user/LLM decisions, review packets, reports, recipes, and final STEP output.

### 3.2 libalea responsibilities

libalea owns:

- conversion of certified material regions to canonical CSG expressions;
- shared-surface canonicalization and tolerance-aware surface reuse;
- complement and Boolean normalization;
- generic void generation within the declared modeled domain;
- spatial subdivision and decomposition of void space;
- MCNP universes, transforms, lattice/instance optimization where applicable;
- MCNP-specific cell/surface simplification and deterministic numbering;
- final membership/coverage validation and MCNP deck emission;
- generic exact/sampled membership evidence and diagnostics for gaps, overlaps,
  undefined fills, unresolved queries, and model limitations. The csgify bridge
  assembles this evidence into the project-level coverage certificate together
  with source IDs, policy, artifact digests, and reproducibility metadata.

### 3.3 Interface rule

The handoff is a versioned csgify-owned data contract implemented by an adapter
over libalea's opaque public C API, not direct access to either project's
internal structures. It carries geometry definitions, instances, material
identities, metadata references, protected boundaries, modeled domains, and
conversion certificates. libalea must not depend on OCCT, BREP, csgify stable-ID
types, the csgify SQLite project store, or csgify workflow policy.

The normal in-process path maps the contract directly into `alea_system_t`.
A portable manifest is a csgify artifact/checkpoint format and must describe
analytic CSG definitions or reference csgify-owned source artifacts; it does not
require libalea core to parse BREP or the manifest. A neutral optional adapter
may be added to libalea only when it is useful to non-csgify clients and follows
libalea's opaque-handle, C-single-source-of-truth design.

## 4. Target pipeline

```text
STEP/XCAF
   |
   v
Assembly graph + semantic metadata + stable IDs
   |
   +--> definition/instance recognition and geometry cache
   |
   v
Local CAD validation/healing and spatial index
   |
   v
Recognition-first exact conversion
   |  convex -> extrusion/revolution/pipe -> BSP -> shatter
   |  -> user-budgeted approximation -> review
   v
Certified, disjoint mechanical material regions
   |
   +--> requested contained-medium generation and certification
   |
   +--> material-aware overlap ownership and exact repair
   |
   v
Modeled-domain package + material regions + instances + metadata
   |
   v
libalea: CSG normalization, generic void generation, universes/transforms
   |
   v
Coverage certificate: membership count exactly one throughout domain
   |
   +--> compacted final STEP (default)
   +--> MCNP deck (default)
   +--> report, decision log, recipe, artifact manifest
```

No downstream stage may silently repair or reinterpret an upstream failed
certificate. It must return a structured diagnostic or review request.

## 5. Core data model

### 5.1 Stable identities

Introduce distinct IDs for source identity, geometry identity, and occurrence:

```cpp
using AssemblyNodeId = StableId;
using ComponentDefinitionId = StableId;
using ComponentInstanceId = StableId;
using GeometryDefinitionId = StableId;
using RegionId = StableId;
using MaterialId = StableId;
using MetadataId = std::uint32_t;
using StringId = std::uint32_t;
```

- `AssemblyNodeId`: stable XCAF hierarchy path plus source document identity.
- `ComponentDefinitionId`: source definition/part identity.
- `ComponentInstanceId`: definition plus occurrence path.
- `GeometryDefinitionId`: tolerance-normalized, transform-invariant geometry
  digest; shared by repeated and mirrored occurrences where safe.
- `RegionId`: stable material, derived-medium, or generated-void region.
- `MaterialId`: stable semantic material identity, never an incidental MCNP
  cell number.

Retain the existing fingerprint as a revision-recovery hint, but add a strong
canonical geometry digest. A weak fingerprint collision must never cause cache
reuse without an exact equivalence check.

### 5.2 Assembly graph

Replace the current flat `AssemblyNode` as the primary representation with:

```cpp
struct ComponentDefinition {
    ComponentDefinitionId id;
    GeometryDefinitionId geometry_id;
    MetadataId metadata_id;
    std::vector<StringId> source_labels;
};

struct ComponentInstance {
    ComponentInstanceId id;
    ComponentDefinitionId definition_id;
    AssemblyNodeId parent_id;
    Transform transform;
    MetadataId metadata_id;
    std::optional<MaterialId> material_override;
    bool active = true;
};

struct AssemblyNode {
    AssemblyNodeId id;
    AssemblyNodeId parent_id;
    MetadataId metadata_id;
    std::vector<AssemblyNodeId> children;
    std::vector<ComponentInstanceId> instances;
};
```

Migration adapters must continue to populate the existing public report fields
until the older API is deprecated.

### 5.3 Semantic strings and comments

Make descriptive information first-class rather than embedding it in labels:

```cpp
enum class AnnotationOrigin {
    cad,
    user,
    csgify,
    libalea,
    llm,
    imported_recipe,
};

enum class ReviewState {
    informational,
    proposed,
    approved,
    rejected,
    superseded,
};

struct Annotation {
    StringId key;
    StringId value;
    AnnotationOrigin origin;
    ReviewState review_state;
    bool user_verified = false;
};

struct EntityMetadata {
    MetadataId id;
    StringId stable_name;
    StringId display_name;
    StringId description;
    StringId engineering_role;
    StringId subsystem;
    StringId source_cad_label;
    StringId source_component_path;
    std::vector<StringId> tags;
    std::vector<Annotation> annotations;
};
```

Use an interned UTF-8 string table so repeated ITER subsystem, material, and
component names do not multiply memory use. Metadata applies to:

- assemblies, component definitions, and instances;
- source solids and converted material regions;
- generated liquid/gas regions and generated void cells;
- surfaces, transforms, universes, and material definitions;
- conversion decisions, review items, approximations, and certificates.

Standard annotation keys initially include:

```text
source_file, source_component_path, source_part_number, subsystem,
engineering_role, material_name, material_source, density_basis,
safety_class, protected_boundary, overlap_priority, instance_index,
conversion_strategy, approximation_state, review_state, user_comment,
llm_suggestion, decision_rationale, derived_region_request
```

Keep user comments, generated explanations, LLM suggestions, and final decision
rationales as separate annotations with origin and approval state. An LLM may
propose text but must not overwrite user-authored text.

### 5.4 Digest separation

Use separate hashes so editing a comment does not invalidate geometry work:

```text
geometry_digest   = canonical geometry + geometric tolerances
conversion_digest = geometry_digest + conversion/approximation policy
model_digest      = conversion outputs + materials + semantic metadata
output_digest     = model_digest + libalea/MCNP policy and versions
```

Geometry artifacts key on `conversion_digest`. Reports and decks key on the
later digests. Record the compiler, OCCT, csgify, and libalea versions.

### 5.5 Materials and regions

Define material independently from geometry:

```cpp
enum class MaterialPhase { solid, liquid, gas, vacuum, unknown };

struct MaterialDefinition {
    MaterialId id;
    MetadataId metadata_id;
    MaterialPhase phase;
    double density;
    StringId density_unit;
    StringId composition_reference;
};

enum class RegionOrigin {
    source_cad,
    derived_contained_medium,
    overlap_repair,
    libalea_void,
};

struct MaterialRegion {
    RegionId id;
    GeometryDefinitionId geometry_id;
    MaterialId material_id;
    MetadataId metadata_id;
    RegionOrigin origin;
    std::vector<ComponentInstanceId> source_instances;
    bool protected_boundary = false;
};
```

## 6. Recognition-first geometry strategy

Classify each unique geometry definition before choosing an algorithm. Store
the evidence and confidence, not only the selected label.

Priority order:

1. already-convex analytic cell;
2. exact prismatic/extruded profile;
3. exact revolved profile;
4. pipe, annulus, elbow, tee, reducer, or constant-thickness shell;
5. repeated pattern or array that can be represented by instances;
6. exact half-space/BSP decomposition;
7. bounded-memory exhaustive shatter;
8. policy-authorized approximation;
9. human/LLM review.

### 6.1 Extrusion recognition

- cluster face normals with configured angular tolerance;
- find candidate extrusion axes and planar end levels;
- project lateral topology into a 2D polygon-with-holes arrangement;
- decompose the 2D cross-section into convex polygons;
- extrude the polygons between topology-event levels;
- certify union equality and volume conservation with OCCT;
- preserve internal holes as Boolean negatives until libalea normalization.

### 6.2 Revolution recognition

- cluster cylindrical, conical, toroidal, and planar supports by common axis;
- recover a meridian profile and angular extent;
- divide at topology events and unsupported surfaces;
- emit canonical revolved primitives or bounded exact pieces;
- certify against the source solid.

### 6.3 Pipe and channel recognition

- identify coaxial inner/outer cylindrical or conical supports;
- build a connectivity graph through elbows, reducers, tees, and manifolds;
- label wall side, wetted side, ports, caps, branches, and embedded obstacles;
- expose this topology to requested contained-medium generation;
- never treat the inferred lumen as a material region until a request is
  accepted.

### 6.4 General decomposition

Retain progressive BSP and shatter but run them per unique definition, with:

- a recognition-derived candidate priority;
- explicit reason codes such as `residual_requires_shatter` instead of the
  ambiguous `no_improving_candidate` when candidates cannot immediately peel
  a convex cell;
- bounded splitter calls, pieces, time, resident memory, and temporary disk;
- incremental OCCT unification of pieces that reduce to one intersection;
- exact certificates before cache publication.

## 7. Requested contained-medium generation

### 7.1 Semantics

Mechanical CAD usually models the pipe wall, not the water, gas, coolant,
liquid metal, or other medium inside it. csgify must support an explicit
`generate-contained-medium` operation that derives the occupied lumen region
from selected host geometry.

This is distinct from libalea void generation:

- a requested medium is a physical material cell constructed and certified by
  csgify;
- libalea receives it like every other material region;
- libalea generates only the remaining intentional void.

### 7.2 Request contract

```cpp
enum class FillMode {
    full,
    level,
    bounded_segment,
};

enum class PortDisposition {
    connect_to_port,
    cap_here,
    open_to_domain,
    interface_to_external_model,
    unresolved,
};

struct MediumPortDecision {
    StableId port_id;
    PortDisposition disposition;
    std::optional<StableId> peer_port_id;
    MetadataId metadata_id;
};

struct MediumGenerationRequest {
    StableId id;
    MetadataId metadata_id;
    std::vector<ComponentInstanceId> host_component_ids;
    std::vector<GeometricSeed> seed_points_or_faces;
    MaterialId material_id;
    FillMode fill_mode = FillMode::full;
    Vec3 gravity_direction;
    std::optional<double> fill_level;
    std::optional<double> target_volume_fraction;
    std::vector<MediumPortDecision> ports;
    std::vector<ComponentInstanceId> internal_components_to_subtract;
    bool require_exact_geometry = true;
};
```

The request must specify material and density, selected circuit/hosts, fill
state, treatment of every boundary port, modeled segment, internal components
to subtract, and permitted approximation. `unresolved` ports block commit and
emission.

### 7.3 Extraction algorithms

Implement two complementary strategies:

**Surface-based lumen extraction**

1. identify connected wetted faces from recognized pipe topology;
2. collect closed end wires and paired connection ports;
3. add zero-thickness closure faces only for explicit `cap_here` decisions;
4. sew/fix a closed shell and make a solid;
5. fuse connected pipe, elbow, reducer, tee, and manifold lumen segments;
6. subtract valve parts, probes, inserts, and other selected internal solids;
7. split into connected regions and assign stable derived-region IDs.

**Seed-based contained-region extraction**

1. start from a user/LLM-provided point or selected wetted face;
2. construct a bounded local enclosing domain;
3. subtract surrounding mechanical material;
4. flood the connected complement containing the seed;
5. stop at explicit caps/interfaces and reject a path to an unintended domain
   boundary;
6. use this path for embedded cooling channels and geometry whose pipe wall
   cannot be reliably recognized globally.

Both strategies are local to the requested circuit. Neither performs global
void generation.

### 7.4 Partial fill and multiple phases

For a partial liquid fill:

- require a gravity direction and either fill elevation or target volume
  fraction;
- intersect the certified lumen with the liquid half-space;
- optionally assign the complementary connected lumen to a gas material;
- report disconnected pockets and require an explicit policy for them;
- preserve the liquid/gas interface as a protected material boundary;
- certify that the liquid, gas, wall, and remaining void are mutually
  disjoint.

### 7.5 Derived-region ledger

Do not charge exact medium construction to the approximation ledger. Add:

```cpp
struct DerivedRegionRecord {
    RegionId region_id;
    StableId request_id;
    StringId operation;
    std::vector<ComponentInstanceId> host_components;
    MaterialId material_id;
    double generated_volume;
    std::vector<StableId> connected_ports;
    std::vector<StableId> capped_ports;
    std::vector<ComponentInstanceId> subtracted_components;
    bool closed;
    bool leak_test_passed;
    bool overlap_test_passed;
    bool user_approved;
    MetadataId metadata_id;
};
```

An approximate lumen construction records both a derived-region record and a
normal approximation-ledger entry.

### 7.6 Commit certificate

A contained-medium proposal may commit only when:

- the OCCT solid is valid and closed;
- unresolved ports equal zero;
- no unintended path reaches the modeled-domain exterior;
- intersection volume with pipe walls and other material regions is zero
  within the certified tolerance;
- subtraction of internal components succeeded or was explicitly excluded;
- generated volume, connected components, caps, material, density, and all
  policies appear in the certificate;
- the operation is explicitly authorized by the user policy.

## 8. Overlap ownership for material components

Maintain the rule that exactly one component owns every point in modeled
space. For a sufficiently small source overlap, ownership normally has little
physical effect, but the final geometry must still be disjoint.

### 8.1 Ownership priority

Resolve ownership in this order:

1. protected-boundary or protected-material rule;
2. explicit user decision;
3. policy-authorized LLM decision with recorded rationale;
4. component/material priority metadata;
5. containment rule, such as an insert owning space inside a host;
6. minimal-removal score and Boolean robustness;
7. stable-ID tie-breaker.

The existing stable-ID default remains deterministic but becomes the last
tie-breaker rather than the main engineering rule.

### 8.2 Transaction

- detect the local positive-volume intersection using the spatial index;
- trial both ownership directions when allowed;
- measure removed volume and topology change exactly;
- reject damage to protected geometry or budget overflow;
- choose the certified policy winner;
- subtract the intersection from every non-owner;
- update the approximation ledger and source lineage;
- re-run local overlap tests, then the hard global emission gate;
- report the owner and affected components even for automatic repair.

An unknown intersection result, multi-owner result, or Boolean failure blocks
that connected spatial partition.

## 9. Scaling architecture

### 9.1 Definition/instance reuse

- canonicalize every component definition in local coordinates;
- detect equivalent transformed definitions with a strong geometry digest and
  exact equivalence confirmation;
- convert each unique definition once;
- store converted cell templates independently of transforms;
- instantiate templates in csgify reports and as libalea/MCNP universes and
  transforms where semantics permit;
- keep instance material and metadata overrides separate;
- invalidate only the changed definition and its dependent instances.

Mirrored instances require an explicit orientation-safe equivalence test.

### 9.2 Spatial index

Build a persistent BVH or R-tree over instance bounding boxes and a second
index over resulting material regions. Use it for:

- overlap candidate generation;
- connected-neighbour and pipe-port matching;
- local coverage checks;
- work partitioning and review framing;
- libalea void-generation tiles.

Never compare every component with every other component. Record candidate and
exact-test counts to detect accidental quadratic regressions.

### 9.3 Artifact store and checkpoints

Use a content-addressed artifact directory plus SQLite metadata:

```text
<work-dir>/
  project.sqlite
  objects/<digest-prefix>/<digest>.brep
  reports/<digest>.json
  logs/<job-id>.jsonl
  recipes/<run-id>.json
  manifests/<run-id>.json
  tmp/<worker-id>/
```

SQLite stores entities, dependency edges, job state, policy/version hashes,
artifact references, diagnostics, and ledgers. Large OCCT BREP and report
payloads stay as immutable files. Publish an artifact by write-to-temporary,
fsync/close, digest validation, and atomic rename.

Checkpoint boundaries:

- import and assembly graph complete;
- each geometry definition healed/classified;
- each definition conversion certified;
- each derived medium committed;
- overlap repair completed per spatial partition;
- csgify/libalea handoff package complete;
- each libalea void tile/universe complete;
- final coverage certificate and outputs complete.

Resume validates source hash, policy hashes, tool/OCCT/libalea versions, and
artifact digests. A mismatch invalidates only dependent work.

### 9.4 Bounded worker processes

Use process workers rather than unrestricted OCCT threads:

- one definition or spatial partition per job;
- configured resident-memory, wall-time, splitter-call, piece, and temporary
  disk limits;
- parent process owns scheduling and deterministic result commit;
- a crash or `bad_alloc` marks only that job failed;
- retry with a lower-memory strategy or smaller partition;
- stdout/stderr and structured progress are captured per job;
- cancellation leaves committed artifacts valid and removes only identified
  temporary worker files.

Do not kill a worker solely from sampled RSS without allowing for OCCT allocator
retention. Enforce hard limits at the process boundary and use operation-level
growth estimates to choose strategies before allocation.

### 9.5 Deterministic scheduling

Jobs may execute in parallel, but commits and numbering follow sorted stable
IDs. Cache hits and worker completion timing must not change:

- geometry IDs;
- cell/surface/universe numbers;
- recipe action order;
- report ordering;
- MCNP comments.

## 10. csgify-to-libalea contract

Create a versioned schema in csgify with an in-process adapter that constructs
and inspects libalea exclusively through its opaque public C API:

```cpp
struct AleaModelPackage {
    SchemaVersion schema_version;
    UnitSystem units;
    std::vector<GeometryDefinition> definitions;
    std::vector<RegionInstance> material_regions;
    std::vector<MaterialDefinition> materials;
    std::vector<EntityMetadata> metadata;
    StringTable strings;
    std::vector<ModeledDomain> domains;
    std::vector<ProtectedBoundary> protected_boundaries;
    GeometrySafetyCertificate csgify_certificate;
    OutputPolicy output_policy;
};
```

The bridge returns:

```cpp
struct AleaConversionResult {
    SchemaVersion schema_version;
    std::vector<CellDefinition> cells;
    std::vector<SurfaceDefinition> surfaces;
    std::vector<UniverseDefinition> universes;
    std::vector<TransformDefinition> transforms;
    std::vector<VoidRegionRecord> void_regions;
    CoverageCertificate coverage;
    std::vector<Diagnostic> diagnostics;
    ArtifactManifest artifacts;
};
```

The schema supports content-addressed chunk references so a complete ITER model
is not copied into one JSON object. JSON is suitable for manifests, metadata,
and stable-ID mappings. The in-process adapter converts csgify's certified
analytic geometry into libalea surfaces, Boolean nodes, cells, universes, and
transforms. BREP remains a csgify artifact and is never passed to libalea core.

The adapter validates the csgify schema and required features before work
starts, records the libalea version and relevant capabilities, and fails closed
when the available public API cannot preserve a required semantic. libalea does
not need to understand the csgify schema. A separately serialized neutral Alea
CSG format is optional future work and must not be conflated with this bridge.

Stable csgify IDs and rich UTF-8 metadata live in a bridge sidecar keyed to the
numeric libalea cell, surface, universe, and transform IDs. Concise sanitized
descriptions are also written through libalea's existing cell/material comment
APIs for MCNP emission. This avoids freezing csgify metadata structures into
`alea_system_t` while preserving complete round-trip provenance.

## 11. libalea reuse and measured ITER-scale gaps

### 11.1 Existing capabilities to reuse

The current libalea implementation already contains the Phase 8 geometry
foundation. The bridge must reuse rather than duplicate:

1. adaptive octree void generation in an explicit bbox or CSG region;
2. per-region complements built from bbox-overlapping occupied cells;
3. deterministic bounded parallel void classification and execution metrics;
4. transactional void generation, commit, rollback, graveyard, and merge APIs;
5. automatic tolerance-aware primitive deduplication and canonical export
   surface maps;
6. complement/difference evaluation and complement-aware cell handling;
7. Boolean normalization, factoring, pruning, and proof-assisted cell
   simplification;
8. hierarchical spatial indexing and complete occurrence/path queries;
9. universes, fills, transforms, lattices, extraction, merge, and optional
   flattening;
10. deterministic greedy/face-sorted void merging;
11. exact ownership queries plus point, grid, ray, and slice coverage
    diagnostics for gaps, overlaps, undefined fills, and unresolved work;
12. MCNP comments, materials, transforms, canonical surfaces, and export.

### 11.2 Remaining integration and qualification work

Implement the following around existing libalea capabilities:

1. map each certified csgify geometry definition once into a libalea universe
   or definition-local cell set, then instantiate it through fills/transforms
   when the semantics and MCNP target permit reuse;
2. create modeled-domain voids through `alea_void_generate_in_region_ex` (or
   the equivalent stable public API), using explicit resource bounds and
   retaining libalea execution statistics;
3. aggregate libalea's complete-coverage classifications and validator findings
   into the project-level `CoverageCertificate`, preserving whether evidence is
   exact, sampled, truncated, or unresolved;
4. maintain a csgify sidecar mapping every libalea/MCNP entity and diagnostic to
   source definition, occurrence, material region, derived medium, domain/tile,
   and protected-boundary metadata;
5. use the csgify artifact store and worker scheduler for persistent checkpoints
   and retries. Do not add SQLite, csgify job types, or project policy to
   libalea;
6. partition modeled domains into deterministic bridge-level jobs only when a
   whole-domain libalea run exceeds measured limits. Add a generic bounded
   region-batch API to libalea only if repeated public calls cannot provide the
   required correctness or performance;
7. verify deterministic cell/surface/universe/transform numbering at the bridge
   boundary and record all renumbering in the sidecar;
8. run ITER-scale memory, runtime, expression-size, universe-reuse, and
   interruption/resume qualification before proposing internal engine changes.

### 11.3 Conditional libalea enhancements

The following are legitimate libalea improvements, but only after a benchmark
demonstrates a remaining generic engine bottleneck:

- structural hash-consing or n-ary associative Boolean nodes to reduce the
  dominant large-model node count, consistent with libalea's existing memory
  plan;
- hierarchical-index-backed neighbor selection inside void generation instead
  of the current conservative per-region bbox scan;
- a generic aggregate coverage-report/certificate API built from the existing
  complete-coverage validators;
- deterministic bounded region-batch import/export helpers for process-level
  checkpointing;
- protected-boundary tags only if they can be expressed as a generic transport
  geometry concept rather than csgify-specific metadata.

These enhancements must preserve libalea's design: `alea_system_t` remains
opaque, C remains the single source of truth, hierarchy is preferred over
flattening for repeated geometry, and no OCCT/csgify dependency enters libalea
core.

### 11.4 Alignment evidence and review gate

The 2026-08-31 audit used the current libalea implementation and its documented
project direction:

- `docs/ARCHITECTURE.md`: one self-contained `alea_system_t`, automatic
  primitive deduplication, lazy universe evaluation, and hierarchical spatial
  queries;
- `docs/PLAN_PUBLIC_API_OPAQUE_SYSTEM.md`: keep `alea_system_t` opaque and add
  read-only value/accessor APIs rather than exposing internal storage;
- `docs/plan_c_single_source_of_truth.md`: mutations belong in the C system,
  with language bindings/adapters remaining projections over that state;
- `docs/PLAN_MEMORY_FOOTPRINT.md`: hierarchy is a memory saver; n-ary Boolean
  nodes are a possible measured future optimization, not an assumed prerequisite;
- `src/core/alea_void.c` and `tests/integration/test_void.c`: local-complement,
  transactional, deterministic parallel void generation and merge are existing
  tested behavior;
- `include/alea_geo_validator.h` and complete-coverage APIs in `include/alea.h`:
  gap/overlap/undefined/unresolved evidence already exists at point, grid, ray,
  slice, hierarchy, and occurrence levels.

Before modifying libalea, record a focused benchmark and an API-gap statement:

```text
required generic operation or evidence
existing public API attempted
correctness/performance result
why composition in the csgify adapter is insufficient
proposed format-neutral libalea API or internal optimization
standalone libalea tests and non-csgify use case
```

No libalea change enters the implementation backlog without this review gate.

The bridge-level coverage certificate must report:

```text
domain definition and hash
material cell count
intentional void cell count
overlap count and maximum measured overlap
unclassified region count
unknown-classification count
exact checks performed
sampling method/count/seed if sampling supplements exact checks
certificate status and limitations
```

## 12. Commands and public API

Add session/API operations first; keep the CLI a thin client.

Proposed CLI shape:

```sh
csgify import model.step --project project.csgify
csgify classify --project project.csgify
csgify convert --project project.csgify --jobs 8 --worker-memory 6GiB
csgify medium propose --project project.csgify \
  --hosts cooling_loop_A --material water --full
csgify medium apply --project project.csgify --request water_loop_A.json
csgify overlap resolve --project project.csgify --policy ownership.json
csgify review --project project.csgify
csgify emit --project project.csgify --step final.step --mcnp final.i
csgify compact --project project.csgify --recipe final.recipe.json \
  --preserve-intermediates=false
csgify resume --project project.csgify
```

Requirements:

- final compacted STEP and MCNP are produced by default after successful gates;
- intermediate STEP preservation remains a compactor option;
- every mutating operation supports propose/dry-run and apply modes;
- machine APIs expose typed data directly; extensions need not shell out to
  Python merely to parse JSON, though Python remains available for orchestration;
- JSON output remains for interoperability and debugging;
- all commands have bounded work options and structured progress events.

## 13. Qt 6 review experience

Add project-scale navigation and focused decision modes:

- assembly tree with subsystem/component/material filters;
- search across names, tags, comments, IDs, decision state, and diagnostics;
- lazy geometry loading and level-of-detail rendering;
- isolate/highlight source, proposed result, removed volume, added volume,
  overlap region, protected boundary, lumen, cap, and unmatched port;
- synchronized table and 3D selection with stable-ID display;
- explicit actions: approve, reject, change owner, connect ports, cap port,
  mark external interface, select material, set fill level, protect boundary;
- evidence panel containing volumes, tolerances, topology changes, affected
  components, policy basis, certificate result, and LLM rationale;
- undo/redo through decision-log events rather than hidden widget state;
- no closing or advancing when a numeric shortcut is pressed unless the
  selected action actually committed;
- automatic low-risk decisions remain visible in a completed-decisions list.

For a medium request, the viewer must render wetted surfaces and proposed
medium as translucent overlays, show flow-port arrows, and prevent approval
while an unresolved port or leak remains.

## 14. LLM autonomy policy

The LLM may interpret semantic intent and propose decisions, but the user owns
the policy envelope.

Policy classes:

- **automatic and report:** deterministic cache reuse, harmless OCCT cleanup,
  redundant-piece unification, small removable detail within a supplied
  budget, and overlap ownership when all configured rules identify one owner;
- **automatic only with explicit project policy:** contained-medium generation
  for named circuits, cap conventions, material assignment, approximate lumen
  recovery, and protected-boundary changes;
- **always review by default:** unresolved port treatment, competing material
  ownership, safety-classified boundary modification, disconnected liquid/gas
  pockets, or a request exceeding any geometry budget;
- **never autonomous:** waiving positive-volume overlap, waiving unknown
  coverage, inventing a material/density without a configured source, or
  emitting a production deck after a failed gate.

Every autonomous action records input evidence, policy rule, proposed action,
confidence, effect metrics, result certificate, and a short rationale. The
replay recipe contains the accepted decision, not a dependency on asking the
LLM again.

## 15. Reports, recipes, and output comments

### 15.1 Report additions

Add sections for:

- assembly definition/instance counts and reuse ratio;
- cache hits/misses and invalidation causes;
- recognition strategies and fallbacks;
- worker peak RSS, duration, retries, and failure reason;
- semantic metadata completeness;
- medium requests, ports, certificates, and derived-region ledger;
- overlap ownership and exact removed volume;
- csgify/libalea schema/tool versions;
- void-generation and membership/coverage certificate;
- final artifacts and their digests.

Counters must describe current output state, not historical attempts. For
example, if shatter eventually converts every residual, report `converted=N/N`
and list failed intermediate attempts separately.

### 15.2 Compact replay recipe

The compacted recipe records only decisions required to reproduce the final
state:

- source file digest and unit interpretation;
- import/healing policy;
- stable-ID mappings or revision matches;
- non-default decomposition and approximation decisions;
- metadata/material assignments;
- medium-generation requests and every port decision;
- overlap owners;
- modeled domains and libalea void policy;
- output/numbering/comment policy;
- required implementation/schema versions.

Intermediate proposal files are optional. Final STEP, MCNP, report, decision
log, compact recipe, and artifact manifest are retained by default.

### 15.3 MCNP comments

- sanitize metadata to deterministic MCNP-safe text;
- wrap lines at a configured width without changing IDs;
- place stable region ID, display name, material, subsystem, source path, and
  concise provenance near each cell/universe;
- keep complete UTF-8 text in JSON/recipe when MCNP character limits require a
  shortened form;
- propagate libalea-generated void descriptions such as domain/tile and
  neighboring material systems.

## 16. Persistence schema

Initial SQLite tables:

```text
runs, source_documents, assembly_nodes, component_definitions,
component_instances, geometry_definitions, materials, material_regions,
string_table, metadata_entities, annotations, modeled_domains,
conversion_jobs, job_dependencies, artifacts, artifact_dependencies,
approximation_ledger, derived_region_ledger, overlap_ownership,
medium_requests, medium_ports, decisions, diagnostics, certificates
```

Add indexes on stable ID, geometry digest, parent node, subsystem, material,
annotation key/value, spatial partition, job state, and artifact digest.
Schema migrations are transactional and versioned. Recipes remain portable
without the database.

## 17. Implementation work packages

### Phase 0 — Baseline and representative corpus

**Deliverables**

- freeze current toy-car and FWTBM1 reports, recipes, peak RSS, duration, and
  output digests as regression baselines;
- obtain sanitized representative ITER subsets: repeated fasteners/components,
  thin-walled shielding, a cooling loop, a port plug sector, and a domain
  suitable for libalea void generation;
- define expected materials, intentional omissions, protected boundaries, and
  acceptable approximation policies for each model;
- add performance-test tooling that records peak RSS and artifact sizes.

**Exit criteria**

- every corpus item has a documented expected result and license/provenance;
- current tests remain green; baseline results are reproducible.

### Phase 1 — Semantic model and stable assembly identities

**Deliverables**

- string interner, `EntityMetadata`, annotations, materials, definitions, and
  instances;
- XCAF import of names, hierarchy, source labels, colours where useful, and
  instance transforms;
- strong canonical geometry digest plus exact reuse verification;
- JSON/report serialization and backward-compatible report adapters;
- tests for duplicate names, Unicode, revision rematching, repeated and
  mirrored instances, metadata-only changes, and deterministic IDs.

**Exit criteria**

- toy-car repeated parts yield fewer unique definitions than occurrences;
- changing a comment does not invalidate converted geometry;
- all source and generated entities can carry searchable metadata.

### Phase 2 — Artifact store, scheduler, and bounded workers

**Deliverables**

- SQLite project store and immutable BREP artifact store;
- dependency graph, resumable jobs, atomic artifact publication;
- process worker executable and protocol;
- resource limits, cancellation, crash recovery, retry/fallback policy;
- deterministic commit ordering and structured progress events.

**Exit criteria**

- forced worker crash and out-of-memory simulation lose no committed work;
- resume performs no already-certified Boolean operation;
- repeated definitions are converted once;
- configured concurrency cannot exceed the aggregate memory envelope.

### Phase 3 — Spatial indexing and local safety operations

**Deliverables**

- persistent instance/region BVH or R-tree;
- local overlap candidates and exact tests;
- material-aware ownership policy and transactional repair;
- local revalidation plus hard global final gate;
- spatial metrics and quadratic-regression tests.

**Exit criteria**

- toy-car overlap resolves to one owner with exact ledger evidence;
- protected and over-budget cases block;
- sparse assemblies exhibit near-local rather than all-pairs exact testing.

### Phase 4 — Recognition-first conversion

**Status:** the core deliverables below are implemented. Additional ITER
feature recognizers (elbows, reducers, tees, and more general swept profiles)
remain incremental extensions, not blockers for the bounded fallback path.

**Deliverables**

- classifier evidence model and routing;
- exact extrusion/profile decomposition;
- revolution and pipe/annulus recognition;
- definition-level conversion cache;
- improved stop reasons and shatter fallback handoff;
- incremental OCCT unification after decomposition.

**Exit criteria**

- FWTBM-like prismatic structures use 2D profile decomposition when valid;
- pipe test corpus exposes reliable wetted-surface and port topology;
- exact certificates match the existing decomposition safety standard;
- fallback stays bounded when recognition fails.

### Phase 5 — Contained-medium generation v1

**Scope:** full fill in straight cylindrical/conical pipe segments with
explicit ports/caps.

**Deliverables**

- typed request, port decisions, proposal/apply transaction;
- surface-based lumen extraction;
- derived-region ledger and certificate;
- material assignment, metadata propagation, report/recipe support;
- Qt overlays and port decision controls;
- autonomy rules requiring configured material and boundary policy.

**Exit criteria**

- generated medium is closed, valid, correctly capped, disjoint from the wall,
  replayable, and emitted as a material cell;
- an unmatched/unresolved port blocks commit;
- no medium is generated without authorization.

### Phase 5.1 — Pi extension integration

**Scope:** expose the Phase 5 contained-medium contract through the installed
`pi_csgify` without requiring the agent to construct shell commands
or temporary JSON files.

**Status (2026-08-30):** implemented in extension version `0.3.0`. The
development and installed copies expose the typed request and policy, direct
certificate evidence, and optional Qt review handoff. The 20-test extension
suite passes, including unauthorized, blocking-certificate, and authorized
exact-apply cases against the current csgify binary.

**Deliverables**

- add a typed `mediumRequest` payload to `csgify_autonomy`, including the host
  occurrences, material, fill mode, exactness requirement, and every port
  disposition/peer connection;
- add typed configured liquid/gas material policy entries and translate each
  entry to the immutable csgify invocation policy;
- expose the same configured material policy in `csgify_context` so Pi can
  reason from the policy envelope before proposing an action;
- return the complete queue item, fresh geometry certificate, blocking
  diagnostics, and policy disposition directly in tool details;
- support an explicit interactive-review handoff to the Qt 6 queue editor,
  while keeping unresolved or uncertified requests non-committable;
- update Pi prompt guidance and `/cadhelp` for medium proposal, authorization,
  review, and replay behavior;
- add extension unit/smoke tests for typed transport, policy translation,
  unauthorized review, authorized auto-apply, unresolved ports, and certificate
  reporting against the current development binary.

**Exit criteria**

- Pi can propose a complete contained-medium request without Python or a JSON
  request file;
- automatic apply occurs only for a user-configured material and fully resolved
  boundary policy, with a fresh passing certificate;
- unsafe or incomplete requests remain visible for Qt/human review and cannot
  cause MCNP emission;
- the installed extension and repository copy pass their extension test suite
  and expose the same typed contract.

### Phase 6 — Contained-medium generation v2

**Status (2026-08-31): implemented.** Connected straight/reducer networks,
seeded elbows/tees/manifolds, certified partition fallback, selected obstacle
subtraction, valve/interface semantics, partial liquid/gas partitioning,
pocket policy, bounded circuit jobs, report/replay/autonomy transport, and Qt
network review are in place. Production qualification on cooling-loop CAD is
tracked by Phase 10 rather than treated as an untested implementation claim.

**Deliverables**

- elbows, reducers, tees, and connected networks;
- seed-based embedded-channel extraction;
- selected internal-component subtraction;
- valve/interface semantics;
- partial liquid fill, gas complement, and pocket reporting;
- complex-manifold partitioning and bounded worker execution.

**Exit criteria**

- cooling-loop reference models pass leak and overlap certificates;
- liquid/gas interfaces are protected and coverage is exact;
- failure localizes to a circuit/port instead of invalidating unrelated work.

### Phase 7 — Versioned libalea handoff

**Status (2026-08-31): implemented.** The existing low-level in-process path is
now wrapped by the csgify-owned `csgify.alea-handoff` 1.0 contract. It provides
public-API capability negotiation, deterministic validation/digests, final
cell/surface/universe/fill/transform/material inventory, resolved UTF-8 semantic
sidecars, identity comments that survive Alea simplification and union
splitting, content-addressed geometry chunks, project artifact publication,
report evidence, and the `csgify handoff` CLI. Repeated definitions can be
placed through public transform and universe-fill adapters. The only libalea
change exposes its existing generic transform constructors in `alea.h`; no
csgify type, storage dependency, STEP parser, or OCCT dependency was added.
Focused clone/numbering/metadata/transform/chunk/schema tests and a CLI/project
smoke test form the cross-repository compatibility gate. Further void and
coverage certificate aggregation belongs to Phase 8.

**Deliverables**

- csgify-owned schema, validator, content-addressed chunk manifest, and
  adapter over libalea's opaque public C API;
- mapping of certified analytic definitions to existing libalea surfaces,
  Boolean nodes, cells, universes, fills, and transforms;
- import/export tests shared between repositories;
- metadata/material/instance/protected-boundary preservation through a stable
  sidecar plus concise libalea/MCNP comments;
- structured import of existing libalea diagnostics, validation evidence,
  numbering, and artifacts;
- version/capability negotiation that identifies public-API gaps without making
  libalea depend on csgify types or storage.

**Exit criteria**

- round trip preserves IDs, transforms, material regions, and metadata;
- unsupported schema features fail before conversion;
- large geometry is referenced, not copied into monolithic JSON;
- public libalea headers remain opaque and no csgify/OCCT dependency is added
  to libalea core.

### Phase 8 — Integrate and qualify libalea scalable voids and instances

**Status (2026-08-31): functional integration implemented; ITER qualification
remains.** The public `alea_void_bridge` and `csgify voids` command now route
finite-domain generation through libalea's existing bounded-parallel,
transactional exact-complement, merge, graveyard, hierarchy, and complete-point
coverage APIs. A versioned deterministic certificate separates structural
exact-complement evidence from its diagnostic ownership lattice, fails closed
on overlaps/gaps/undefined fills/truncation/unresolved queries, records resource
statistics and hierarchy-reuse savings, and maps derived cell IDs to their
domain. Project jobs publish immutable geometry/certificate artifacts and reuse
them by handoff/domain/policy digest on resume. Unit and CLI tests cover commit,
certification, blocking overlap evidence, digest tampering, artifact persistence,
and byte-identical resume. The remaining work is representative ITER-subset
runtime/RSS/expression/deck-size qualification and any benchmark-justified,
format-neutral libalea optimization; no new libalea algorithm was required for
the functional integration.

**Deliverables**

- route modeled-domain void construction through existing libalea region void,
  local complement, bounded parallel, merge, and simplification APIs;
- map repeated csgify definitions onto existing universe/fill/transform
  machinery and measure the resulting deck/memory reduction;
- persist bridge-level domain/tile jobs and artifacts through the csgify
  scheduler without duplicating a scheduler in libalea;
- aggregate existing complete-coverage evidence into a versioned certificate
  and stable source/derived-region metadata mapping;
- benchmark current surface deduplication, Boolean simplification, spatial
  neighbor filtering, and node memory before selecting any conditional
  libalea enhancement from Section 11.3;
- add only generic, independently useful libalea APIs when the public interface
  cannot express a required bounded operation or diagnostic.

**Exit criteria**

- every declared domain has exact-one membership or a blocking diagnostic;
- repeated instances reduce MCNP deck complexity measurably;
- void generation stays within configured memory on the ITER subset;
- interruption/resume reuses certified bridge artifacts without repeating
  completed domain work;
- no generic void algorithm is duplicated in csgify;
- qualification distinguishes reused existing capability from any measured
  and documented libalea engine change.

### Phase 9 — Project-scale Qt review and autonomous operation

**Status (2026-08-31): complete.** Existing Qt geometry review, typed medium
editor, persistent autonomy queue, certified automatic execution,
keyboard-safety fixes, fixed-point progress, residual review, decision logs,
recipe compaction, and resume support provide much of the base. The first new
Phase 9 slice adds strict typed ingestion of Phase 8 certificates plus
`csgify coverage status|review`: a searchable/filterable Qt panel presents
void, coverage, hierarchy-reuse, resource, and blocker evidence without raw
JSON. Unsafe ownership findings expose corrective actions but cannot be
auto-approved or dismissed into a successful state. Coverage blockers now map
into the standard persistent autonomy queue with certificate path/digest and
actionable overlap-pair evidence. Automatic mode preserves them as human review;
only reconciliation against a replacement certificate that clears the finding
marks it verified resolved. `csgify project status` now reads the durable
SQLite index without loading CAD, while `csgify project review` opens a unified
Qt window with overview, paginated definition/instance navigation, jobs,
artifacts, coverage/autonomy state, explicit stale-job recovery and failed-job
retry, and recipe compaction. OCCT geometry and the executable decision queue
are loaded only on an explicit review action. The next slice adds whole-index
assembly search, a paginated spatial-index panel with namespace/kind/owner
filtering, and focused graphical LOD: the first OCCT presentation contains only
the selected solid or piece, while **Show all** explicitly promotes the viewer
to the complete model. A qualification run on `large_pi/Triangle.stp` indexed
229 spatial bodies in 7.75 seconds with a 98,892 KiB peak RSS; subsequent
project status took 0.02 seconds and the Qt workspace started without importing
the STEP. The permanent `csgify_phase9_workspace_qualification` scenario now
exercises a real overlap-blocked libalea certificate, a non-dismissible human
review queue, offscreen Qt workspace construction with validated attachments,
replacement-certificate reconciliation, an automatically approved certified
decomposition with actor/policy audit, final STEP and MCNP artifacts, and
recipe compaction. Together with the large-model index run, this satisfies the
Phase 9 exit criteria. Further model qualification belongs to Phase 10.

**Deliverables**

- lazy assembly tree, search, filters, LOD rendering, and spatial focus;
- complete medium/overlap/approximation/coverage decision panels;
- autonomy queue driven by immutable project policy;
- completed automatic-actions log and safe keyboard navigation;
- end-to-end compact recipe and resume controls.

**Exit criteria**

- a reviewer can find and decide every blocking item without reading raw JSON;
- clicking/number shortcuts never close the viewer without a committed action;
- an authorized LLM can complete routine decisions and produce an auditable,
  deterministic recipe.

### Phase 10 — ITER qualification and release gate

Qualification ladder:

1. analytic primitives and deliberately malformed micro-models;
2. toy-car assembly for overlap and instance reuse;
3. FWTBM1 for thin-wall/prismatic decomposition and unification;
4. pipe networks with full/partial media and internal obstacles;
5. representative ITER subsystem;
6. complete port component;
7. multi-component port/sector modeled domain.

**Release criteria**

- zero positive-volume emitted overlaps;
- zero unknown overlaps;
- zero unclassified regions in every configured domain;
- every approximation fits its explicit budget and ledger;
- every medium region has a passing derived-region certificate;
- protected geometry invariants pass;
- final STEP and MCNP are produced by default after successful gates;
- interruption/resume and worker-failure tests pass;
- peak memory and runtime meet agreed limits for the qualification model;
- two consecutive clean runs produce identical semantic output and numbering.

## 18. Test strategy

### 18.1 Unit tests

- string interning, metadata origin/review states, and UTF-8 serialization;
- digest separation and stable ID construction;
- transform-invariant definition matching and mirror rejection/acceptance;
- port matching, cap decisions, fill-plane calculations, and owner ranking;
- SQLite migrations, dependency invalidation, and artifact digest checks;
- canonical deterministic ordering independent of worker completion.

### 18.2 Geometry property tests

- generated medium lies inside the intended lumen;
- medium intersection with host walls and other material is zero;
- reconstructed definition union equals source within exact tolerances;
- overlap repair is removal-only from non-owners;
- unification does not change occupied volume;
- partial liquid plus gas equals lumen and is mutually disjoint.

Use adversarial cases: tiny gaps, nearly coincident ports, reversed faces,
non-manifold shells, sliver caps, branched networks, blind channels, open pipes,
and tolerance-scale wall thickness.

### 18.3 Contract tests

- csgify package accepted by supported libalea versions;
- unknown required feature rejected clearly;
- metadata and stable IDs survive conversion;
- cells map back to their material/source/derived region;
- coverage diagnostics map back to spatial tiles and source entities.

### 18.4 Failure-injection tests

- kill a worker during Boolean construction;
- exceed memory, time, piece, and temporary-disk limits;
- corrupt or truncate an artifact;
- change policy or metadata during resume;
- simulate SQLite interruption before/after artifact publication;
- return unknown OCCT intersection status;
- leave a medium port unresolved or introduce an exterior leak.

### 18.5 Performance tests

Track against model size:

- import peak RSS and time;
- unique definitions versus occurrences;
- cache hit ratio and avoided Boolean calls;
- exact overlap tests per spatial neighbour;
- conversion time/RSS per strategy;
- artifact/database size;
- libalea surface/cell/Boolean-node counts;
- void-generation time/RSS per tile;
- deck-size reduction from universes/transforms;
- cold run, warm cache, and interrupted-resume duration.

## 19. Observability and diagnostics

Every job emits structured events:

```text
job_id, target_id, stage, strategy, attempt, state, elapsed_ms,
rss_current, rss_peak, input_digest, output_digest, counters,
diagnostic_code, fallback_reason
```

Use stable diagnostic families:

- `ASSEMBLY_*`, `IDENTITY_*`, `CACHE_*`, `WORKER_*`;
- `RECOGNITION_*`, `DECOMPOSITION_*`, `UNIFICATION_*`;
- `MEDIUM_*`, `PORT_*`, `LEAK_*`, `FILL_*`;
- `OVERLAP_*`, `PROTECTED_*`, `APPROXIMATION_*`;
- `ALEA_SCHEMA_*`, `VOID_*`, `COVERAGE_*`, `EMIT_*`.

Each blocking diagnostic includes the target, evidence, policy clause, safest
next action, and whether it can be automated under the current policy.

## 20. Migration and compatibility

- add new data alongside the current `ShapeMetadata`, `AssemblyNode`, and
  integer material fields, then migrate call sites incrementally;
- retain current single-shot commands by creating an ephemeral project store;
- preserve existing JSON fields and add schema/version fields before removing
  legacy counters;
- import existing decision logs and compact recipes into the project store;
- allow old recipes to replay with exact defaults and no medium generation;
- require explicit migration for a recipe that used legacy overlap waivers;
- keep final output compatible with existing MCNP workflows.

## 21. Recommended repository layout

Proposed csgify modules:

```text
include/csgify/assembly.hpp
include/csgify/metadata.hpp
include/csgify/material.hpp
include/csgify/project_store.hpp
include/csgify/medium.hpp
include/csgify/alea_contract.hpp
src/assembly/
src/metadata/
src/project/
src/workers/
src/spatial/
src/recognition/
src/medium/
src/alea_bridge/
src/viewer/
tests/assembly/
tests/project/
tests/recognition/
tests/medium/
tests/alea_contract/
```

Do not move all existing code at once. New modules should wrap current engine
operations and gradually retire large cross-cutting structures after tests
cover the new boundaries.

## 22. Key risks and mitigations

| Risk | Mitigation |
|---|---|
| Geometry digest false match | Strong digest plus exact equivalence check before reuse |
| OCCT Boolean memory spike | Process limits, estimates, smaller partitions, recognition-first paths |
| Cache invalidation bugs | Explicit dependency DAG and split digests |
| Huge void expressions | Spatial libalea generation, canonical surfaces, bounded expressions, universes |
| Incorrect lumen closure | Explicit port decisions, leak test, seed containment, blocking certificate |
| Semantically wrong LLM decision | Immutable user policy, provenance, proposal evidence, replay accepted action |
| Metadata bloats memory/deck | String interning, references, concise MCNP comments, full external report |
| Partial fill ambiguity | Explicit gravity/level/pocket policy and protected phase interface |
| Parallel nondeterminism | Stable-ID commit/numbering order independent of completion |
| Cross-repository schema drift | Versioned contract and shared compatibility tests |
| Safety regression from approximation | Independent hard overlap and coverage gates |

## 23. First executable slice

The first slice should prove the architecture without waiting for every
recognizer or libalea enhancement:

1. implement interned metadata, materials, definitions, and instances;
2. migrate STEP import and report output to populate them;
3. add a minimal SQLite project/artifact store;
4. cache one certified conversion per unique toy-car definition;
5. implement material-aware overlap owner ranking on the existing repair path;
6. implement full-fill medium generation for one straight pipe with explicit
   caps and a derived-region certificate;
7. serialize a minimal versioned csgify/libalea package;
8. display the metadata, overlap owner, lumen, ports, and certificate in Qt;
9. replay the operation from a compact recipe;
10. verify final STEP and MCNP output through the existing safety gates.

This slice exercises identity, persistence, semantics, one derived material,
review, autonomy boundaries, handoff, and reproducibility before scaling to
distributed geometry work.

## 24. Definition of complete implementation

The plan is complete only when a representative ITER port model can be loaded,
interrupted, resumed, reviewed, and emitted such that:

- repeated definitions are processed once and instantiated deterministically;
- all mechanical and requested medium materials have stable semantic identity;
- every automatic removal, approximation, ownership, and generated region is
  reported with provenance and measurements;
- libalea creates and optimizes all remaining void regions;
- the modeled-domain certificate proves exactly-one membership everywhere at
  the supported certificate strength;
- the compact recipe reproduces the accepted model without repeating human or
  LLM deliberation;
- the final compacted STEP and MCNP deck are produced by default;
- the run stays within configured memory, time, and disk limits and safely
  resumes after a worker failure.

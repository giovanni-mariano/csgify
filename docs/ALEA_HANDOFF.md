# Versioned csgify-to-libalea handoff

The Phase 7 bridge is owned by csgify and uses only libalea's opaque public C
API. libalea does not parse STEP, csgify manifests, project databases, or OCCT
objects.

## Contract

`csgify.alea-handoff` version 1.0 contains:

- producer, libalea, source, policy, and contract digests;
- negotiated public capabilities;
- final Alea cell, surface, universe, fill, transform, material, and expression
  identities;
- resolved UTF-8 semantic sidecar entries for definitions, occurrences,
  geometry, materials, names, descriptions, roles, tags, comments, annotations,
  and occurrence transforms;
- content-addressed chunk references for large geometry and later validation
  evidence;
- structured diagnostics.

The manifest digest is computed over the deterministic serialization with the
digest field blank. Mutation, duplicate IDs, missing cell/universe references,
incomplete chunks, and unsupported schema versions fail validation.

Every emitted Alea cell receives a compact `csgify:id=...` inline comment.
libalea preserves it through simplification, cloning, and union-cell splitting,
so the final sidecar is derived from final cell numbering rather than assuming
that pre-simplification storage indices remain stable.

## Repeated definitions

`register_alea_instance_transform` converts csgify's row-major affine 3x4
matrix to libalea's public auxiliary-to-main transform convention.
`add_alea_fill_instance` creates a tagged fill container referencing an existing
definition universe. libalea remains responsible for hierarchy queries,
universe indexing, simplification, voids, and MCNP export.

The only libalea change required by Phase 7 is exposing its already-implemented
generic `alea_add_transform` and `alea_add_inline_transform` functions in
`alea.h`.

## CLI and project artifacts

For an already simplified, exact STEP file:

```bash
csgify handoff simplified.step \
  --out handoff.json \
  --report handoff-report.json \
  --project project-directory
```

With `--project`, csgify exports the in-memory Alea geometry as an immutable
MCNP geometry chunk, publishes it in the project artifact store, adds its
digest reference to the manifest, then publishes and indexes the manifest.
Large geometry bytes are never copied into the JSON manifest.

The ordinary conversion report includes an `alea_handoff` summary containing
compatibility, schema/provider versions, final counts, digest, chunk count, and
missing capabilities.

## Safety boundary

The bridge does not weaken readiness gates. Non-convex residuals, unsupported
surfaces, positive-volume overlaps, unknown overlap results, incomplete domain
coverage, or an invalid handoff still block successful production conversion.
Phase 8 consumes this contract to schedule and aggregate libalea void and
coverage work.

The implemented adapter and certificate semantics are documented in
[`ALEA_VOID_AND_COVERAGE.md`](ALEA_VOID_AND_COVERAGE.md). The `csgify voids`
command consumes the in-process handoff, invokes libalea's public void APIs,
and persists resumable geometry/certificate artifacts.

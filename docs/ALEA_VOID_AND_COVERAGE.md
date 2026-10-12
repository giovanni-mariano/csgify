# Alea void generation and coverage certificates

Phase 8 keeps general void construction in libalea. csgify supplies a finite
modeled domain, resource policy, source-overlap evidence, stable identities,
project scheduling, and artifact persistence. It does not implement a second
void geometry algorithm.

## Command

Run this on an exact, already simplified/decomposed STEP model:

```sh
csgify voids final.step \
  --out coverage.json \
  --out-mcnp geometry-with-voids.i \
  --project conversion.csgify \
  --workers 8 \
  --scratch-bytes 4294967296
```

The default modeled domain is the root geometry bounding box expanded by five
percent of its diagonal. This is useful for deterministic conversion tests.
For production transport work, declare the physical domain explicitly:

```sh
csgify voids final.step --out coverage.json \
  --domain-id domain:port-cell \
  --bounds "-6000,-4000,-3000,6000,4000,3000"
```

`--scratch-bytes` bounds libalea's parallel classification scratch allocation;
it is not a whole-process RSS limit. Zero deliberately forces serial void
classification. `--samples N` controls a deterministic `N x N x N`
complete-ownership lattice. Elapsed time is not limited by this command.

## Safety meaning

Each generated void cell is an exact analytic expression:

```text
modeled-domain region & complement(all occupied cells relevant to that region)
```

libalea owns adaptive region construction, local complements, deterministic
merge, transactional commit/rollback, hierarchy queries, and graveyard
construction. csgify records this structural invariant separately from the
coverage lattice. The lattice uses libalea's complete occurrence-chain query;
it is diagnostic evidence and is not mislabeled as an exhaustive continuum
proof of source-cell disjointness.

The certificate blocks success for any of:

- source CAD overlap evidence;
- multiple Alea owners at a sampled point;
- an unowned point inside the modeled domain;
- an undefined universe fill;
- a truncated or unresolved ownership query.

Small volume approximation does not relax overlap ownership. The overlap must
be resolved before final MCNP assembly so each spatial point belongs to only
one component.

## Persistence and resume

With `--project`, csgify creates a deterministic `alea_void_generate_v1` job.
The final Alea/MCNP geometry and the versioned
`csgify.alea-coverage-certificate` are immutable, content-addressed artifacts.
The conversion cache key includes the handoff contract, domain, and complete
void policy. Repeating the same command validates and reuses those artifacts
without repeating void generation. A policy or geometry change produces a new
job and cache entry.

The certificate records:

- domain, policy, handoff, and certificate digests;
- material, intentional-void, graveyard, fill, and reused-universe counts;
- an estimate of duplicated definition cells avoided through hierarchy reuse;
- worker and scratch-memory execution facts;
- source overlap and exact-query counts;
- gap, overlap, undefined-fill, unresolved, and truncation evidence;
- stable void/graveyard cell IDs and domain-scoped diagnostics;
- explicit limitations of the evidence.

## Boundary with later work

Phase 8 produces certified geometry and evidence. Phase 9 consumes those
artifacts for project-scale review and autonomous decision handling. Final
transport-deck policy—materials, importances, source/tally cards, and reviewer
approval—remains a later assembly concern.

The first Phase 9 integration is available through `csgify coverage status`
and `csgify coverage review`. Both parse and validate the typed certificate;
the Qt path adds searchable blocker and evidence panels without exposing a
button that can authorize unsafe geometry.

`csgify coverage queue` maps blocking findings into the standard persistent
autonomy queue. A later valid certificate can reconcile the queue and mark only
findings that actually disappeared as `verified resolved`.

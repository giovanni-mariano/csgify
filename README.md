# cad2csg

`cad2csg` converts STEP CAD models into Alea CSG systems for neutronics workflows.

This first engine pass targets Open CASCADE Technology 8.0.0 APIs and links to
`../libalea`. CMake requires `OpenCASCADE 8.0.0 EXACT`, so older system OCCT
packages are rejected.

Current scope:

- import STEP through OCCT
- optionally run OCCT shape healing
- traverse solids and faces
- convert plane, sphere, and cylinder faces to Alea halfspaces
- represent arbitrary cylinder axes as Alea general quadrics
- build one Alea cell per converted solid
- export the resulting system to MCNP through libalea
- emit structured diagnostics for unsupported spline and other analytic faces

Project style is described in [docs/PHILOSOPHY.md](docs/PHILOSOPHY.md): small
core, explicit data flow, command-line first, and useful failure reports.
The broader implementation plan is in
[docs/IMPLEMENTATION_PLAN.md](docs/IMPLEMENTATION_PLAN.md).
The non-convex path is tracked in [docs/DECOMPOSITION.md](docs/DECOMPOSITION.md).

## OCCT 8 setup

The `/home/giovanni/OCCT-8_0_0` directory is the OCCT source tree. Build and
install it into a separate prefix first:

```sh
cmake -S /home/giovanni/OCCT-8_0_0 \
  -B /home/giovanni/OCCT-8_0_0-build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/home/giovanni/OCCT-8_0_0-install \
  -DINSTALL_DIR=/home/giovanni/OCCT-8_0_0-install \
  -DBUILD_MODULE_Draw=OFF \
  -DBUILD_MODULE_Visualization=OFF

cmake --build /home/giovanni/OCCT-8_0_0-build --parallel 2
cmake --install /home/giovanni/OCCT-8_0_0-build
```

`cad2csg` requires `OpenCASCADE 8.0.0 EXACT`, so CMake will reject the system
OCCT 7.8.1 package.

Build `cad2csg`:

```sh
cmake --preset occt8
cmake --build --preset occt8
```

Run:

```sh
./build/cad2csg input.step output.i
```

Inspect a model before conversion:

```sh
./build/cad2csg inspect input.step --report report.json
```

`inspect` writes a machine-readable report with summary counters, diagnostics,
and allowed actions for human or agent preprocessing. It does not create Alea
cells or export neutronics input.

The report includes stable traversal IDs for decisions:

- solids: `solid:1`, `solid:2`, ...
- faces: `face:1.4` means solid 1, face 4

Diagnostics include `shape_id`, and the report contains `solids[]` and `faces[]`
sections with volume, area, bounding box, surface type, and convertibility flags.
`solids[]` also includes semantic-cleanup metrics such as `volume_fraction`,
`bbox_diagonal_fraction`, bbox extents/aspect ratio, and `cleanup_candidates`
flags for small-volume, small-bbox, thin-plate, and long-body solids.
It also reports `replacement_candidates` for currently supported simple
replacements, including bbox, conservative bbox-cylinder, fitted cylinder,
fitted sphere, and fitted cone volume metrics. These are decision metrics, not
automatic approval.

Calculate volume, or compare original and simplified models:

```sh
./build/cad2csg volume input.step
./build/cad2csg volume original.step --compare simplified.step --rel-tol 1e-4
./build/cad2csg volume original.step --compare simplified.step --abs-tol 0.01
```

The comparison command exits with success only when the absolute and relative
tolerances that were provided are both satisfied. If no tolerance is provided,
the command reports the delta and requires exact equality.

Run a first in-memory simplification and export:

```sh
./build/cad2csg simplify input.step --replace-solid-bbox solid:1 --out simplified.i
./build/cad2csg simplify input.step --replace-solid-bbox-cylinder solid:1 --out simplified.i
./build/cad2csg simplify input.step --replace-solid-fitted-cylinder solid:1 --out simplified.i
./build/cad2csg simplify input.step --replace-solid-fitted-sphere solid:1 --out simplified.i
./build/cad2csg simplify input.step --replace-solid-fitted-cone solid:1 --out simplified.i
./build/cad2csg simplify input.step --decompose-planar solid:1 --out simplified.i
./build/cad2csg simplify input.step --remove-solid solid:1 --out simplified.i
```

`--replace-solid-bbox` suppresses the selected CAD solid and adds a synthetic
axis-aligned box from its bounding box. Synthetic bodies are emitted directly to
Alea; no decision file is involved yet.
`--replace-solid-bbox-cylinder` suppresses the selected CAD solid and adds a
conservative axis-aligned cylinder: the longest bbox axis becomes the cylinder
height and the larger transverse bbox extent sets the radius. It reports the
original/replacement volume delta.
`--replace-solid-fitted-cylinder`, `--replace-solid-fitted-sphere`, and
`--replace-solid-fitted-cone` use the dominant matching OCCT analytic face as
the primitive source and report the volume delta before export. Fitted cones are
exported as an explicit CSG intersection of a cone side and two cap planes.
Axis-aligned cones use libalea cone surfaces, producing MCNP `KX`, `KY`, `KZ`
or translated `K/X`, `K/Y`, `K/Z` cards. Arbitrary-axis cones fall back to a
general quadric `GQ` side. Neither path requires libalea's TRC macrobody
exporter.

Replacement commands also accept volume guards:

```sh
./build/cad2csg simplify input.step --replace-solid-fitted-cylinder solid:1 \
  --out simplified.i --max-rel-delta 0.01
./build/cad2csg simplify input.step --replace-solid-bbox solid:1 \
  --out simplified.i --max-abs-delta 0.001
./build/cad2csg simplify input.step --replace-solid-fitted-cylinder solid:1 \
  --out simplified.i --report simplified-report.json
```

When a guard is provided, the command checks the replacement candidate metrics
from the analysis report before modifying the model. `--report` writes the
post-preprocessing conversion report for agent tooling.

The same guard logic is available through the C++ API:
`replacement_metrics_for_operation(...)` reads metrics from an analyzed solid,
and `check_replacement_limits(...)` validates a proposed operation against
`ReplacementLimits`. `apply_replacement_operation(...)` applies an explicit
replacement, while `apply_checked_replacement_operation(...)` validates limits
and mutates the model only if the check passes. This keeps future Lua or agent
bindings on the same decision path as the CLI.

Phase 2 has started with explicit planar decomposition:
`--decompose-planar` splits a selected solid using its planar faces, suppresses
the original solid, and converts the resulting pieces. The command writes
volume-preservation diagnostics and a structured `decomposed_pieces[]` report
section with per-piece volume, bbox, face count, and convertibility flags. This
first decomposition pass is intentionally limited: pieces may still fail
conversion if they remain non-convex or contain unsupported surfaces.
Axis-aligned torus faces are supported through libalea `TX`, `TY`, and `TZ`
surfaces; arbitrary-axis tori still require replacement or a future
transform-aware emitter.

The engine deliberately stops on spline faces for now. That is the right place
to add the agent/user loop for simplification choices: remove, replace with an
analytic surrogate, approximate under tolerance, or keep for manual handling.

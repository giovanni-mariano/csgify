# Session and Workspace API Plan

This document describes how `csgify` should evolve from the current native
C++ engine API into a future user/agent/scripting API without exposing OCCT
state. The short version is:

- Keep OCCT inside the engine.
- Expose sessions, documents, stable IDs, commands, reports, and files.
- Let one session manage more than one STEP file.
- Do not turn the project into a framework.

The plan is intentionally conservative. The current engine is still being
shaped around OCCT 8, spline repair, decomposition, and Alea emission. The
outer API should hide OCCT from users and agents, but the internal engine can
continue to use OCCT directly where that keeps the code clearer.

## Problem

The current public header exposes engine state such as `TopoDS_Shape` and
`TopoDS_Solid` through `CadModel` and decomposition records. This is practical
for current C++ implementation work, but it is not the right contract for:

- agentic tools such as Codex, Claude, or Pi
- a future Lua/Python binding
- a stable command vocabulary
- multi-STEP workflows
- long-lived preprocessing sessions

External tools should not need to know OCCT object lifetimes, OCCT shape
identity, or OCCT topology classes. They should operate on stable shape IDs and
plain data reports.

## Design Principle

Use two API layers.

1. Internal engine API

   This is the implementation boundary. It may use OCCT types directly. It can
   keep `CadModel`, `TopoDS_Shape`, `TopoDS_Solid`, `FaceReplacement`,
   decomposition pieces, synthetic bodies, and Alea handles where they are
   needed.

2. External workspace API

   This is the user, CLI, agent, and future scripting boundary. It must not
   expose OCCT. It should expose:

   - opaque session/document handles
   - stable IDs such as `doc:1/solid:4` and `doc:1/face:4.2`
   - plain reports
   - explicit commands
   - validation results
   - file import/export

The internal API can change while the engine is young. The external API should
be smaller, slower to change, and suitable for automation.

## Antirez-Style Constraint

The session API is acceptable only if it remains plain and useful. It should be
a real owner of workflow state, not a general platform.

Good shape:

```cpp
struct CsgifySession;
struct CsgifyDocument;

CsgifySession* csgify_session_create();
void csgify_session_destroy(CsgifySession* session);

CsgifyDocumentId csgify_load_step(CsgifySession* session,
                                    const char* path);

bool csgify_analyze(CsgifySession* session,
                     CsgifyDocumentId document);

bool csgify_remove_solid(CsgifySession* session,
                          const char* shape_id);
```

Bad shape:

```cpp
class AbstractCadProvider;
class ShapeRepository;
class CommandBus;
class GeometryService;
class ReportPublisher;
```

The rule is: structs, functions, IDs, reports. No deep class hierarchy, no
framework, no hidden control flow.

## Core Concepts

### Session

A session is a csgify workspace. It owns loaded documents, default options,
the command history, and the latest reports.

It is not limited to one STEP file.

```cpp
struct CsgifySession {
    std::vector<CadDocument> documents;
    ConversionOptions options;
    ConversionReport last_report;
    std::vector<CommandRecord> command_history;
};
```

This struct is illustrative. In the external API it should be opaque.

### Document

A document is one imported CAD file and its mutable preprocessing state.

```cpp
struct CadDocument {
    std::string id;          // "doc:1"
    std::string source_path;
    CadModel model;          // internal, OCCT-backed
    ConversionReport report;
};
```

The current `CadModel` should become an internal document payload. It remains
free to use OCCT directly.

### Shape IDs

Current IDs are single-document:

```text
solid:1
face:1.4
piece:2
synthetic:3
```

Future external IDs should be document-scoped:

```text
doc:1/solid:1
doc:1/face:1.4
doc:2/solid:1
doc:2/piece:3
doc:2/synthetic:5
```

The session layer can accept both forms while only one document is loaded. Once
more than one document exists, unscoped IDs should be rejected as ambiguous.

### Commands

Commands are the external control plane. A command should be explicit,
serializable, and replayable.

Examples:

```text
load_step path=model.step
analyze doc:1
remove_solid doc:1/solid:12
replace_solid_with_bbox doc:1/solid:12 max_relative_volume_delta=0.02
replace_face_with_cylinder doc:1/face:4.2 max_deviation=1e-6
decompose_planar doc:1/solid:7
add_box doc:1 min=[0,0,0] max=[1,1,1] material=1 density=1.0
emit_document doc:1 out=model.i
emit_assembly docs=[doc:1,doc:2] out=assembly.i
```

The first implementation can expose these as C++ functions. Later the same
vocabulary can be used by CLI batch files, Lua, JSONL logs, or Python.

### Reports

Reports are the observation channel. They should be plain data and should not
contain OCCT objects.

A report item should always include:

- `document_id`
- stable `shape_id` when applicable
- human-readable `shape_label`
- severity
- code
- message
- allowed actions
- relevant metrics

Diagnostics should not require label parsing to recover stable IDs. Labels are
for humans. IDs are for machines.

## Proposed Native API Shape

The first API can be C++ and still opaque enough for later bindings.

```cpp
namespace csgify {

struct Session;

using DocumentId = std::string;

struct CommandResult {
    bool ok = false;
    ConversionReport report;
};

std::unique_ptr<Session> create_session();

DocumentId load_step(Session& session,
                     const std::string& path,
                     const ConversionOptions& options = {});

CommandResult analyze(Session& session, const DocumentId& document_id);

CommandResult remove_solid(Session& session,
                           const std::string& scoped_shape_id);

CommandResult replace_solid_with_bbox(Session& session,
                                      const std::string& scoped_shape_id);

CommandResult replace_solid_with_fitted_cylinder(
    Session& session,
    const std::string& scoped_shape_id);

CommandResult replace_face_with_fit(Session& session,
                                    const std::string& scoped_shape_id,
                                    SplineFitKind kind,
                                    double max_deviation);

CommandResult auto_fit_splines(Session& session,
                               const DocumentId& document_id,
                               double max_deviation);

CommandResult decompose_planar(Session& session,
                               const std::string& scoped_shape_id);

CommandResult emit_document(Session& session,
                            const DocumentId& document_id,
                            const std::string& output_path);

CommandResult emit_assembly(Session& session,
                            const std::vector<DocumentId>& document_ids,
                            const std::string& output_path);

} // namespace csgify
```

Important details:

- `Session` is opaque in the public header.
- `CadModel` and OCCT includes move out of the external API over time.
- Existing engine functions can remain available internally.
- `CommandResult` returns a report after every mutation.
- The CLI should eventually call this API instead of manually loading and
  mutating `CadModel` for every subcommand.

## Proposed C ABI Shape

Lua, Python, and other bindings are easiest if the stable boundary can also be
expressed as a small C ABI.

This should come after the C++ session API is working.

```c
typedef struct csgify_session csgify_session_t;

csgify_session_t* csgify_session_create(void);
void csgify_session_destroy(csgify_session_t* session);

int csgify_load_step(csgify_session_t* session,
                      const char* path,
                      char* document_id_out,
                      size_t document_id_capacity);

int csgify_analyze(csgify_session_t* session,
                    const char* document_id);

int csgify_remove_solid(csgify_session_t* session,
                         const char* scoped_shape_id);

int csgify_replace_face_with_fit(csgify_session_t* session,
                                  const char* scoped_face_id,
                                  const char* kind,
                                  double max_deviation);

int csgify_emit_document(csgify_session_t* session,
                          const char* document_id,
                          const char* output_path);

const char* csgify_last_report_json(csgify_session_t* session);
const char* csgify_last_error(csgify_session_t* session);
```

Keep ownership simple:

- The session owns returned strings.
- Returned pointers remain valid until the next API call on that session.
- The caller copies strings if it needs to keep them.

## Multi-Document Semantics

### Loading

Every successful `load_step` creates one document:

```text
doc:1
doc:2
doc:3
```

Document IDs are assigned by the session, not by file name.

### Targeting Shapes

Commands that target shapes should use scoped IDs:

```text
doc:1/solid:4
doc:2/face:1.3
```

The session layer parses the document prefix, finds the document, strips the
prefix, and calls the existing internal engine operation with the local ID:

```text
external: doc:2/face:1.3
internal: face:1.3 on CadDocument doc:2
```

This keeps most current code unchanged.

### Reports

Reports should include both document and shape identity.

```json
{
  "document_id": "doc:2",
  "shape_id": "doc:2/face:1.3",
  "local_shape_id": "face:1.3",
  "shape_label": "solid[1]/face[3]",
  "code": "unsupported_spline_face",
  "severity": "error"
}
```

For a single-document report, the scoped ID is still useful because it makes
the report stable when copied into a larger session log.

### Assembly Emission

Assembly emission is a later feature. The first version can reject it.

Eventually, `emit_assembly` should combine already-ready documents into one
Alea system. It must define how document coordinate systems, materials, and
overlaps are handled. Until those rules exist, document-level emission is the
safe default.

## Migration Plan

### Phase A: Name the Boundary

Goal: make the codebase vocabulary honest without changing behavior.

Tasks:

- Document that `CadModel` is an internal engine model.
- Add comments in `engine.hpp` warning that OCCT-backed structs are not the
  future scripting boundary.
- Keep current CLI behavior unchanged.
- Keep current tests unchanged.

Exit criteria:

- Developers understand the split between internal engine API and future
  external API.
- No behavior changes.

### Phase B: Add a C++ Session Wrapper

Goal: introduce one owner for workflow state.

Tasks:

- Add `src/session.hpp` and `src/session.cpp`.
- Define opaque or semi-opaque `csgify::Session`.
- Store a vector of documents inside the session.
- Implement:
  - `create_session`
  - `load_step`
  - `analyze`
  - `remove_solid`
  - `replace_face_with_fit`
  - `auto_fit_splines`
  - `emit_document`
- Use existing engine functions internally.
- Do not move OCCT types yet unless the move is easy.

Exit criteria:

- A unit or smoke test can load one STEP file through `Session`, analyze it,
  apply one operation, and emit.
- Current CLI still works.

### Phase C: Introduce Document-Scoped IDs

Goal: make the session safe for more than one STEP file.

Tasks:

- Add ID helpers:
  - `make_document_id(index)`
  - `parse_document_id`
  - `scope_shape_id(document_id, local_shape_id)`
  - `parse_scoped_shape_id`
- Report both local and scoped IDs.
- Allow unscoped IDs only when exactly one document is loaded.
- Add tests for ambiguous unscoped IDs.

Exit criteria:

- One session can load two fixtures.
- Commands can target `doc:1/solid:1` and `doc:2/solid:1` without ambiguity.
- Reports from both documents have scoped IDs.

### Phase D: Move CLI Onto Session API

Goal: make CLI and future scripts share the same command path.

Tasks:

- Refactor `inspect`, `simplify`, `auto-fit`, `volume`, and default convert
  paths to create a session and call session commands.
- Preserve existing CLI syntax for one-document use.
- Add optional scoped-ID support without breaking current examples.
- Ensure every CLI mutation returns a post-mutation report.

Exit criteria:

- Existing smoke and contract tests pass.
- No command has a separate behavior path that bypasses session mutation.

### Phase E: Hide OCCT From External Header

Goal: make the installed/public API usable without OCCT shape types.

Tasks:

- Split headers into:
  - external public API, no OCCT includes
  - internal engine API, OCCT-backed
- Move `CadModel`, `DecomposedSolidPiece`, and any `TopoDS_*` members to an
  internal header.
- Keep report structs, options, enums, and simple geometry structs public.

Exit criteria:

- A user can include the external API header without including OCCT headers.
- CLI and tests build through the new boundary.

### Phase F: Add C ABI

Goal: prepare for Lua/Python/agent integrations.

Tasks:

- Add `include/csgify/c_api.h`.
- Wrap the C++ session API.
- Return reports as JSON strings owned by the session.
- Add simple examples:
  - load and inspect
  - replace a face
  - emit document

Exit criteria:

- A plain C test can load a STEP file, run analyze, and read report JSON.
- No OCCT symbols appear in the C header.

### Phase G: Add Script Binding

Goal: support interactive preprocessing workflows.

Candidate first binding: Lua.

Tasks:

- Bind only the C ABI or the narrow C++ session API.
- Expose commands, reports, and IDs.
- Do not expose OCCT.
- Keep scripts optional; CLI remains first-class.

Exit criteria:

- A Lua script can load a STEP, inspect diagnostics, remove or replace shapes,
  run volume checks, and emit.

## Testing Plan

Add tests incrementally with each phase.

Required tests:

- Single-document session load/analyze/emit.
- Single-document compatibility with unscoped IDs.
- Two-document session load.
- Scoped shape command targets correct document.
- Unscoped shape command fails when multiple documents are loaded.
- Reports contain:
  - `document_id`
  - scoped `shape_id`
  - local shape ID if useful
  - human label
- CLI still accepts current one-document commands.
- C ABI can load, analyze, and return JSON after Phase F.

Contract tests should stay close to `tests/contract_smoke.sh` because this is
the API agents will depend on.

## Report Format Additions

The current JSON report should evolve carefully. Suggested additions:

```json
{
  "session": {
    "document_count": 2
  },
  "document": {
    "id": "doc:1",
    "source_path": "input.step"
  },
  "diagnostics": [
    {
      "document_id": "doc:1",
      "shape_id": "doc:1/face:1.4",
      "local_shape_id": "face:1.4",
      "shape_label": "solid[1]/face[4]",
      "severity": "error",
      "code": "unsupported_spline_face",
      "actions": ["replace_with_cylinder", "replace_with_plane", "manual"]
    }
  ]
}
```

Do not remove existing fields immediately. Add fields first, update tests, then
deprecate only when there is a reason.

## Command History

A session should eventually keep a lightweight command history.

Purpose:

- make preprocessing reproducible
- let agents explain what they changed
- allow save/replay later without making JSON the internal control plane

Suggested record:

```cpp
struct CommandRecord {
    std::string command;
    std::string target_id;
    std::map<std::string, std::string> parameters;
    bool ok = false;
    std::string diagnostic_code;
};
```

This can later be exported as JSONL:

```json
{"command":"remove_solid","target":"doc:1/solid:12","ok":true}
{"command":"replace_face_with_fit","target":"doc:1/face:2.5","kind":"cylinder","max_deviation":1e-6,"ok":true}
```

The internal command history should remain optional until it is needed by a
real workflow.

## What Not To Do Yet

- Do not rewrite the current engine around immutable pipeline types.
- Do not hide every OCCT type immediately if that slows Phase 2 geometry work.
- Do not invent a plugin architecture.
- Do not make JSON command files mandatory.
- Do not add Lua before the C++ session API is stable.
- Do not implement assembly emission before material, transform, and overlap
  rules are explicit.

## First Concrete Step

The first implementation step should be small:

1. Add `src/session.hpp` and `src/session.cpp`.
2. Define `Session` and `CadDocument`.
3. Implement single-document `load_step`, `analyze`, and `emit_document`.
4. Add one smoke test that uses the session API directly.
5. Keep CLI unchanged.

After that works, move one CLI command at a time onto the session API.

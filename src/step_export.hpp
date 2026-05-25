#pragma once

// STEP exporter for the as-simplified geometry.
//
// Writes a STEP file that contains:
//   - every solid from CadModel.root that was NOT removed
//   - every TopoDS_Solid in CadModel.decomposed_pieces
//   - every primitive synthetic body (box/sphere/cylinder/cone)
//     materialized via BRepPrimAPI_*
//
// Pass 1 deliberately skips boolean synthetic nodes (union/intersection/
// difference) — those need recursive BRepAlgoAPI_* builds and add real
// failure modes that should not block the verification workflow. They
// are emitted as bbox placeholders in pass 1 with a stderr warning.

#include <string>

namespace cad2csg {

struct CadModel;

// Returns 0 on success, non-zero on write failure. Diagnostics are
// written to stderr.
int export_step(const CadModel& model, const std::string& output_path);

} // namespace cad2csg

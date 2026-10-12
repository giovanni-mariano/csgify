#pragma once

// STEP exporter for the as-simplified geometry.
//
// Writes a STEP file that contains:
//   - every solid from CadModel.root that was NOT removed
//   - every TopoDS_Solid in CadModel.decomposed_pieces
//   - every primitive synthetic body (box/sphere/cylinder/cone)
//     materialized via BRepPrimAPI_*
//
// Boolean synthetic nodes are recursively materialized with OCCT Boolean
// operations. Export fails if an active synthetic cannot be materialized;
// it never substitutes a bounding-box placeholder.

#include <cstddef>
#include <functional>
#include <string>

namespace csgify {

struct CadModel;

// Called after each exportable body is materialized into the transient STEP
// document. Returning false cancels before the output writer is invoked.
using StepExportProgressCallback =
    std::function<bool(std::size_t completed, std::size_t total,
                       const std::string& target_id)>;

enum class StepExportStatus {
    success = 0,
    failed = 1,
    cancelled = 2,
};

// Returns 0 on success, non-zero on write failure. Diagnostics are
// written to stderr.
int export_step(const CadModel& model, const std::string& output_path,
                StepExportProgressCallback progress = {});

} // namespace csgify

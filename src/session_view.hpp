#pragma once

// Internal bridge between the session layer and the AIS viewer.
// Declares SolidViewEntry, SolidViewStatus, and collect_view_entries.
// Not installed as part of the public API.

#include "cad2csg/session.hpp"

#include <TopoDS_Shape.hxx>
#include <string>
#include <vector>

namespace cad2csg {

enum class SolidViewStatus {
    exact_csg,            // ready for CSG emission — displayed green
    needs_decomposition,  // nonconvex, split required — displayed yellow
    no_interior_point,    // interior_point_not_found — displayed red
    piece,                // decomposed piece — displayed blue
};

struct SolidViewEntry {
    std::string  shape_id;
    std::string  label;
    std::string  name;            // from STEP metadata, may be empty
    TopoDS_Shape shape;
    SolidViewStatus status = SolidViewStatus::needs_decomposition;
    std::string  source_solid_id; // non-empty for decomposed pieces
};

// Collect all displayable solids and pieces from the first document in the
// session. Removed solids are excluded. Status coloring uses the last
// analysis report when available; defaults to needs_decomposition otherwise.
std::vector<SolidViewEntry> collect_view_entries(const Session& session);

} // namespace cad2csg

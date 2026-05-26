#pragma once

// Planar decomposition. Splits a solid along its own planar faces using
// OCCT's BOPAlgo_Splitter. Used both as an automatic fallback by the emit
// stage and as an explicit user-driven cleanup command.

#include "cad2csg/engine.hpp"

#include <vector>

#include <TopoDS_Face.hxx>
#include <TopoDS_Solid.hxx>
#include <gp_Pln.hxx>

namespace cad2csg::detail {

double solid_box_span(const TopoDS_Solid& solid);

std::vector<TopoDS_Solid> split_solid_by_planar_faces(const TopoDS_Solid& solid,
                                                      const ConversionOptions& options,
                                                      ConversionReport& report,
                                                      std::size_t solid_index);

// Cut a solid by the bounded cutter of EVERY one of its faces in a single
// splitter call: planes stay planes; cylinders/cones/spheres become their
// full quadric surface; tori become their axis plane. The result is the
// solid intersected with the full arrangement of its own surfaces, so each
// produced piece lies on one definite side of every surface. Cutters are
// deduplicated by surface signature so coincident faces do not multiply the
// tool list. Returns an empty vector (and a diagnostic) when no split is
// produced. This is the geometric core of `shatter_solid`.
std::vector<TopoDS_Solid> split_solid_by_all_surfaces(const TopoDS_Solid& solid,
                                                      const ConversionOptions& options,
                                                      ConversionReport& report,
                                                      std::size_t solid_index);

// Cut a solid by closure planes synthesized from its partial cylinder/cone
// faces (the axis/chord plane through each arc's angular-extreme generators).
// Used as a deep fallback in shatter_solid when neither a targeted
// failure-face cut nor the full surface arrangement subdivided a residual
// piece -- i.e. when a swept arc has no planar CAD face bounding it. Returns
// an empty vector when no partial curved face yields a plane.
std::vector<TopoDS_Solid> split_solid_by_closure_planes(const TopoDS_Solid& solid,
                                                        const ConversionOptions& options,
                                                        ConversionReport& report,
                                                        std::size_t solid_index);

std::vector<TopoDS_Solid> split_solid_by_face(const TopoDS_Solid& solid,
                                              const TopoDS_Face& face,
                                              const ConversionOptions& options,
                                              ConversionReport& report,
                                              std::size_t solid_index,
                                              std::size_t face_index);

std::vector<TopoDS_Solid> split_solid_by_plane(const TopoDS_Solid& solid,
                                               const gp_Pln& plane,
                                               const ConversionOptions& options,
                                               ConversionReport& report,
                                               std::size_t solid_index,
                                               const std::string& label);

// Multi-tool plane splitter. Hands every plane in `planes` to OCCT's
// splitter as a single tool list so the operation produces all pieces
// in one call. The single-plane variant above is a special case but
// kept separate for clarity in the dispatcher.
std::vector<TopoDS_Solid> split_solid_by_planes(const TopoDS_Solid& solid,
                                                const std::vector<gp_Pln>& planes,
                                                const ConversionOptions& options,
                                                ConversionReport& report,
                                                std::size_t solid_index,
                                                const std::string& label);

} // namespace cad2csg::detail

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

} // namespace cad2csg::detail

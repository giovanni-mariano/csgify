#pragma once

// Spline-surface analytic fit. Internal module: not part of the public API.
// Used by the analyze stage to attach plane/sphere/cylinder candidates to
// faces whose underlying OCCT surface is B-spline or Bezier.

#include "cad2csg/engine.hpp"

#include <TopoDS_Face.hxx>

namespace cad2csg::detail {

SplineFitCandidates fit_spline_face(const TopoDS_Face& face);

} // namespace cad2csg::detail

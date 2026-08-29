#pragma once

// Fitted-primitive candidates computed from a solid's analytic faces.
// The engine treats these as decision metrics: the agent or user chooses
// whether to apply `--replace-solid-fitted-*`.

#include "csgify/engine.hpp"

#include <TopoDS_Solid.hxx>

namespace csgify::detail {

FittedCylinderCandidate fitted_cylinder_candidate(const TopoDS_Solid& solid,
                                                  double original_volume);
FittedSphereCandidate fitted_sphere_candidate(const TopoDS_Solid& solid,
                                              double original_volume);
FittedConeCandidate fitted_cone_candidate(const TopoDS_Solid& solid,
                                          double original_volume);

} // namespace csgify::detail

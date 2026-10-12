#include "spline_fit.hpp"

#include <BRepBuilderAPI_MakeFace.hxx>
#include <Geom_BSplineSurface.hxx>
#include <NCollection_Array1.hxx>
#include <NCollection_Array2.hxx>

#include <cassert>
#include <cmath>

int main() {
    // The 13x13 sampler visits u = n/12. Put a narrow ridge strictly between
    // u=0 and u=1/12. This is a regression for sampled residual being
    // mislabeled as a certified global deviation bound.
    const double u_values[] = {0.0, 0.04, 0.05, 0.06, 1.0};
    NCollection_Array2<gp_Pnt> poles(1, 5, 1, 2);
    NCollection_Array1<double> u_knots(1, 5);
    NCollection_Array1<double> v_knots(1, 2);
    NCollection_Array1<int> u_multiplicities(1, 5);
    NCollection_Array1<int> v_multiplicities(1, 2);
    for (int i = 1; i <= 5; ++i) {
        u_knots(i) = u_values[i - 1];
        u_multiplicities(i) = (i == 1 || i == 5) ? 2 : 1;
        for (int j = 1; j <= 2; ++j) {
            poles(i, j) = gp_Pnt(
                u_values[i - 1], static_cast<double>(j - 1),
                i == 3 ? 1.0 : 0.0);
        }
    }
    v_knots(1) = 0.0;
    v_knots(2) = 1.0;
    v_multiplicities(1) = 2;
    v_multiplicities(2) = 2;

    Handle(Geom_BSplineSurface) surface = new Geom_BSplineSurface(
        poles, u_knots, v_knots, u_multiplicities, v_multiplicities, 1, 1);
    const TopoDS_Face face = BRepBuilderAPI_MakeFace(surface, 1.0e-7).Face();
    const csgify::SplineFitCandidates fits =
        csgify::detail::fit_spline_face(face);

    assert(fits.plane.available);
    assert(std::abs(fits.plane.max_deviation) < 1.0e-14);
    assert(std::abs(surface->Value(0.05, 0.5).Z() - 1.0) < 1.0e-14);
    assert(!fits.plane.deviation_certified);
    return 0;
}

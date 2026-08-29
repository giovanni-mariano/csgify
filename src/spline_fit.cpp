#include "spline_fit.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include <BRepAdaptor_Surface.hxx>
#include <gp_Pnt.hxx>

namespace csgify::detail {
namespace {

constexpr double kEps = 1.0e-14;

struct SplineSample {
    gp_Pnt p;
};

std::vector<SplineSample> sample_face_surface(const TopoDS_Face& face,
                                              std::size_t grid_n = 13) {
    std::vector<SplineSample> out;
    BRepAdaptor_Surface surface(face, true);
    const double u0 = surface.FirstUParameter();
    const double u1 = surface.LastUParameter();
    const double v0 = surface.FirstVParameter();
    const double v1 = surface.LastVParameter();
    if (!std::isfinite(u0) || !std::isfinite(u1)
        || !std::isfinite(v0) || !std::isfinite(v1)
        || u1 <= u0 || v1 <= v0) {
        return out;
    }
    out.reserve(grid_n * grid_n);
    for (std::size_t i = 0; i < grid_n; ++i) {
        const double tu = static_cast<double>(i) / static_cast<double>(grid_n - 1);
        const double u = u0 + (u1 - u0) * tu;
        for (std::size_t j = 0; j < grid_n; ++j) {
            const double tv = static_cast<double>(j) / static_cast<double>(grid_n - 1);
            const double v = v0 + (v1 - v0) * tv;
            try {
                out.push_back({surface.Value(u, v)});
            } catch (...) {
                // OCCT can throw on degenerate parameters; skip the sample.
            }
        }
    }
    return out;
}

double samples_bbox_diagonal(const std::vector<SplineSample>& samples) {
    if (samples.empty()) {
        return 0.0;
    }
    double xmin = samples[0].p.X();
    double xmax = xmin;
    double ymin = samples[0].p.Y();
    double ymax = ymin;
    double zmin = samples[0].p.Z();
    double zmax = zmin;
    for (const SplineSample& s : samples) {
        xmin = std::min(xmin, s.p.X());
        xmax = std::max(xmax, s.p.X());
        ymin = std::min(ymin, s.p.Y());
        ymax = std::max(ymax, s.p.Y());
        zmin = std::min(zmin, s.p.Z());
        zmax = std::max(zmax, s.p.Z());
    }
    const double dx = xmax - xmin;
    const double dy = ymax - ymin;
    const double dz = zmax - zmin;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Jacobi eigendecomposition for a 3x3 symmetric matrix. Returns eigenvalues
// ascending and corresponding eigenvectors in columns of V.
void jacobi_3x3(double a[3][3], double w[3], double V[3][3]) {
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            V[i][j] = (i == j) ? 1.0 : 0.0;
        }
    }
    for (int iter = 0; iter < 64; ++iter) {
        double off = std::abs(a[0][1]) + std::abs(a[0][2]) + std::abs(a[1][2]);
        if (off < 1.0e-18) {
            break;
        }
        for (int p = 0; p < 2; ++p) {
            for (int q = p + 1; q < 3; ++q) {
                const double apq = a[p][q];
                if (std::abs(apq) < 1.0e-20) {
                    continue;
                }
                const double app = a[p][p];
                const double aqq = a[q][q];
                const double theta = (aqq - app) / (2.0 * apq);
                double t;
                if (std::abs(theta) > 1.0e15) {
                    t = 1.0 / (2.0 * theta);
                } else {
                    const double sign = theta >= 0.0 ? 1.0 : -1.0;
                    t = sign / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
                }
                const double c = 1.0 / std::sqrt(1.0 + t * t);
                const double s = t * c;
                a[p][p] = app - t * apq;
                a[q][q] = aqq + t * apq;
                a[p][q] = 0.0;
                a[q][p] = 0.0;
                for (int r = 0; r < 3; ++r) {
                    if (r != p && r != q) {
                        const double arp = a[r][p];
                        const double arq = a[r][q];
                        a[r][p] = c * arp - s * arq;
                        a[p][r] = a[r][p];
                        a[r][q] = s * arp + c * arq;
                        a[q][r] = a[r][q];
                    }
                    const double vrp = V[r][p];
                    const double vrq = V[r][q];
                    V[r][p] = c * vrp - s * vrq;
                    V[r][q] = s * vrp + c * vrq;
                }
            }
        }
    }
    int idx[3] = {0, 1, 2};
    w[0] = a[0][0];
    w[1] = a[1][1];
    w[2] = a[2][2];
    for (int i = 0; i < 2; ++i) {
        for (int j = i + 1; j < 3; ++j) {
            if (w[idx[j]] < w[idx[i]]) {
                std::swap(idx[i], idx[j]);
            }
        }
    }
    double tmpw[3] = {w[idx[0]], w[idx[1]], w[idx[2]]};
    double tmpV[3][3];
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            tmpV[r][c] = V[r][idx[c]];
        }
    }
    for (int i = 0; i < 3; ++i) {
        w[i] = tmpw[i];
        for (int r = 0; r < 3; ++r) {
            V[r][i] = tmpV[r][i];
        }
    }
}

// Solve a 4x4 linear system Ax=b via Gaussian elimination with partial pivot.
bool solve_4x4(double A[4][4], double b[4], double x[4]) {
    double M[4][5];
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            M[i][j] = A[i][j];
        }
        M[i][4] = b[i];
    }
    for (int k = 0; k < 4; ++k) {
        int pivot = k;
        double pivot_abs = std::abs(M[k][k]);
        for (int i = k + 1; i < 4; ++i) {
            if (std::abs(M[i][k]) > pivot_abs) {
                pivot_abs = std::abs(M[i][k]);
                pivot = i;
            }
        }
        if (pivot_abs < 1.0e-18) {
            return false;
        }
        if (pivot != k) {
            for (int j = 0; j < 5; ++j) {
                std::swap(M[k][j], M[pivot][j]);
            }
        }
        for (int i = k + 1; i < 4; ++i) {
            const double f = M[i][k] / M[k][k];
            for (int j = k; j < 5; ++j) {
                M[i][j] -= f * M[k][j];
            }
        }
    }
    for (int i = 3; i >= 0; --i) {
        double s = M[i][4];
        for (int j = i + 1; j < 4; ++j) {
            s -= M[i][j] * x[j];
        }
        x[i] = s / M[i][i];
    }
    return true;
}

void finalize_fit_deviations(SplineFaceFit& fit,
                             const std::vector<double>& devs,
                             double scale) {
    if (devs.empty()) {
        fit.available = false;
        return;
    }
    double max_dev = 0.0;
    double sq = 0.0;
    for (double d : devs) {
        const double ad = std::abs(d);
        if (ad > max_dev) {
            max_dev = ad;
        }
        sq += d * d;
    }
    fit.sample_count = devs.size();
    fit.max_deviation = max_dev;
    fit.rms_deviation = std::sqrt(sq / devs.size());
    fit.relative_max_deviation = scale > kEps ? max_dev / scale : 0.0;
    fit.available = true;
}

SplineFaceFit fit_plane_from_samples(const std::vector<SplineSample>& samples,
                                     double scale) {
    SplineFaceFit fit;
    fit.kind = SplineFitKind::plane;
    if (samples.size() < 4) {
        return fit;
    }
    double cx = 0.0;
    double cy = 0.0;
    double cz = 0.0;
    for (const SplineSample& s : samples) {
        cx += s.p.X();
        cy += s.p.Y();
        cz += s.p.Z();
    }
    const double inv_n = 1.0 / static_cast<double>(samples.size());
    cx *= inv_n;
    cy *= inv_n;
    cz *= inv_n;
    double M[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    for (const SplineSample& s : samples) {
        const double dx = s.p.X() - cx;
        const double dy = s.p.Y() - cy;
        const double dz = s.p.Z() - cz;
        M[0][0] += dx * dx;
        M[0][1] += dx * dy;
        M[0][2] += dx * dz;
        M[1][1] += dy * dy;
        M[1][2] += dy * dz;
        M[2][2] += dz * dz;
    }
    M[1][0] = M[0][1];
    M[2][0] = M[0][2];
    M[2][1] = M[1][2];
    double w[3];
    double V[3][3];
    jacobi_3x3(M, w, V);
    const double nx = V[0][0];
    const double ny = V[1][0];
    const double nz = V[2][0];
    const double nm = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (nm < kEps) {
        return fit;
    }
    fit.origin = {cx, cy, cz};
    fit.direction = {nx / nm, ny / nm, nz / nm};
    std::vector<double> devs;
    devs.reserve(samples.size());
    for (const SplineSample& s : samples) {
        const double d = fit.direction.x * (s.p.X() - cx)
            + fit.direction.y * (s.p.Y() - cy)
            + fit.direction.z * (s.p.Z() - cz);
        devs.push_back(d);
    }
    finalize_fit_deviations(fit, devs, scale);
    return fit;
}

SplineFaceFit fit_sphere_from_samples(const std::vector<SplineSample>& samples,
                                      double scale) {
    SplineFaceFit fit;
    fit.kind = SplineFitKind::sphere;
    if (samples.size() < 5) {
        return fit;
    }
    double A[4][4] = {{0}};
    double b[4] = {0, 0, 0, 0};
    for (const SplineSample& s : samples) {
        const double x = s.p.X();
        const double y = s.p.Y();
        const double z = s.p.Z();
        const double rhs = x * x + y * y + z * z;
        const double row[4] = {2.0 * x, 2.0 * y, 2.0 * z, -1.0};
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                A[i][j] += row[i] * row[j];
            }
            b[i] += row[i] * rhs;
        }
    }
    double x[4];
    if (!solve_4x4(A, b, x)) {
        return fit;
    }
    const double cx = x[0];
    const double cy = x[1];
    const double cz = x[2];
    const double r2 = cx * cx + cy * cy + cz * cz - x[3];
    if (r2 <= 0.0) {
        return fit;
    }
    const double r = std::sqrt(r2);
    fit.origin = {cx, cy, cz};
    fit.radius = r;
    std::vector<double> devs;
    devs.reserve(samples.size());
    for (const SplineSample& s : samples) {
        const double dx = s.p.X() - cx;
        const double dy = s.p.Y() - cy;
        const double dz = s.p.Z() - cz;
        devs.push_back(std::sqrt(dx * dx + dy * dy + dz * dz) - r);
    }
    finalize_fit_deviations(fit, devs, scale);
    return fit;
}

// Try one candidate axis direction and fit a 2D circle in the perpendicular
// plane. Returns the RMS residual. Populates `out` with the resulting fit.
double try_cylinder_axis(const std::vector<SplineSample>& samples,
                         double cx, double cy, double cz,
                         double ax, double ay, double az,
                         double scale,
                         SplineFaceFit* out) {
    SplineFaceFit fit;
    fit.kind = SplineFitKind::cylinder;
    const double an = std::sqrt(ax * ax + ay * ay + az * az);
    if (an < kEps) {
        return std::numeric_limits<double>::infinity();
    }
    ax /= an;
    ay /= an;
    az /= an;

    double ex = 1.0;
    double ey = 0.0;
    double ez = 0.0;
    if (std::abs(ax) > 0.9) {
        ex = 0.0;
        ey = 1.0;
        ez = 0.0;
    }
    const double dot = ax * ex + ay * ey + az * ez;
    double e1x = ex - dot * ax;
    double e1y = ey - dot * ay;
    double e1z = ez - dot * az;
    const double e1n = std::sqrt(e1x * e1x + e1y * e1y + e1z * e1z);
    if (e1n < kEps) {
        return std::numeric_limits<double>::infinity();
    }
    e1x /= e1n;
    e1y /= e1n;
    e1z /= e1n;
    const double e2x = ay * e1z - az * e1y;
    const double e2y = az * e1x - ax * e1z;
    const double e2z = ax * e1y - ay * e1x;

    double AA[3][3] = {{0}};
    double bb[3] = {0, 0, 0};
    std::vector<double> u_coords;
    std::vector<double> v_coords;
    u_coords.reserve(samples.size());
    v_coords.reserve(samples.size());
    for (const SplineSample& s : samples) {
        const double dx = s.p.X() - cx;
        const double dy = s.p.Y() - cy;
        const double dz = s.p.Z() - cz;
        const double u = dx * e1x + dy * e1y + dz * e1z;
        const double v = dx * e2x + dy * e2y + dz * e2z;
        u_coords.push_back(u);
        v_coords.push_back(v);
        const double rhs = -(u * u + v * v);
        const double row[3] = {u, v, 1.0};
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                AA[i][j] += row[i] * row[j];
            }
            bb[i] += row[i] * rhs;
        }
    }
    double A4[4][4] = {{0}};
    double b4[4] = {0, 0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            A4[i][j] = AA[i][j];
        }
        b4[i] = bb[i];
    }
    A4[3][3] = 1.0;
    double sol[4];
    if (!solve_4x4(A4, b4, sol)) {
        return std::numeric_limits<double>::infinity();
    }
    const double D = sol[0];
    const double E = sol[1];
    const double F = sol[2];
    const double uc = -0.5 * D;
    const double vc = -0.5 * E;
    const double r2 = uc * uc + vc * vc - F;
    if (r2 <= 0.0) {
        return std::numeric_limits<double>::infinity();
    }
    const double r = std::sqrt(r2);
    const double ox = cx + uc * e1x + vc * e2x;
    const double oy = cy + uc * e1y + vc * e2y;
    const double oz = cz + uc * e1z + vc * e2z;
    fit.origin = {ox, oy, oz};
    fit.direction = {ax, ay, az};
    fit.radius = r;
    std::vector<double> devs;
    devs.reserve(samples.size());
    for (std::size_t i = 0; i < u_coords.size(); ++i) {
        const double du = u_coords[i] - uc;
        const double dv = v_coords[i] - vc;
        devs.push_back(std::sqrt(du * du + dv * dv) - r);
    }
    finalize_fit_deviations(fit, devs, scale);
    *out = fit;
    return fit.rms_deviation;
}

// Try one candidate cone axis direction. Given the axis, points project
// onto a 1D coordinate `t` (along axis) with radial distance `r`. A cone
// satisfies `r = alpha * (t - s)` for slope alpha = tan(half_angle) and
// apex axial coordinate `s`. We solve `r ≈ alpha*t + beta` by least squares
// (linear in alpha, beta); then alpha = tan(half_angle), s = -beta/alpha.
double try_cone_axis(const std::vector<SplineSample>& samples,
                     double cx, double cy, double cz,
                     double ax, double ay, double az,
                     double scale,
                     SplineFaceFit* out) {
    SplineFaceFit fit;
    fit.kind = SplineFitKind::cone;
    const double an = std::sqrt(ax * ax + ay * ay + az * az);
    if (an < kEps) {
        return std::numeric_limits<double>::infinity();
    }
    ax /= an;
    ay /= an;
    az /= an;

    std::vector<double> ts;
    std::vector<double> rs;
    ts.reserve(samples.size());
    rs.reserve(samples.size());
    for (const SplineSample& s : samples) {
        const double dx = s.p.X() - cx;
        const double dy = s.p.Y() - cy;
        const double dz = s.p.Z() - cz;
        const double t = dx * ax + dy * ay + dz * az;
        const double radial_x = dx - t * ax;
        const double radial_y = dy - t * ay;
        const double radial_z = dz - t * az;
        const double r = std::sqrt(radial_x * radial_x
                                   + radial_y * radial_y
                                   + radial_z * radial_z);
        ts.push_back(t);
        rs.push_back(r);
    }

    // Least-squares fit r = alpha * t + beta.
    double sum_t = 0.0;
    double sum_r = 0.0;
    double sum_tt = 0.0;
    double sum_tr = 0.0;
    const double n = static_cast<double>(ts.size());
    for (std::size_t i = 0; i < ts.size(); ++i) {
        sum_t += ts[i];
        sum_r += rs[i];
        sum_tt += ts[i] * ts[i];
        sum_tr += ts[i] * rs[i];
    }
    const double denom = n * sum_tt - sum_t * sum_t;
    if (std::abs(denom) < kEps) {
        return std::numeric_limits<double>::infinity();
    }
    const double alpha = (n * sum_tr - sum_t * sum_r) / denom;
    const double beta = (sum_r - alpha * sum_t) / n;
    if (std::abs(alpha) < 1.0e-6) {
        // Slope is too flat — this is effectively a cylinder, not a cone.
        return std::numeric_limits<double>::infinity();
    }
    const double half_angle = std::atan(std::abs(alpha));
    const double apex_t = -beta / alpha;
    const double apex_x = cx + apex_t * ax;
    const double apex_y = cy + apex_t * ay;
    const double apex_z = cz + apex_t * az;
    // Orient axis so the cone opens in the positive `t` direction.
    double dir_x = ax;
    double dir_y = ay;
    double dir_z = az;
    if (alpha < 0.0) {
        dir_x = -ax;
        dir_y = -ay;
        dir_z = -az;
    }

    fit.origin = {apex_x, apex_y, apex_z};
    fit.direction = {dir_x, dir_y, dir_z};
    fit.half_angle = half_angle;
    fit.radius = 0.0;

    // NOTE: the apex inherits the centroid's lateral position. For samples
    // that are angularly uniform around the true axis (e.g. a fully-revolved
    // cone with uniform u-parameter spacing), this is the right axis. For
    // NURBS-converted cones the u-knot vector is non-uniform, so the centroid
    // is laterally biased by O(few percent of the bbox diagonal). Closing
    // that gap requires either area-weighted sampling (via surface
    // derivatives) or a nonlinear axis-refinement step; deferred.
    const double cos_half = std::cos(half_angle);
    std::vector<double> devs;
    devs.reserve(ts.size());
    for (std::size_t i = 0; i < ts.size(); ++i) {
        const double expected = std::abs(alpha) * std::abs(ts[i] - apex_t);
        devs.push_back((rs[i] - expected) * cos_half);
    }
    finalize_fit_deviations(fit, devs, scale);
    *out = fit;
    return fit.rms_deviation;
}

SplineFaceFit fit_cone_from_samples(const std::vector<SplineSample>& samples,
                                    double scale) {
    SplineFaceFit fit;
    fit.kind = SplineFitKind::cone;
    if (samples.size() < 8) {
        return fit;
    }
    double cx = 0.0;
    double cy = 0.0;
    double cz = 0.0;
    for (const SplineSample& s : samples) {
        cx += s.p.X();
        cy += s.p.Y();
        cz += s.p.Z();
    }
    const double inv_n = 1.0 / static_cast<double>(samples.size());
    cx *= inv_n;
    cy *= inv_n;
    cz *= inv_n;
    double M[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    for (const SplineSample& s : samples) {
        const double dx = s.p.X() - cx;
        const double dy = s.p.Y() - cy;
        const double dz = s.p.Z() - cz;
        M[0][0] += dx * dx;
        M[0][1] += dx * dy;
        M[0][2] += dx * dz;
        M[1][1] += dy * dy;
        M[1][2] += dy * dz;
        M[2][2] += dz * dz;
    }
    M[1][0] = M[0][1];
    M[2][0] = M[0][2];
    M[2][1] = M[1][2];
    double w[3];
    double V[3][3];
    jacobi_3x3(M, w, V);

    SplineFaceFit best;
    double best_rms = std::numeric_limits<double>::infinity();
    auto try_axis = [&](double ax, double ay, double az) {
        SplineFaceFit candidate;
        const double rms = try_cone_axis(samples, cx, cy, cz, ax, ay, az,
                                         scale, &candidate);
        if (rms < best_rms) {
            best_rms = rms;
            best = candidate;
        }
    };
    for (int k = 0; k < 3; ++k) {
        try_axis(V[0][k], V[1][k], V[2][k]);
    }
    // Also try the world axes — for axis-aligned NURBS cones the moment
    // matrix can be nearly degenerate, leaving the PCA eigenvectors rotated
    // within the radial plane and producing a slightly tilted fit.
    try_axis(1.0, 0.0, 0.0);
    try_axis(0.0, 1.0, 0.0);
    try_axis(0.0, 0.0, 1.0);
    return best;
}

// A true cylinder's axis is one of the three principal axes of the centered
// sample cloud. Which one depends on the aspect ratio (a tall thin cylinder
// has the axis as the largest eigenvector; a short fat one has it as the
// smallest). We try all three and keep the best fit.
SplineFaceFit fit_cylinder_from_samples(const std::vector<SplineSample>& samples,
                                        double scale) {
    SplineFaceFit fit;
    fit.kind = SplineFitKind::cylinder;
    if (samples.size() < 6) {
        return fit;
    }
    double cx = 0.0;
    double cy = 0.0;
    double cz = 0.0;
    for (const SplineSample& s : samples) {
        cx += s.p.X();
        cy += s.p.Y();
        cz += s.p.Z();
    }
    const double inv_n = 1.0 / static_cast<double>(samples.size());
    cx *= inv_n;
    cy *= inv_n;
    cz *= inv_n;
    double M[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    for (const SplineSample& s : samples) {
        const double dx = s.p.X() - cx;
        const double dy = s.p.Y() - cy;
        const double dz = s.p.Z() - cz;
        M[0][0] += dx * dx;
        M[0][1] += dx * dy;
        M[0][2] += dx * dz;
        M[1][1] += dy * dy;
        M[1][2] += dy * dz;
        M[2][2] += dz * dz;
    }
    M[1][0] = M[0][1];
    M[2][0] = M[0][2];
    M[2][1] = M[1][2];
    double w[3];
    double V[3][3];
    jacobi_3x3(M, w, V);

    SplineFaceFit best;
    double best_rms = std::numeric_limits<double>::infinity();
    for (int k = 0; k < 3; ++k) {
        SplineFaceFit candidate;
        const double rms = try_cylinder_axis(samples, cx, cy, cz,
                                             V[0][k], V[1][k], V[2][k],
                                             scale, &candidate);
        if (rms < best_rms) {
            best_rms = rms;
            best = candidate;
        }
    }
    return best;
}

} // namespace

const char* spline_fit_kind_name(SplineFitKind kind) {
    switch (kind) {
    case SplineFitKind::none:
        return "none";
    case SplineFitKind::plane:
        return "plane";
    case SplineFitKind::sphere:
        return "sphere";
    case SplineFitKind::cylinder:
        return "cylinder";
    case SplineFitKind::cone:
        return "cone";
    }
    return "none";
}

SplineFitCandidates fit_spline_face(const TopoDS_Face& face) {
    SplineFitCandidates out;
    out.attempted = true;
    std::vector<SplineSample> samples = sample_face_surface(face);
    if (samples.size() < 4) {
        return out;
    }
    const double scale = samples_bbox_diagonal(samples);
    out.plane = fit_plane_from_samples(samples, scale);
    out.sphere = fit_sphere_from_samples(samples, scale);
    out.cylinder = fit_cylinder_from_samples(samples, scale);
    out.cone = fit_cone_from_samples(samples, scale);
    return out;
}

} // namespace csgify::detail

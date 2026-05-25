#include "hole_fill.hpp"

#include "engine_model.hpp"
#include "engine_internal.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <GProp_GProps.hxx>
#include <TopAbs.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Ax1.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>

namespace cad2csg {
namespace {

constexpr double kPi = 3.14159265358979323846264338327950288;

struct IndexedSolid {
    std::size_t index = 0;
    std::string id;
    TopoDS_Solid solid;
    BBox bbox;
    double volume = 0.0;
    bool active = true;
};

Diagnostic info_diagnostic(const std::string& code,
                           const std::string& message,
                           const std::string& shape_id = {}) {
    Diagnostic diagnostic;
    diagnostic.severity = DiagnosticSeverity::info;
    diagnostic.code = code;
    diagnostic.message = message;
    diagnostic.shape_id = shape_id;
    diagnostic.shape_label = shape_id;
    return diagnostic;
}

std::string hole_fill_kind_reason(HoleFillKind kind) {
    switch (kind) {
    case HoleFillKind::cylindrical_through:
        return "cylindrical_through";
    case HoleFillKind::cylindrical_blind:
        return "cylindrical_blind";
    case HoleFillKind::coaxial_stack:
        return "coaxial_stack";
    case HoleFillKind::planar_wire_patch:
        return "planar_wire_patch";
    case HoleFillKind::unknown:
        return "unknown";
    }
    return "unknown";
}

std::vector<IndexedSolid> collect_solids(const CadModel& model, bool active_only) {
    std::vector<IndexedSolid> solids;
    std::size_t solid_index = 0;
    for (TopExp_Explorer exp(model.root, TopAbs_SOLID); exp.More(); exp.Next()) {
        ++solid_index;
        const std::string id = detail::solid_id(solid_index);
        const bool active = !detail::contains_id(model.removed_solid_ids, id);
        if (active_only && !active) {
            continue;
        }
        IndexedSolid entry;
        entry.index = solid_index;
        entry.id = id;
        entry.active = active;
        entry.solid = TopoDS::Solid(exp.Current());
        entry.bbox = detail::bbox_from_shape(entry.solid);
        GProp_GProps props;
        BRepGProp::VolumeProperties(entry.solid, props);
        entry.volume = std::abs(props.Mass());
        solids.push_back(entry);
    }
    return solids;
}

const IndexedSolid* find_solid(const std::vector<IndexedSolid>& solids,
                               const std::string& id) {
    for (const IndexedSolid& solid : solids) {
        if (solid.id == id) {
            return &solid;
        }
    }
    return nullptr;
}

bool bbox_intersects(const BBox& a, const BBox& b, double pad) {
    if (!a.valid || !b.valid) {
        return false;
    }
    return a.xmin - pad <= b.xmax && a.xmax + pad >= b.xmin
        && a.ymin - pad <= b.ymax && a.ymax + pad >= b.ymin
        && a.zmin - pad <= b.zmax && a.zmax + pad >= b.zmin;
}

Vec3 vec_from_dir(const gp_Dir& dir) {
    return {dir.X(), dir.Y(), dir.Z()};
}

Vec3 vec_from_pnt(const gp_Pnt& pnt) {
    return {pnt.X(), pnt.Y(), pnt.Z()};
}

gp_Pnt pnt_from_vec(Vec3 v) {
    return {v.x, v.y, v.z};
}

double projection_on_axis(Vec3 point, Vec3 origin, Vec3 direction) {
    return detail::dot(detail::sub(point, origin), direction);
}

bool projected_axis_interval(const BBox& bbox,
                             Vec3 origin,
                             Vec3 direction,
                             double* min_out,
                             double* max_out) {
    const std::vector<Vec3> corners = detail::bbox_corners(bbox);
    if (corners.empty()) {
        return false;
    }
    double min_value = projection_on_axis(corners.front(), origin, direction);
    double max_value = min_value;
    for (Vec3 corner : corners) {
        const double value = projection_on_axis(corner, origin, direction);
        min_value = std::min(min_value, value);
        max_value = std::max(max_value, value);
    }
    *min_out = min_value;
    *max_out = max_value;
    return max_value > min_value;
}

bool vector_is_finite(Vec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

DecisionRequest decision_request_for_candidate(const HoleFillCandidate& candidate) {
    DecisionRequest request;
    request.id = "decision:" + candidate.id;
    request.kind = "hole_fill";
    request.target_id = candidate.id;
    request.reason = "removed component may leave a small cylindrical cavity";
    request.recommended_action = candidate.suggested_action.empty()
        ? "fill_hole"
        : candidate.suggested_action;
    request.summary = "Review " + candidate.id + " in " + candidate.host_solid_id;
    request.metrics.push_back({"radius", candidate.radius, "model"});
    request.metrics.push_back({"length", candidate.length, "model"});
    request.metrics.push_back({"plug_volume", candidate.plug_volume, "model^3"});
    request.metrics.push_back({"plug_volume_fraction_of_host",
                               candidate.plug_volume_fraction_of_host,
                               "ratio"});

    DecisionCandidateView fill;
    fill.id = candidate.id;
    fill.action = "fill_hole";
    fill.confidence = candidate.confidence;
    fill.reason = "small cylindrical void in host solid";
    fill.metrics = request.metrics;
    request.candidates.push_back(fill);

    DecisionCandidateView leave;
    leave.id = "leave_void:" + candidate.id;
    leave.action = "leave_void";
    leave.confidence = "medium";
    leave.reason = "the hole may represent a real void or streaming path";
    request.candidates.push_back(leave);
    return request;
}

void add_candidate_from_cylindrical_face(HoleFillProposal& proposal,
                                         const IndexedSolid& host,
                                         const TopoDS_Face& face,
                                         std::size_t face_index,
                                         const HoleFillOptions& hole_options,
                                         double tolerance) {
    BRepAdaptor_Surface adaptor(face, false);
    if (adaptor.GetType() != GeomAbs_Cylinder) {
        return;
    }
    const gp_Cylinder cylinder = adaptor.Cylinder();
    const double radius = cylinder.Radius();
    if (radius <= tolerance) {
        return;
    }
    if (hole_options.has_max_radius && radius > hole_options.max_radius) {
        return;
    }

    const gp_Ax1 axis = cylinder.Axis();
    Vec3 origin = vec_from_pnt(axis.Location());
    Vec3 direction = vec_from_dir(axis.Direction());
    if (!vector_is_finite(origin) || !vector_is_finite(direction)
        || detail::vec_len(direction) <= detail::kEps) {
        return;
    }
    direction = detail::scale(direction, 1.0 / detail::vec_len(direction));

    const BBox face_bbox = detail::bbox_from_shape(face);
    double tmin = 0.0;
    double tmax = 0.0;
    if (!projected_axis_interval(face_bbox, origin, direction, &tmin, &tmax)) {
        return;
    }
    const double length = tmax - tmin;
    if (length <= tolerance) {
        return;
    }

    const Vec3 base = detail::add(origin, detail::scale(direction, tmin));
    const Vec3 height = detail::scale(direction, length);
    const Vec3 mid = detail::add(base, detail::scale(height, 0.5));
    const bool axis_mid_inside =
        detail::point_is_inside_or_on(host.solid, pnt_from_vec(mid), tolerance);
    if (axis_mid_inside) {
        return;
    }

    const double plug_volume = kPi * radius * radius * length;
    if (plug_volume <= 0.0 || host.volume <= 0.0) {
        return;
    }
    const double volume_fraction = plug_volume / host.volume;
    if (hole_options.has_max_volume_fraction
        && volume_fraction > hole_options.max_volume_fraction) {
        return;
    }

    HoleFillCandidate candidate;
    std::ostringstream id;
    id << "hole-fill:" << (proposal.candidates.size() + 1);
    candidate.id = id.str();
    candidate.host_solid_id = host.id;
    if (!hole_options.near_removed_solid_id.empty()) {
        candidate.seed_removed_solid_ids.push_back(hole_options.near_removed_solid_id);
    }
    candidate.face_ids.push_back(detail::face_id(host.index, face_index));
    candidate.kind = HoleFillKind::cylindrical_through;
    candidate.axis_origin = origin;
    candidate.axis_direction = direction;
    candidate.base = base;
    candidate.height = height;
    candidate.radius = radius;
    candidate.length = length;
    candidate.plug_volume = plug_volume;
    candidate.host_volume = host.volume;
    candidate.plug_volume_fraction_of_host = volume_fraction;
    candidate.estimated_host_overlap_fraction = 0.0;
    candidate.estimated_other_overlap_fraction = 0.0;
    candidate.confidence = hole_options.near_removed_solid_id.empty()
        ? "medium"
        : "high";
    candidate.score = hole_options.near_removed_solid_id.empty() ? 0.55 : 0.85;
    candidate.reason_codes.push_back(hole_fill_kind_reason(candidate.kind));
    candidate.reason_codes.push_back("axis_midpoint_outside_host");
    if (!hole_options.near_removed_solid_id.empty()) {
        candidate.reason_codes.push_back("near_removed_solid");
    }
    candidate.suggested_action = "fill_hole";

    proposal.decision_requests.push_back(decision_request_for_candidate(candidate));
    proposal.candidates.push_back(candidate);
}

} // namespace

HoleFillProposal propose_hole_fill_candidates(
    const CadModel& model,
    const HoleFillOptions& hole_options,
    const ConversionOptions& conversion_options) {
    HoleFillProposal proposal;
    proposal.ok = model.loaded();
    if (!proposal.ok) {
        Diagnostic diagnostic;
        diagnostic.severity = DiagnosticSeverity::error;
        diagnostic.code = "model_not_loaded";
        diagnostic.message = "Hole-fill proposal requires a loaded STEP model.";
        proposal.diagnostics.push_back(diagnostic);
        return proposal;
    }

    const std::vector<IndexedSolid> all_solids = collect_solids(model, false);
    const std::vector<IndexedSolid> active_solids = collect_solids(model, true);
    const IndexedSolid* removed = nullptr;
    if (!hole_options.near_removed_solid_id.empty()) {
        removed = find_solid(all_solids, hole_options.near_removed_solid_id);
        if (!removed) {
            proposal.diagnostics.push_back(info_diagnostic(
                "hole_fill_removed_solid_not_found",
                "The near-removed solid was not found in the loaded model.",
                hole_options.near_removed_solid_id));
        }
    }

    const double search_pad = hole_options.has_max_radius
        ? std::max(hole_options.max_radius * 2.0, conversion_options.linear_tolerance)
        : std::max(conversion_options.linear_tolerance * 100.0, 1.0e-6);

    for (const IndexedSolid& host : active_solids) {
        if (!hole_options.host_solid_id.empty() && host.id != hole_options.host_solid_id) {
            continue;
        }
        if (removed && host.id == removed->id) {
            continue;
        }
        if (removed && !bbox_intersects(host.bbox, removed->bbox, search_pad)) {
            continue;
        }

        std::size_t face_index = 0;
        for (TopExp_Explorer face_exp(host.solid, TopAbs_FACE);
             face_exp.More();
             face_exp.Next()) {
            ++face_index;
            add_candidate_from_cylindrical_face(
                proposal,
                host,
                TopoDS::Face(face_exp.Current()),
                face_index,
                hole_options,
                std::max(conversion_options.linear_tolerance, 1.0e-7));
            if (proposal.candidates.size() >= hole_options.max_candidates) {
                break;
            }
        }
        if (proposal.candidates.size() >= hole_options.max_candidates) {
            proposal.diagnostics.push_back(info_diagnostic(
                "hole_fill_candidate_limit_reached",
                "Hole-fill candidate search stopped at the configured limit.",
                host.id));
            break;
        }
    }

    if (proposal.candidates.empty()) {
        proposal.diagnostics.push_back(info_diagnostic(
            "hole_fill_no_candidates",
            "No simple cylindrical hole-fill candidates were found.",
            hole_options.host_solid_id.empty()
                ? hole_options.near_removed_solid_id
                : hole_options.host_solid_id));
    }
    return proposal;
}

} // namespace cad2csg

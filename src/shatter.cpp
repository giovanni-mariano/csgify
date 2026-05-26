// Exhaustive surface-driven subdivision ("shatter").
//
// Where the proposal/confidence path (src/cleanup.cpp) searches for one good
// face cut and may stall at `*_needs_decision`, this op never searches and
// never needs a convex result up front: it cuts the solid by the bounded
// cutter of every one of its own surfaces (src/decompose.cpp
// split_solid_by_all_surfaces), then recurses into any piece that still fails
// convex halfspace validation until every piece is a simple CSG cell. The
// subdivision is deterministic, so it either reduces the solid to simple
// pieces or reports the specific residual that defeated it -- there is no
// confidence gate.

#include "decompose.hpp"
#include "engine_internal.hpp"
#include "engine_model.hpp"

#include <cmath>
#include <cstddef>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Solid.hxx>
#include <gp_Pnt.hxx>

namespace cad2csg {

bool shatter_solid(CadModel& model,
                   const std::string& id,
                   const ConversionOptions& options) {
    using namespace cad2csg::detail;

    std::size_t target_index = 0;
    if (!parse_solid_id(id, &target_index)) {
        model.diagnostics.push_back({
            DiagnosticSeverity::error, "solid_not_found",
            "Shatter target must be a solid:N identifier.", id});
        return false;
    }

    TopoDS_Solid root_solid;
    std::size_t solid_index = 0;
    bool found = false;
    {
        std::size_t i = 0;
        for (TopExp_Explorer exp(model.root, TopAbs_SOLID); exp.More(); exp.Next()) {
            ++i;
            if (i == target_index) {
                root_solid = TopoDS::Solid(exp.Current());
                solid_index = i;
                found = true;
                break;
            }
        }
    }
    if (!found) {
        model.diagnostics.push_back({
            DiagnosticSeverity::error, "solid_not_found",
            "Requested solid ID was not found.", id});
        return false;
    }

    GProp_GProps root_props;
    BRepGProp::VolumeProperties(root_solid, root_props);
    const double original_volume = std::abs(root_props.Mass());
    if (original_volume <= kEps) {
        model.diagnostics.push_back({
            DiagnosticSeverity::error, "empty_or_tiny_solid",
            "Cannot shatter a near-zero-volume solid.", id});
        return false;
    }

    // A piece is "simple" iff it lies on one definite side of every surface
    // bounding it -- exactly the condition the emit path requires. Use a
    // scratch report so the validator's diagnostics do not leak into the model.
    auto is_simple = [&](const TopoDS_Solid& solid) -> bool {
        GProp_GProps props;
        BRepGProp::VolumeProperties(solid, props);
        gp_Pnt interior;
        if (!find_interior_point(solid, props, options.linear_tolerance, &interior)) {
            return false;
        }
        ConversionReport scratch;
        return validate_convex_halfspace_cell(solid, interior, solid_index, options,
                                              scratch, false, false, false,
                                              &model.face_replacements);
    };

    const int max_depth = options.shatter_max_depth > 0 ? options.shatter_max_depth : 8;

    struct WorkItem {
        TopoDS_Solid solid;
        int depth;
    };
    std::vector<WorkItem> work;
    work.push_back({root_solid, 0});

    std::vector<TopoDS_Solid> final_pieces;
    std::size_t residual_nonconvex = 0;
    bool any_split = false;

    while (!work.empty()) {
        const WorkItem item = work.back();
        work.pop_back();

        if (is_simple(item.solid)) {
            final_pieces.push_back(item.solid);
            continue;
        }
        if (item.depth >= max_depth) {
            final_pieces.push_back(item.solid);
            ++residual_nonconvex;
            continue;
        }

        ConversionReport scratch;
        std::vector<TopoDS_Solid> pieces =
            split_solid_by_all_surfaces(item.solid, options, scratch, solid_index);
        if (pieces.size() <= 1) {
            // No progress: the solid's own surfaces could not reduce this
            // piece further (e.g. a torus or spline-bounded region that needs
            // surface replacement, not decomposition).
            final_pieces.push_back(item.solid);
            ++residual_nonconvex;
            continue;
        }
        any_split = true;
        for (TopoDS_Solid& piece : pieces) {
            work.push_back({piece, item.depth + 1});
        }
    }

    if (!any_split) {
        if (residual_nonconvex == 0) {
            // Already a single simple cell -- leave solid:N intact so its
            // identity (and any metadata anchored on it) survives.
            model.diagnostics.push_back({
                DiagnosticSeverity::info, "shatter_not_needed",
                "Solid already validates as a single convex halfspace cell; "
                "no subdivision applied.", id});
            return true;
        }
        model.diagnostics.push_back({
            DiagnosticSeverity::error, "shatter_residual_nonconvex",
            "Solid is non-convex but no surface cut subdivided it; it likely "
            "needs surface replacement (torus/spline) rather than "
            "decomposition. No CSG emitted.", id});
        return false;
    }

    // Drop degenerate sliver pieces produced by the splitter.
    const double sliver_floor = original_volume * 1.0e-9;
    std::vector<TopoDS_Solid> kept;
    kept.reserve(final_pieces.size());
    double pieces_volume = 0.0;
    for (const TopoDS_Solid& piece : final_pieces) {
        GProp_GProps props;
        BRepGProp::VolumeProperties(piece, props);
        const double volume = std::abs(props.Mass());
        if (volume <= sliver_floor) {
            continue;
        }
        kept.push_back(piece);
        pieces_volume += volume;
    }
    if (kept.empty()) {
        model.diagnostics.push_back({
            DiagnosticSeverity::error, "shatter_no_pieces",
            "Subdivision produced only degenerate pieces.", id});
        return false;
    }

    const double relative_delta =
        std::abs(pieces_volume - original_volume) / original_volume;
    if (relative_delta > options.linear_tolerance
        && !options.allow_shatter_volume_drift) {
        std::ostringstream msg;
        msg << "Shatter changed volume by relative delta " << relative_delta
            << " (> linear_tolerance " << options.linear_tolerance
            << "). Refusing to mutate; set allow_shatter_volume_drift to override.";
        model.diagnostics.push_back({
            DiagnosticSeverity::error, "decomposition_volume_changed", msg.str(), id});
        return false;
    }

    std::ostringstream msg;
    msg << "Shatter produced " << kept.size() << " pieces (max_depth=" << max_depth
        << "). original_volume=" << std::setprecision(10) << original_volume
        << " pieces_volume=" << pieces_volume
        << " relative_delta=" << relative_delta;
    model.diagnostics.push_back({
        relative_delta <= options.linear_tolerance ? DiagnosticSeverity::info
                                                    : DiagnosticSeverity::warning,
        "shatter", msg.str(), id});

    if (residual_nonconvex > 0) {
        std::ostringstream rmsg;
        rmsg << residual_nonconvex << " shattered piece(s) still fail convex "
                "validation and will be reported per-piece at emit.";
        model.diagnostics.push_back({
            DiagnosticSeverity::warning, "shatter_residual_nonconvex", rmsg.str(), id});
    }

    remove_solid(model, id);
    for (const TopoDS_Solid& piece : kept) {
        GProp_GProps props;
        BRepGProp::VolumeProperties(piece, props);
        DecomposedSolidPiece decomposed;
        decomposed.id = "piece:" + std::to_string(model.decomposed_pieces.size() + 1);
        decomposed.label = decomposed.id + "/from_" + id;
        decomposed.source_solid_id = id;
        decomposed.solid = piece;
        decomposed.volume = std::abs(props.Mass());
        model.decomposed_pieces.push_back(decomposed);
    }
    return true;
}

} // namespace cad2csg

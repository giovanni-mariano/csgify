#include "step_export.hpp"

#include "engine_internal.hpp"  // parse_solid_id
#include "engine_model.hpp"

#include <iostream>
#include <string>
#include <unordered_set>

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <gp_Ax2.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <STEPControl_StepModelType.hxx>
#include <STEPControl_Writer.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Shape.hxx>

namespace cad2csg {
namespace {

std::unordered_set<std::size_t> removed_indices(const CadModel& model) {
    std::unordered_set<std::size_t> out;
    for (const std::string& id : model.removed_solid_ids) {
        std::size_t idx = 0;
        if (detail::parse_solid_id(id, &idx)) {
            out.insert(idx);
        }
    }
    return out;
}

TopoDS_Shape build_synthetic_box(const SyntheticBody& body) {
    if (!body.bbox.valid) return {};
    const gp_Pnt corner(body.bbox.xmin, body.bbox.ymin, body.bbox.zmin);
    const double dx = body.bbox.xmax - body.bbox.xmin;
    const double dy = body.bbox.ymax - body.bbox.ymin;
    const double dz = body.bbox.zmax - body.bbox.zmin;
    if (dx <= 0.0 || dy <= 0.0 || dz <= 0.0) return {};
    BRepPrimAPI_MakeBox maker(corner, dx, dy, dz);
    maker.Build();
    if (!maker.IsDone()) {
        std::cerr << "  BRepPrimAPI_MakeBox::IsDone() returned false\n";
        return {};
    }
    return maker.Solid();
}

TopoDS_Shape build_synthetic_sphere(const SyntheticBody& body) {
    if (body.radius <= 0.0) return {};
    const gp_Pnt center(body.center.x, body.center.y, body.center.z);
    BRepPrimAPI_MakeSphere maker(center, body.radius);
    maker.Build();
    if (!maker.IsDone()) return {};
    return maker.Solid();
}

bool axis_from_height(const SyntheticBody& body, gp_Ax2* axis_out,
                      double* length_out) {
    const gp_Pnt base(body.base.x, body.base.y, body.base.z);
    const gp_Vec height(body.height.x, body.height.y, body.height.z);
    const double length = height.Magnitude();
    if (length <= 0.0) return false;
    const gp_Dir dir(height);
    *axis_out = gp_Ax2(base, dir);
    *length_out = length;
    return true;
}

TopoDS_Shape build_synthetic_cylinder(const SyntheticBody& body) {
    if (body.radius <= 0.0) return {};
    gp_Ax2 axis;
    double length = 0.0;
    if (!axis_from_height(body, &axis, &length)) return {};
    BRepPrimAPI_MakeCylinder maker(axis, body.radius, length);
    maker.Build();
    if (!maker.IsDone()) return {};
    return maker.Solid();
}

TopoDS_Shape build_synthetic_cone(const SyntheticBody& body) {
    gp_Ax2 axis;
    double length = 0.0;
    if (!axis_from_height(body, &axis, &length)) return {};
    BRepPrimAPI_MakeCone maker(axis, body.radius, body.top_radius, length);
    maker.Build();
    if (!maker.IsDone()) return {};
    return maker.Solid();
}

TopoDS_Shape build_synthetic_placeholder(const SyntheticBody& body) {
    // Boolean synthetic nodes (union/intersection/difference) need a
    // recursive BRepAlgoAPI_* build from leaves. Pass 2 territory.
    // For pass 1, emit the bbox as a labeled placeholder so the user
    // sees *something* at the right location during cross-tool review.
    std::cerr << "warning: synthetic boolean body " << body.id
              << " exported as bbox placeholder (pass 2 will build the"
                 " actual CSG)\n";
    return build_synthetic_box(body);
}

TopoDS_Shape build_synthetic(const SyntheticBody& body) {
    switch (body.kind) {
    case SyntheticBodyKind::box:      return build_synthetic_box(body);
    case SyntheticBodyKind::sphere:   return build_synthetic_sphere(body);
    case SyntheticBodyKind::cylinder: return build_synthetic_cylinder(body);
    case SyntheticBodyKind::cone:     return build_synthetic_cone(body);
    case SyntheticBodyKind::union_body:
    case SyntheticBodyKind::intersection_body:
    case SyntheticBodyKind::difference_body:
        return build_synthetic_placeholder(body);
    }
    return {};
}

} // namespace

int export_step(const CadModel& model, const std::string& output_path) {
    if (model.root.IsNull()) {
        std::cerr << "error step_export: model root is null\n";
        return 1;
    }

    STEPControl_Writer writer;
    const std::unordered_set<std::size_t> removed = removed_indices(model);

    // Surviving solids from the original root. Index matches solid:N.
    std::size_t index = 0;
    int transferred = 0;
    for (TopExp_Explorer exp(model.root, TopAbs_SOLID); exp.More(); exp.Next()) {
        ++index;
        if (removed.count(index)) continue;
        const IFSelect_ReturnStatus s =
            writer.Transfer(exp.Current(), STEPControl_AsIs);
        if (s != IFSelect_RetDone) {
            std::cerr << "warning: transfer failed for solid:" << index << "\n";
            continue;
        }
        ++transferred;
    }

    // Decomposition pieces — they replaced their source solid, which is
    // recorded in removed_solid_ids and therefore already skipped above.
    for (const DecomposedSolidPiece& piece : model.decomposed_pieces) {
        if (!piece.active) continue;
        if (piece.solid.IsNull()) continue;
        const IFSelect_ReturnStatus s =
            writer.Transfer(piece.solid, STEPControl_AsIs);
        if (s != IFSelect_RetDone) {
            std::cerr << "warning: transfer failed for piece " << piece.id << "\n";
            continue;
        }
        ++transferred;
    }

    // Synthetic bodies — materialized as OCCT primitives. Booleans are
    // emitted as bbox placeholders for now.
    for (const SyntheticBody& body : model.synthetic_bodies) {
        if (!body.emit_as_cell) continue;
        const TopoDS_Shape shape = build_synthetic(body);
        if (shape.IsNull()) {
            std::cerr << "warning: synthetic body " << body.id
                      << " could not be materialized for STEP export\n";
            continue;
        }
        const IFSelect_ReturnStatus s = writer.Transfer(shape, STEPControl_AsIs);
        if (s != IFSelect_RetDone) {
            std::cerr << "warning: transfer failed for synthetic " << body.id << "\n";
            continue;
        }
        ++transferred;
    }

    if (transferred == 0) {
        std::cerr << "error step_export: nothing to write (every solid removed?)\n";
        return 1;
    }

    const IFSelect_ReturnStatus status = writer.Write(output_path.c_str());
    if (status != IFSelect_RetDone) {
        std::cerr << "error step_export: write failed for " << output_path << "\n";
        return 1;
    }
    return 0;
}

} // namespace cad2csg

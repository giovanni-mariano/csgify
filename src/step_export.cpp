#include "step_export.hpp"

#include "engine_internal.hpp"  // parse_solid_id
#include "engine_model.hpp"

#include <fstream>
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
#include <STEPCAFControl_Writer.hxx>
#include <TCollection_ExtendedString.hxx>
#include <TDataStd_Name.hxx>
#include <TDF_Label.hxx>
#include <TDocStd_Document.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Shape.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>

namespace cad2csg {
namespace {

class CoutSilencer {
public:
    CoutSilencer() : null_("/dev/null"), old_(std::cout.rdbuf(null_.rdbuf())) {}
    ~CoutSilencer() { std::cout.rdbuf(old_); }

private:
    std::ofstream null_;
    std::streambuf* old_ = nullptr;
};

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

const ShapeMetadata* metadata_for_solid(const CadModel& model, std::size_t index) {
    const std::string id = "solid:" + std::to_string(index);
    for (const ShapeMetadata& metadata : model.solid_metadata) {
        if (metadata.shape_id == id) {
            return &metadata;
        }
    }
    return nullptr;
}

std::string fallback_name_for_solid(const CadModel& model, std::size_t index) {
    if (const ShapeMetadata* metadata = metadata_for_solid(model, index)) {
        if (!metadata->name.empty()) {
            return metadata->name;
        }
        if (!metadata->hierarchy_path.empty()) {
            return metadata->hierarchy_path;
        }
    }
    return "solid:" + std::to_string(index);
}

std::string source_name_for_piece(const CadModel& model,
                                  const DecomposedSolidPiece& piece) {
    std::size_t index = 0;
    if (detail::parse_solid_id(piece.source_solid_id, &index)) {
        return fallback_name_for_solid(model, index);
    }
    return piece.source_solid_id.empty() ? "decomposed" : piece.source_solid_id;
}

std::string export_name_for_piece(const CadModel& model,
                                  const DecomposedSolidPiece& piece) {
    return source_name_for_piece(model, piece) + " / " + piece.id;
}

std::string export_name_for_synthetic(const SyntheticBody& body) {
    if (!body.label.empty()) {
        return body.label;
    }
    return body.id.empty() ? "synthetic" : body.id;
}

std::string source_name_from_replacement_label(const CadModel& model,
                                               const std::string& label,
                                               std::string* operation_out) {
    const std::string marker = "_from_solid:";
    const std::size_t marker_pos = label.find(marker);
    if (marker_pos == std::string::npos) {
        return {};
    }
    const std::size_t op_start = label.find('/');
    if (op_start == std::string::npos || op_start + 1 >= marker_pos) {
        return {};
    }
    if (operation_out) {
        *operation_out = label.substr(op_start + 1, marker_pos - (op_start + 1));
    }

    const std::size_t index_start = marker_pos + marker.size();
    std::size_t index_end = index_start;
    while (index_end < label.size()
           && label[index_end] >= '0'
           && label[index_end] <= '9') {
        ++index_end;
    }
    if (index_end == index_start) {
        return {};
    }
    const std::size_t index =
        static_cast<std::size_t>(std::stoull(label.substr(index_start,
                                                          index_end - index_start)));
    return fallback_name_for_solid(model, index);
}

std::string export_name_for_synthetic(const CadModel& model,
                                      const SyntheticBody& body) {
    if (body.role == "hole_fill") {
        std::string host_name = body.host_solid_id;
        std::size_t host_index = 0;
        if (detail::parse_solid_id(body.host_solid_id, &host_index)) {
            host_name = fallback_name_for_solid(model, host_index);
        }
        if (!body.source_solid_id.empty()) {
            std::string source_name = body.source_solid_id;
            std::size_t source_index = 0;
            if (detail::parse_solid_id(body.source_solid_id, &source_index)) {
                source_name = fallback_name_for_solid(model, source_index);
            }
            return host_name + " / fill:hole from " + source_name;
        }
        return host_name + " / fill:hole";
    }
    std::string operation;
    const std::string source =
        source_name_from_replacement_label(model, body.label, &operation);
    if (!source.empty() && !operation.empty()) {
        return source + " / replacement:" + operation;
    }
    return export_name_for_synthetic(body);
}

void add_named_shape(const Handle(XCAFDoc_ShapeTool)& shape_tool,
                     const TopoDS_Shape& shape,
                     const std::string& name,
                     int* transferred) {
    if (shape_tool.IsNull() || shape.IsNull()) {
        return;
    }
    TDF_Label label = shape_tool->AddShape(shape, /*makeAssembly=*/false);
    if (!name.empty()) {
        TDataStd_Name::Set(label, TCollection_ExtendedString(name.c_str()));
    }
    ++(*transferred);
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

    CoutSilencer silence_occt_transfer_stats;
    Handle(TDocStd_Document) doc;
    XCAFApp_Application::GetApplication()->NewDocument("MDTV-XCAF", doc);
    Handle(XCAFDoc_ShapeTool) shape_tool =
        XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    if (shape_tool.IsNull()) {
        std::cerr << "error step_export: could not create XCAF shape tool\n";
        return 1;
    }
    XCAFDoc_ShapeTool::SetAutoNaming(false);

    const std::unordered_set<std::size_t> removed = removed_indices(model);

    // Surviving solids from the original root. Index matches solid:N.
    std::size_t index = 0;
    int transferred = 0;
    for (TopExp_Explorer exp(model.root, TopAbs_SOLID); exp.More(); exp.Next()) {
        ++index;
        if (removed.count(index)) continue;
        add_named_shape(shape_tool,
                        exp.Current(),
                        fallback_name_for_solid(model, index),
                        &transferred);
    }

    // Decomposition pieces — they replaced their source solid, which is
    // recorded in removed_solid_ids and therefore already skipped above.
    for (const DecomposedSolidPiece& piece : model.decomposed_pieces) {
        if (!piece.active) continue;
        if (piece.solid.IsNull()) continue;
        add_named_shape(shape_tool,
                        piece.solid,
                        export_name_for_piece(model, piece),
                        &transferred);
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
        add_named_shape(shape_tool,
                        shape,
                        export_name_for_synthetic(model, body),
                        &transferred);
    }

    if (transferred == 0) {
        std::cerr << "error step_export: nothing to write (every solid removed?)\n";
        return 1;
    }

    STEPCAFControl_Writer writer;
    writer.SetNameMode(true);
    if (!writer.Transfer(doc, STEPControl_AsIs)) {
        std::cerr << "error step_export: document transfer failed\n";
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

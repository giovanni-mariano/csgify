// STEP import. Reads a file through OCCT's STEP reader, optionally runs
// ShapeFix, and packages the result as a CadModel ready for the analyze
// and emit stages. Pure import — no Alea, no traversal of solids.

#include "engine_model.hpp"

#include <cmath>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TCollection_AsciiString.hxx>
#include <TDataStd_Name.hxx>
#include <TDF_Label.hxx>
#include <TDF_Tool.hxx>
#include <TDocStd_Document.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <NCollection_Sequence.hxx>
#include <ShapeFix_Shape.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <STEPControl_Reader.hxx>
#include <TopoDS_Shape.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>

namespace cad2csg {
namespace {

void add_diag(ConversionReport& report,
              DiagnosticSeverity severity,
              const std::string& code,
              const std::string& message,
              const std::string& label = {}) {
    report.diagnostics.push_back({severity, code, message, label});
}

std::string label_entry(const TDF_Label& label) {
    TCollection_AsciiString entry;
    TDF_Tool::Entry(label, entry);
    return entry.ToCString();
}

std::string label_name(const TDF_Label& label) {
    Handle(TDataStd_Name) name;
    if (!label.FindAttribute(TDataStd_Name::GetID(), name) || name.IsNull()) {
        return {};
    }
    return TCollection_AsciiString(name->Get(), '?').ToCString();
}

std::string node_id(std::size_t index) {
    std::ostringstream out;
    out << "asm:" << index;
    return out.str();
}

// Walk parent chain to build a path that disambiguates instances: at each
// level we append the label name (or the entry as a fallback), and if a
// sibling with the same name already exists we append the label entry to
// keep instances distinct. Stops when Father() becomes null or the root
// document label is reached.
std::string build_path_from_label(const TDF_Label& leaf) {
    std::vector<std::string> segments;
    for (TDF_Label cur = leaf; !cur.IsNull() && cur.Depth() > 0;
         cur = cur.Father()) {
        std::string name = label_name(cur);
        const std::string entry = label_entry(cur);
        if (name.empty()) {
            name = entry;
        } else {
            // Disambiguate same-named siblings by appending the entry tag.
            // The entry is globally unique, so identical-name instances
            // produce different segments.
            name += "#" + entry;
        }
        segments.push_back(name);
    }
    std::string path;
    for (auto it = segments.rbegin(); it != segments.rend(); ++it) {
        if (!path.empty()) path += "/";
        path += *it;
    }
    return path;
}

void collect_assembly_node(const Handle(XCAFDoc_ShapeTool)& shape_tool,
                           const TDF_Label& label,
                           const std::string& parent_id,
                           const std::string& parent_path,
                           std::size_t* next_node_index,
                           std::vector<AssemblyNode>* nodes) {
    const std::string name = label_name(label);
    const std::string id = node_id((*next_node_index)++);
    std::string path = parent_path;
    if (!name.empty()) {
        path = path.empty() ? name : path + "/" + name;
    }
    if (path.empty()) {
        path = label_entry(label);
    }

    AssemblyNode node;
    node.id = id;
    node.parent_id = parent_id;
    node.name = name;
    // Record the *component* entry: for instanced children this differs
    // between two copies of the same prototype, which is what lets us
    // associate each instance with its own solid below.
    node.label_entry = label_entry(label);
    node.kind = XCAFDoc_ShapeTool::IsAssembly(label) ? "assembly" : "shape";
    node.path = path;
    nodes->push_back(node);

    if (XCAFDoc_ShapeTool::IsAssembly(label)) {
        NCollection_Sequence<TDF_Label> components;
        XCAFDoc_ShapeTool::GetComponents(label, components);
        for (NCollection_Sequence<TDF_Label>::Iterator it(components); it.More(); it.Next()) {
            const TDF_Label& component = it.Value();
            TDF_Label referred;
            if (XCAFDoc_ShapeTool::GetReferredShape(component, referred)
                && XCAFDoc_ShapeTool::IsAssembly(referred)) {
                // Recurse into the prototype assembly to expose its
                // structure, but keep the *component* label as the node
                // identity so two instances stay distinct.
                const std::string saved_entry = label_entry(component);
                const std::size_t before = nodes->size();
                collect_assembly_node(shape_tool, referred, id, path,
                                      next_node_index, nodes);
                if (before < nodes->size()) {
                    (*nodes)[before].label_entry = saved_entry;
                }
            } else {
                collect_assembly_node(shape_tool, component, id, path,
                                      next_node_index, nodes);
            }
        }
    }
}

// Geometric identity used as a soft-match key on replay. Rounded values
// are caller's concern; the raw doubles are stored and rounded only at
// comparison time. See [[project-priorities]] and the
// DECISION_LOG_AND_RECIPE doc for the rationale.
ShapeFingerprint compute_fingerprint(const TopoDS_Shape& solid) {
    ShapeFingerprint fp;

    GProp_GProps vol_props;
    BRepGProp::VolumeProperties(solid, vol_props);
    fp.volume = std::abs(vol_props.Mass());

    GProp_GProps area_props;
    BRepGProp::SurfaceProperties(solid, area_props);
    fp.surface_area = std::abs(area_props.Mass());

    const gp_Pnt c = vol_props.CentreOfMass();
    fp.centroid_x = c.X();
    fp.centroid_y = c.Y();
    fp.centroid_z = c.Z();

    Bnd_Box box;
    BRepBndLib::Add(solid, box);
    if (!box.IsVoid()) {
        double xmin, ymin, zmin, xmax, ymax, zmax;
        box.Get(xmin, ymin, zmin, xmax, ymax, zmax);
        const double dx = xmax - xmin;
        const double dy = ymax - ymin;
        const double dz = zmax - zmin;
        fp.bbox_diagonal = std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    int face_count = 0;
    for (TopExp_Explorer fexp(solid, TopAbs_FACE); fexp.More(); fexp.Next()) {
        ++face_count;
    }
    fp.face_count = face_count;
    fp.valid = true;
    return fp;
}

// Single explorer-driven pass. For each solid in TopExp order (the engine's
// load-bearing solid:N invariant), look up the owning *component* label via
// XCAF Search so that two instances of the same prototype get distinct
// metadata rows.
void populate_solid_metadata(const Handle(XCAFDoc_ShapeTool)& shape_tool,
                             const TopoDS_Shape& root,
                             std::vector<ShapeMetadata>* metadata) {
    std::size_t index = 0;
    for (TopExp_Explorer exp(root, TopAbs_SOLID); exp.More(); exp.Next()) {
        ++index;
        ShapeMetadata m;
        m.shape_id = "solid:" + std::to_string(index);

        if (!shape_tool.IsNull()) {
            TDF_Label label;
            if (shape_tool->Search(exp.Current(), label,
                                   /*findInstance=*/true,
                                   /*findComponent=*/true,
                                   /*findSubShape=*/true)
                && !label.IsNull()) {
                m.component_entry = label_entry(label);
                m.name = label_name(label);
                m.hierarchy_path = build_path_from_label(label);
            }
        }

        m.fingerprint = compute_fingerprint(exp.Current());

        metadata->push_back(m);
    }
}

struct ImportedShape {
    TopoDS_Shape shape;
    std::vector<AssemblyNode> assembly;
    std::vector<ShapeMetadata> solid_metadata;
};

ImportedShape read_step_shape(const std::string& step_path, ConversionReport& report) {
    STEPCAFControl_Reader reader;
    const IFSelect_ReturnStatus status = reader.ReadFile(step_path.c_str());
    if (status != IFSelect_RetDone) {
        add_diag(report, DiagnosticSeverity::error, "step_read_failed",
                 "OCCT could not read the STEP file: " + step_path);
        return {};
    }

    const int roots = reader.NbRootsForTransfer();
    Handle(TDocStd_Document) doc;
    XCAFApp_Application::GetApplication()->NewDocument("MDTV-XCAF", doc);
    if (roots <= 0 || !reader.Transfer(doc)) {
        add_diag(report, DiagnosticSeverity::error, "step_transfer_failed",
                 "OCCT did not transfer any STEP roots.");
        return {};
    }

    ImportedShape imported;
    imported.shape = reader.Reader().OneShape();

    Handle(XCAFDoc_ShapeTool) shape_tool =
        XCAFDoc_DocumentTool::ShapeTool(doc->Main());

    // Solid metadata is anchored on the TopExp_Explorer order of the root
    // shape — that order defines solid:N throughout the engine. Each solid
    // looks up its own component label, so repeated instances of the same
    // prototype get distinct rows.
    populate_solid_metadata(shape_tool, imported.shape, &imported.solid_metadata);

    if (!shape_tool.IsNull()) {
        NCollection_Sequence<TDF_Label> free_shapes;
        shape_tool->GetFreeShapes(free_shapes);
        std::size_t next_node_index = 1;
        for (NCollection_Sequence<TDF_Label>::Iterator it(free_shapes); it.More(); it.Next()) {
            collect_assembly_node(shape_tool, it.Value(), {}, {}, &next_node_index,
                                  &imported.assembly);
        }

        // Link assembly nodes to solids by component entry — one lookup
        // table, no string-match on path, no traversal-order coincidence.
        std::unordered_map<std::string, std::string> entry_to_shape_id;
        for (const ShapeMetadata& m : imported.solid_metadata) {
            if (!m.component_entry.empty() && !m.shape_id.empty()) {
                entry_to_shape_id.emplace(m.component_entry, m.shape_id);
            }
        }
        for (AssemblyNode& node : imported.assembly) {
            auto it = entry_to_shape_id.find(node.label_entry);
            if (it != entry_to_shape_id.end()) {
                node.shape_id = it->second;
            }
        }
    }
    return imported;
}

ImportedShape load_and_prepare_shape(const std::string& step_path,
                                     const ConversionOptions& options,
                                     ConversionReport& report) {
    ImportedShape imported = read_step_shape(step_path, report);
    if (imported.shape.IsNull()) {
        return imported;
    }

    if (options.heal_shape) {
        Handle(ShapeFix_Shape) fixer = new ShapeFix_Shape(imported.shape);
        fixer->Perform();
        imported.shape = fixer->Shape();
    }

    BRepCheck_Analyzer analyzer(imported.shape);
    if (!analyzer.IsValid()) {
        add_diag(report, DiagnosticSeverity::warning, "invalid_brep",
                 "OCCT reports that the transferred BRep is not fully valid; conversion will continue.");
    }

    return imported;
}

} // namespace

CadModel load_step_model(const std::string& step_path,
                         const ConversionOptions& options) {
    CadModel model;
    model.source_path = step_path;

    ConversionReport load_report;
    ImportedShape imported = load_and_prepare_shape(step_path, options, load_report);
    model.root = imported.shape;
    model.assembly = imported.assembly;
    model.solid_metadata = imported.solid_metadata;
    model.diagnostics = load_report.diagnostics;
    return model;
}

} // namespace cad2csg

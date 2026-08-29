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
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <gp_Trsf.hxx>
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
#include <UnitsMethods_LengthUnit.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_LengthUnit.hxx>
#include <XCAFDoc_ShapeTool.hxx>

namespace csgify {
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
    // Length unit captured from the XCAF document (or via the
    // XCAFDoc_LengthUnit attribute if the document carries one). Empty
    // name + 1.0 factor means "could not determine; assume native".
    std::string length_unit;
    double length_unit_to_mm = 1.0;
};

// Map the meter-scale factor returned by XCAFDoc_DocumentTool::GetLengthUnit
// to a short unit label (the value is the size of one internal unit in
// metres). Returns empty string if no exact match; in that case we still
// store the numeric factor so downstream code can reason about it.
const char* unit_name_from_meter_factor(double f) {
    constexpr double eps = 1.0e-9;
    auto close = [eps](double a, double b) {
        return std::abs(a - b) <= eps + eps * std::abs(b);
    };
    if (close(f, 1.0))            return "m";
    if (close(f, 0.001))          return "mm";
    if (close(f, 0.01))           return "cm";
    if (close(f, 1000.0))         return "km";
    if (close(f, 1.0e-6))         return "micron";
    if (close(f, 0.0254))         return "in";
    if (close(f, 0.3048))         return "ft";
    if (close(f, 1609.344))       return "mile";
    if (close(f, 2.54e-5))        return "mil";
    if (close(f, 2.54e-8))        return "microinch";
    return "";
}

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

    // STEP length unit. Prefer the XCAFDoc_LengthUnit attribute if the
    // file carries one (it has both a name and a scale); otherwise fall
    // back to GetLengthUnit, which returns the meter-scale factor. If
    // neither is available, leave the fields empty and let downstream
    // code surface the ambiguity as a diagnostic.
    Handle(XCAFDoc_LengthUnit) length_unit_attr;
    if (doc->Main().FindAttribute(XCAFDoc_LengthUnit::GetID(), length_unit_attr)
        && !length_unit_attr.IsNull() && !length_unit_attr->IsEmpty()) {
        imported.length_unit = length_unit_attr->GetUnitName().ToCString();
        // GetUnitValue() returns the scale to metres; convert to mm.
        imported.length_unit_to_mm = length_unit_attr->GetUnitValue() * 1000.0;
    } else {
        double meters = 0.0;
        if (XCAFDoc_DocumentTool::GetLengthUnit(doc, meters) && meters > 0.0) {
            const char* name = unit_name_from_meter_factor(meters);
            if (name && name[0]) {
                imported.length_unit = name;
            }
            imported.length_unit_to_mm = meters * 1000.0;
        }
    }

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

namespace {
// Map from short unit label to the scale factor that converts a value
// in that unit to millimetres. Used by --units / --normalize-units.
double scale_to_mm_from_name(const std::string& name) {
    if (name == "mm") return 1.0;
    if (name == "cm") return 10.0;
    if (name == "m")  return 1000.0;
    if (name == "km") return 1000000.0;
    if (name == "micron") return 0.001;
    if (name == "in") return 25.4;
    if (name == "ft") return 304.8;
    if (name == "mile") return 1609344.0;
    if (name == "mil") return 0.0254;
    if (name == "microinch") return 0.0000254;
    return 0.0;
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
    model.length_unit = imported.length_unit;
    model.length_unit_to_mm = imported.length_unit_to_mm;
    model.diagnostics = load_report.diagnostics;

    // --units NAME overrides the asserted unit name (no geometry change).
    // --scale X overrides the scale-to-mm factor (no geometry change).
    // Both are recorded as info diagnostics so the override is visible
    // in the report and decision log.
    if (!options.unit_override.empty()) {
        const double known = scale_to_mm_from_name(options.unit_override);
        const std::string before = model.length_unit;
        model.length_unit = options.unit_override;
        if (known > 0.0) {
            model.length_unit_to_mm = known;
        }
        model.diagnostics.push_back(
            {DiagnosticSeverity::info, "unit_override",
             "Length unit overridden by --units '" + options.unit_override
                 + "' (was '" + (before.empty() ? "unknown" : before) + "').",
             {}, {}, {}, {}});
    }
    if (options.unit_scale_to_mm_override > 0.0) {
        const double before = model.length_unit_to_mm;
        model.length_unit_to_mm = options.unit_scale_to_mm_override;
        std::ostringstream msg;
        msg << "Length-to-mm scale overridden by --scale "
            << options.unit_scale_to_mm_override
            << " (was " << before << ").";
        model.diagnostics.push_back(
            {DiagnosticSeverity::info, "unit_scale_override", msg.str(),
             {}, {}, {}, {}});
    }

    // --normalize-units NAME actually rescales geometry. This is a
    // mutating operation and is logged at info severity. The factor
    // applied is (current model unit -> target unit), so a model in
    // cm normalized to mm gets a 10x scale, and the report afterwards
    // claims the target unit.
    if (!options.normalize_to_unit.empty() && !model.root.IsNull()) {
        const double target_to_mm = scale_to_mm_from_name(options.normalize_to_unit);
        if (target_to_mm <= 0.0) {
            model.diagnostics.push_back(
                {DiagnosticSeverity::error, "unit_normalize_unknown_target",
                 "--normalize-units '" + options.normalize_to_unit
                     + "' is not a recognized unit name.",
                 {}, {}, {}, {}});
        } else if (model.length_unit_to_mm > 0.0) {
            const double factor = model.length_unit_to_mm / target_to_mm;
            if (std::abs(factor - 1.0) > 1.0e-12) {
                gp_Trsf trsf;
                trsf.SetScaleFactor(factor);
                BRepBuilderAPI_Transform xform(model.root, trsf, /*copy=*/true);
                if (xform.IsDone()) {
                    model.root = xform.Shape();
                    std::ostringstream msg;
                    msg << "Geometry rescaled from "
                        << (model.length_unit.empty() ? "unknown"
                                                      : model.length_unit)
                        << " to " << options.normalize_to_unit
                        << " (factor=" << factor << ").";
                    model.diagnostics.push_back(
                        {DiagnosticSeverity::info, "unit_normalized",
                         msg.str(), {}, {}, {}, {}});
                    model.length_unit = options.normalize_to_unit;
                    model.length_unit_to_mm = target_to_mm;
                } else {
                    model.diagnostics.push_back(
                        {DiagnosticSeverity::error, "unit_normalize_failed",
                         "BRepBuilderAPI_Transform did not complete the "
                         "rescale; geometry left untouched.",
                         {}, {}, {}, {}});
                }
            }
        }
    }

    if (model.length_unit.empty() && !model.root.IsNull()) {
        model.diagnostics.push_back(
            {DiagnosticSeverity::warning, "step_unit_unknown",
             "STEP file did not declare a length unit; values are reported "
             "in OCCT's native units (mm). Use --units or --normalize-units "
             "to override.",
             {}, {}, {}, {}});
    }
    return model;
}

} // namespace csgify

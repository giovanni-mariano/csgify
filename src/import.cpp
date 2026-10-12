// STEP import. Reads a file through OCCT's STEP reader, optionally runs
// ShapeFix, and packages the result as a CadModel ready for the analyze
// and emit stages. Pure import — no Alea, no traversal of solids.

#include "engine_model.hpp"
#include "fnv.hpp"
#include "occt_document_guard.hpp"

#include <cmath>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepTools.hxx>
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
#include <TopLoc_Location.hxx>
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

std::string semantic_id(const char* prefix, const std::string& value) {
    return std::string(prefix) + fnv1a128_hex(fnv1a128(value.data(), value.size()));
}

InstanceTransform instance_transform(const TDF_Label& label) {
    InstanceTransform result;
    const gp_Trsf transform = XCAFDoc_ShapeTool::GetLocation(label).Transformation();
    std::size_t offset = 0;
    for (int row = 1; row <= 3; ++row) {
        for (int column = 1; column <= 4; ++column) {
            result.values[offset++] = transform.Value(row, column);
        }
    }
    return result;
}

std::string local_geometry_digest(const TDF_Label& definition_label) {
    TopoDS_Shape shape = XCAFDoc_ShapeTool::GetShape(definition_label);
    if (shape.IsNull()) {
        return {};
    }
    // Prototype geometry is hashed in local coordinates and without mesh data,
    // so occurrence placement and triangulation do not invalidate reuse.
    shape.Location(TopLoc_Location());
    std::ostringstream serialized;
    BRepTools::Write(shape, serialized,
                     /*theWithTriangles=*/false,
                     /*theWithNormals=*/false,
                     TopTools_FormatVersion_CURRENT);
    if (!serialized) {
        return {};
    }
    const std::string bytes = serialized.str();
    return fnv1a128_hex(fnv1a128(bytes.data(), bytes.size()));
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
    TDF_Label referred_definition;
    if (XCAFDoc_ShapeTool::GetReferredShape(label, referred_definition)
        && !referred_definition.IsNull()) {
        node.definition_entry = label_entry(referred_definition);
        node.geometry_digest = local_geometry_digest(referred_definition);
    } else {
        node.definition_entry = node.label_entry;
        node.geometry_digest = local_geometry_digest(label);
    }
    node.geometry_digest_verified = !node.geometry_digest.empty();
    node.definition_id = semantic_id("def:", node.definition_entry);
    node.instance_id = semantic_id(
        "inst:", parent_id + "|" + node.label_entry + "|" + node.path);
    node.transform = instance_transform(label);
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
                    (*nodes)[before].instance_id = semantic_id(
                        "inst:", id + "|" + saved_entry + "|"
                            + (*nodes)[before].path);
                    (*nodes)[before].transform = instance_transform(component);
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
                // Search may return a definition subshape label for a solid
                // nested inside a multi-solid occurrence. Assembly semantic
                // nodes, however, are keyed by the owning component label.
                // Walk to that occurrence so every local solid can be joined
                // to its definition and share recognition/certification with
                // rigidly transformed repetitions.
                TDF_Label owner = label;
                while (!owner.IsNull() && owner.Depth() > 0
                       && !XCAFDoc_ShapeTool::IsComponent(owner)) {
                    owner = owner.Father();
                }
                if (owner.IsNull() || owner.Depth() == 0) owner = label;
                m.component_entry = label_entry(owner);
                m.name = label_name(owner);
                m.hierarchy_path = build_path_from_label(owner);
            }
        }

        m.fingerprint = compute_fingerprint(exp.Current());

        metadata->push_back(m);
    }
}

struct ImportedShape {
    TopoDS_Shape shape;
    std::string source_digest;
    std::vector<AssemblyNode> assembly;
    std::vector<ShapeMetadata> solid_metadata;
    StringTable strings;
    std::vector<EntityMetadata> metadata;
    std::vector<ComponentDefinition> component_definitions;
    std::vector<ComponentInstance> component_instances;
    // Length unit captured from the XCAF document (or via the
    // XCAFDoc_LengthUnit attribute if the document carries one). Empty
    // name + 1.0 factor means "could not determine; assume native".
    std::string length_unit;
    double length_unit_to_mm = 1.0;
};

void populate_semantic_model(ImportedShape* imported) {
    std::unordered_set<std::string> definitions;
    for (AssemblyNode& node : imported->assembly) {
        const std::string source_key = imported->source_digest.empty()
            ? std::string("unknown-source") : imported->source_digest;
        node.definition_id = semantic_id(
            "def:", source_key + "|" + node.definition_entry);
        node.instance_id = semantic_id(
            "inst:", source_key + "|" + node.parent_id + "|"
                + node.label_entry + "|" + node.path);
        EntityMetadata metadata;
        metadata.id = static_cast<MetadataId>(imported->metadata.size() + 1);
        metadata.stable_name = imported->strings.intern(
            node.kind == "shape"
                ? node.instance_id
                : semantic_id("assembly:", source_key + "|" + node.path));
        metadata.display_name = imported->strings.intern(node.name);
        metadata.source_cad_label = imported->strings.intern(node.label_entry);
        metadata.source_component_path = imported->strings.intern(node.path);
        metadata.annotations.push_back({
            imported->strings.intern("source_definition_entry"),
            imported->strings.intern(node.definition_entry),
            AnnotationOrigin::cad,
            MetadataReviewState::informational,
            false,
        });
        metadata.annotations.push_back({
            imported->strings.intern("source_document_digest"),
            imported->strings.intern(imported->source_digest),
            AnnotationOrigin::csgify,
            MetadataReviewState::informational,
            false,
        });
        node.metadata_id = metadata.id;
        imported->metadata.push_back(std::move(metadata));

        if (node.kind != "shape") {
            continue;
        }

        if (definitions.insert(node.definition_id).second) {
            ComponentDefinition definition;
            definition.id = node.definition_id;
            definition.source_definition_key =
                imported->strings.intern(node.definition_entry);
            definition.geometry_digest = node.geometry_digest;
            definition.geometry_digest_verified = node.geometry_digest_verified;
            definition.metadata_id = node.metadata_id;
            imported->component_definitions.push_back(std::move(definition));
        }

        ComponentInstance instance;
        instance.id = node.instance_id;
        instance.definition_id = node.definition_id;
        instance.parent_assembly_id = node.parent_id;
        instance.source_shape_id = node.shape_id;
        instance.transform = node.transform;
        instance.metadata_id = node.metadata_id;
        imported->component_instances.push_back(std::move(instance));
    }
}

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
    const std::string source_digest = fnv1a128_file_hex(step_path);
    STEPCAFControl_Reader reader;
    const IFSelect_ReturnStatus status = reader.ReadFile(step_path.c_str());
    if (status != IFSelect_RetDone) {
        add_diag(report, DiagnosticSeverity::error, "step_read_failed",
                 "OCCT could not read the STEP file: " + step_path);
        return {};
    }

    const int roots = reader.NbRootsForTransfer();
    Handle(TDocStd_Document) doc;
    const Handle(XCAFApp_Application) application =
        XCAFApp_Application::GetApplication();
    application->NewDocument("MDTV-XCAF", doc);
    detail::XcafDocumentGuard document_guard(application, doc);
    if (roots <= 0 || !reader.Transfer(doc)) {
        add_diag(report, DiagnosticSeverity::error, "step_transfer_failed",
                 "OCCT did not transfer any STEP roots.");
        return {};
    }

    ImportedShape imported;
    imported.source_digest = source_digest;
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
        populate_semantic_model(&imported);
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
    model.strings = std::move(imported.strings);
    model.metadata = std::move(imported.metadata);
    model.component_definitions = std::move(imported.component_definitions);
    model.component_instances = std::move(imported.component_instances);
    model.length_unit = imported.length_unit;
    model.length_unit_to_mm = imported.length_unit_to_mm;
    model.diagnostics = load_report.diagnostics;

    MaterialDefinition default_material;
    default_material.id = "material:" + std::to_string(options.default_material_id);
    EntityMetadata material_metadata;
    material_metadata.id = static_cast<MetadataId>(model.metadata.size() + 1);
    material_metadata.stable_name = model.strings.intern(default_material.id);
    material_metadata.display_name = model.strings.intern("default material");
    material_metadata.annotations.push_back({
        model.strings.intern("material_source"),
        model.strings.intern("conversion_default"),
        AnnotationOrigin::csgify,
        MetadataReviewState::informational,
        false,
    });
    model.metadata.push_back(std::move(material_metadata));
    default_material.metadata_id = model.metadata.back().id;
    default_material.phase = MaterialPhase::unknown;
    default_material.density = options.default_density;
    model.materials.push_back(std::move(default_material));

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

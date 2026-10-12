#include "engine_model.hpp"

#include <cassert>
#include <cmath>

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRep_Builder.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

int main() {
    const TopoDS_Shape outer = BRepPrimAPI_MakeCylinder(10.0, 40.0).Shape();
    const TopoDS_Shape inner = BRepPrimAPI_MakeCylinder(6.0, 40.0).Shape();
    const TopoDS_Shape pipe = BRepAlgoAPI_Cut(outer, inner).Shape();

    gp_Trsf translated;
    translated.SetTranslation(gp_Vec(100.0, 20.0, 0.0));
    const TopoDS_Shape second = BRepBuilderAPI_Transform(
        pipe, translated, true).Shape();

    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);
    builder.Add(compound, pipe);
    builder.Add(compound, second);

    csgify::CadModel model;
    model.root = compound;
    csgify::ComponentDefinition definition;
    definition.id = "def:pipe";
    definition.geometry_digest = "verified-pipe-geometry";
    definition.geometry_digest_verified = true;
    model.component_definitions.push_back(definition);

    csgify::ComponentInstance first;
    first.id = "inst:first";
    first.definition_id = definition.id;
    first.source_shape_id = "solid:1";
    model.component_instances.push_back(first);
    csgify::ComponentInstance moved = first;
    moved.id = "inst:moved";
    moved.source_shape_id = "solid:2";
    moved.transform.values[3] = 100.0;
    moved.transform.values[7] = 20.0;
    model.component_instances.push_back(moved);

    csgify::ConversionOptions options;
    options.check_overlaps = false;
    const csgify::ConversionReport report = csgify::analyze_model(model, options);
    assert(report.solids.size() == 2);
    assert(report.solids[0].component_instance_id == first.id);
    assert(report.solids[0].component_definition_id == definition.id);
    assert(report.solids[1].component_instance_id == moved.id);
    assert(report.solids[1].component_definition_id == definition.id);
    assert(report.performance.definition_recognition_cache_misses == 1);
    assert(report.performance.definition_recognition_cache_hits == 1);
    assert(report.solids[0].topology.pipe_annulus.candidate);
    assert(report.solids[1].topology.pipe_annulus.candidate);
    const csgify::Vec3& first_origin =
        report.solids[0].topology.pipe_annulus.axis_origin;
    const csgify::Vec3& moved_origin =
        report.solids[1].topology.pipe_annulus.axis_origin;
    assert(std::abs((moved_origin.x - first_origin.x) - 100.0) < 1.0e-8);
    assert(std::abs((moved_origin.y - first_origin.y) - 20.0) < 1.0e-8);
    assert(std::abs(moved_origin.z - first_origin.z) < 1.0e-8);
    assert(report.solids[0].topology.pipe_annulus.ports.size()
           == report.solids[1].topology.pipe_annulus.ports.size());
    assert(report.solids[0].face_count == report.solids[1].face_count);
    assert(report.solids[1].face_ids.front().rfind("face:2.", 0) == 0);
    assert(std::abs((report.solids[1].bbox.xmin
                     - report.solids[0].bbox.xmin) - 100.0) < 1.0e-8);
    assert(std::abs((report.solids[1].bbox.ymin
                     - report.solids[0].bbox.ymin) - 20.0) < 1.0e-8);
    assert(report.solids[0].certificate.certified
           == report.solids[1].certificate.certified);
    assert(report.faces.size() == 2 * report.solids[0].face_count);
    assert(report.faces[report.solids[0].face_count].id.rfind("face:2.", 0)
           == 0);
    assert(std::abs((report.faces[report.solids[0].face_count].bbox.xmin
                     - report.faces[0].bbox.xmin) - 100.0) < 1.0e-8);

    assert(csgify::decompose_solid_pipe_annulus(model, "solid:1", options));
    assert(csgify::reuse_definition_decomposition(
        model, "solid:1", first.transform, "solid:2", moved.transform,
        options));
    assert(model.cad_boolean_bodies.size() == 2);
    assert(model.cad_boolean_bodies[0].source_solid_id == "solid:1");
    assert(model.cad_boolean_bodies[1].source_solid_id == "solid:2");
    assert(model.cad_boolean_bodies[1].positive_cells.size()
           == model.cad_boolean_bodies[0].positive_cells.size());
    assert(model.cad_boolean_bodies[1].negative_cells.size()
           == model.cad_boolean_bodies[0].negative_cells.size());
    return 0;
}

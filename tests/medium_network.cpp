#include "engine_model.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRep_Builder.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeTorus.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Ax2.hxx>

namespace {

TopoDS_Shape annulus(double z0, double length) {
    const gp_Ax2 axis(gp_Pnt(0.0, 0.0, z0), gp_Dir(0.0, 0.0, 1.0));
    BRepAlgoAPI_Cut cut(
        BRepPrimAPI_MakeCylinder(axis, 10.0, length).Shape(),
        BRepPrimAPI_MakeCylinder(axis, 6.0, length).Shape());
    cut.Build();
    assert(cut.IsDone());
    return cut.Shape();
}

csgify::MediumPortDecision decision(
    const std::string& id,
    csgify::MediumPortDisposition disposition,
    const std::string& peer = {}) {
    csgify::MediumPortDecision result;
    result.port_id = id;
    result.disposition = disposition;
    result.peer_port_id = peer;
    return result;
}

} // namespace

int main() {
    TopoDS_Compound root;
    BRep_Builder builder;
    builder.MakeCompound(root);
    builder.Add(root, annulus(0.0, 40.0));
    builder.Add(root, annulus(40.0, 40.0));

    csgify::CadModel model;
    model.root = root;
    csgify::MaterialDefinition water;
    water.id = "material:7";
    water.phase = csgify::MaterialPhase::liquid;
    water.density = -1.0;
    csgify::MaterialDefinition gas;
    gas.id = "material:8";
    gas.phase = csgify::MaterialPhase::gas;
    gas.density = -0.001;
    model.materials = {water, gas};
    for (int i = 1; i <= 2; ++i) {
        csgify::ComponentInstance instance;
        instance.id = "inst:pipe-" + std::to_string(i);
        instance.source_shape_id = "solid:" + std::to_string(i);
        model.component_instances.push_back(instance);
    }

    const csgify::ConversionOptions options;
    const csgify::ConversionReport report = csgify::analyze_model(model, options);
    assert(report.solids.size() == 2);
    const auto& first_ports = report.solids[0].topology.pipe_annulus.ports;
    const auto& second_ports = report.solids[1].topology.pipe_annulus.ports;
    assert(first_ports.size() == 2 && second_ports.size() == 2);
    std::string first_join, first_end, second_join, second_end;
    for (const auto& port : first_ports) {
        const std::string id = "inst:pipe-1/" + port.id;
        (std::abs(port.center.z - 40.0) < 1.0e-8 ? first_join : first_end) = id;
    }
    for (const auto& port : second_ports) {
        const std::string id = "inst:pipe-2/" + port.id;
        (std::abs(port.center.z - 40.0) < 1.0e-8 ? second_join : second_end) = id;
    }
    assert(!first_join.empty() && !second_join.empty());

    csgify::MediumGenerationRequest full;
    full.id = "medium:connected-loop";
    full.host_component_ids = {"inst:pipe-1", "inst:pipe-2"};
    full.material_id = water.id;
    full.ports = {
        decision(first_end, csgify::MediumPortDisposition::cap_here),
        decision(first_join, csgify::MediumPortDisposition::connect_to_port,
                 second_join),
        decision(second_join, csgify::MediumPortDisposition::connect_to_port,
                 first_join),
        decision(second_end,
                 csgify::MediumPortDisposition::interface_to_external_model),
    };
    const csgify::MediumGenerationProposal connected =
        csgify::propose_straight_pipe_medium(model, full, options);
    assert(connected.ok);
    assert(connected.network.segments.size() == 2);
    assert(connected.network.connection_count == 1);
    assert(connected.network.connected_component_count == 1);
    assert(connected.certificate.network_segment_count == 2);
    assert(connected.certificate.overlap_test_passed);
    assert(csgify::apply_straight_pipe_medium(model, connected, true, options));
    assert(model.cad_boolean_bodies.size() == 1);
    assert(model.cad_boolean_bodies[0].positive_cells.size() == 2);
    assert(model.cad_boolean_bodies[0].material_id == 7);

    csgify::CadModel partial_model;
    partial_model.root = root;
    partial_model.materials = {water, gas};
    partial_model.component_instances = model.component_instances;
    csgify::MediumGenerationRequest partial = full;
    partial.id = "medium:half-full-loop";
    partial.fill_mode = csgify::MediumFillMode::level;
    partial.target_volume_fraction = 0.5;
    partial.complement_material_id = gas.id;
    const csgify::MediumGenerationProposal partitioned =
        csgify::propose_straight_pipe_medium(partial_model, partial, options);
    assert(partitioned.ok);
    assert(partitioned.certificate.phase_coverage_passed);
    assert(partitioned.certificate.phase_coverage_delta
           < partitioned.certificate.generated_volume * 1.0e-8);
    assert(std::abs(partitioned.certificate.liquid_volume
                    / partitioned.certificate.generated_volume - 0.5) < 1.0e-8);
    assert(csgify::apply_straight_pipe_medium(
        partial_model, partitioned, true, options));
    assert(partial_model.cad_boolean_bodies.size() == 2);
    assert(partial_model.cad_boolean_bodies[0].material_id == 7);
    assert(partial_model.cad_boolean_bodies[1].material_id == 8);

    // A genuine curved elbow: the seed identifies the lumen-side toroidal
    // wall. The extractor must stop at the two annular end faces, cap the
    // channel, and partition it into certified emission cells.
    const double pi = std::acos(-1.0);
    BRepAlgoAPI_Cut elbow_cut(
        BRepPrimAPI_MakeTorus(20.0, 5.0, 0.5 * pi).Shape(),
        BRepPrimAPI_MakeTorus(20.0, 3.0, 0.5 * pi).Shape());
    elbow_cut.Build();
    assert(elbow_cut.IsDone());
    csgify::CadModel elbow_model;
    elbow_model.root = elbow_cut.Shape();
    elbow_model.materials = {water};
    csgify::ComponentInstance elbow_instance;
    elbow_instance.id = "inst:elbow";
    elbow_instance.source_shape_id = "solid:1";
    elbow_model.component_instances.push_back(elbow_instance);
    std::string seed_face;
    std::vector<std::string> elbow_ports;
    std::size_t face_index = 0;
    for (TopExp_Explorer it(elbow_model.root, TopAbs_FACE);
         it.More(); it.Next()) {
        ++face_index;
        const TopoDS_Face face = TopoDS::Face(it.Current());
        BRepAdaptor_Surface surface(face, false);
        const std::string id = "face:1." + std::to_string(face_index);
        if (surface.GetType() == GeomAbs_Torus
            && std::abs(surface.Torus().MinorRadius() - 3.0) < 1.0e-8)
            seed_face = id;
        if (surface.GetType() == GeomAbs_Plane) {
            std::size_t wires = 0;
            for (TopExp_Explorer wi(face, TopAbs_WIRE);
                 wi.More(); wi.Next()) ++wires;
            if (wires > 1) elbow_ports.push_back(id);
        }
    }
    assert(!seed_face.empty() && elbow_ports.size() == 2);
    csgify::MediumGenerationRequest elbow_request;
    elbow_request.id = "medium:seeded-elbow";
    elbow_request.host_component_ids = {elbow_instance.id};
    elbow_request.material_id = water.id;
    csgify::GeometricSeed elbow_seed;
    elbow_seed.kind = csgify::GeometricSeedKind::face;
    elbow_seed.entity_id = seed_face;
    elbow_request.seed_points_or_faces.push_back(elbow_seed);
    elbow_request.ports = {
        decision(elbow_ports[0], csgify::MediumPortDisposition::cap_here),
        decision(elbow_ports[1],
                 csgify::MediumPortDisposition::interface_to_external_model),
    };
    const csgify::MediumGenerationProposal elbow =
        csgify::propose_straight_pipe_medium(
            elbow_model, elbow_request, options);
    for (const csgify::Diagnostic& diagnostic : elbow.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    assert(elbow.ok);
    assert(elbow.network.elbow_count == 1);
    assert(elbow.certificate.network_segment_count == 1);
    assert(elbow.certificate.leak_test_passed);
    assert(csgify::apply_straight_pipe_medium(
        elbow_model, elbow, true, options));
    assert(elbow_model.cad_boolean_bodies.size() == 1);
    assert(!elbow_model.cad_boolean_bodies[0].positive_cells.empty());

    const gp_Ax2 main_axis(gp_Pnt(-20.0, 0.0, 0.0),
                           gp_Dir(1.0, 0.0, 0.0));
    const gp_Ax2 branch_axis(gp_Pnt(0.0, 0.0, 0.0),
                             gp_Dir(0.0, 1.0, 0.0));
    BRepAlgoAPI_Fuse outer_tee(
        BRepPrimAPI_MakeCylinder(main_axis, 5.0, 40.0).Shape(),
        BRepPrimAPI_MakeCylinder(branch_axis, 5.0, 20.0).Shape());
    BRepAlgoAPI_Fuse inner_tee(
        BRepPrimAPI_MakeCylinder(main_axis, 3.0, 40.0).Shape(),
        BRepPrimAPI_MakeCylinder(branch_axis, 3.0, 20.0).Shape());
    outer_tee.Build();
    inner_tee.Build();
    assert(outer_tee.IsDone() && inner_tee.IsDone());
    BRepAlgoAPI_Cut tee_cut(outer_tee.Shape(), inner_tee.Shape());
    tee_cut.Build();
    assert(tee_cut.IsDone());
    csgify::CadModel tee_model;
    tee_model.root = tee_cut.Shape();
    tee_model.materials = {water};
    csgify::ComponentInstance tee_instance;
    tee_instance.id = "inst:tee";
    tee_instance.source_shape_id = "solid:1";
    tee_model.component_instances.push_back(tee_instance);
    std::string tee_seed_face;
    std::vector<std::string> tee_ports;
    face_index = 0;
    for (TopExp_Explorer it(tee_model.root, TopAbs_FACE);
         it.More(); it.Next()) {
        ++face_index;
        const TopoDS_Face face = TopoDS::Face(it.Current());
        BRepAdaptor_Surface surface(face, false);
        const std::string id = "face:1." + std::to_string(face_index);
        if (tee_seed_face.empty() && surface.GetType() == GeomAbs_Cylinder
            && std::abs(surface.Cylinder().Radius() - 3.0) < 1.0e-8)
            tee_seed_face = id;
        if (surface.GetType() == GeomAbs_Plane) {
            std::size_t wires = 0;
            for (TopExp_Explorer wi(face, TopAbs_WIRE);
                 wi.More(); wi.Next()) ++wires;
            if (wires > 1) tee_ports.push_back(id);
        }
    }
    assert(!tee_seed_face.empty() && tee_ports.size() == 3);
    csgify::MediumGenerationRequest tee_request;
    tee_request.id = "medium:seeded-tee";
    tee_request.host_component_ids = {tee_instance.id};
    tee_request.material_id = water.id;
    csgify::GeometricSeed tee_seed;
    tee_seed.kind = csgify::GeometricSeedKind::point;
    tee_seed.point = {{0.0, 10.0, 0.0}};
    tee_request.seed_points_or_faces.push_back(tee_seed);
    for (const std::string& port : tee_ports)
        tee_request.ports.push_back(decision(
            port, csgify::MediumPortDisposition::cap_here));
    const csgify::MediumGenerationProposal tee =
        csgify::propose_straight_pipe_medium(tee_model, tee_request, options);
    for (const csgify::Diagnostic& diagnostic : tee.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    assert(tee.ok);
    assert(tee.network.tee_count == 1);
    assert(tee.network.ports.size() == 3);
    assert(tee.certificate.leak_test_passed);
    assert(csgify::apply_straight_pipe_medium(
        tee_model, tee, true, options));
    assert(tee_model.cad_boolean_bodies.size() == 1);
    assert(tee_model.cad_boolean_bodies[0].positive_cells.size() == 2);

    TopoDS_Compound obstructed_root;
    builder.MakeCompound(obstructed_root);
    builder.Add(obstructed_root, annulus(0.0, 40.0));
    const gp_Ax2 obstacle_axis(gp_Pnt(-4.0, 0.0, 20.0),
                               gp_Dir(1.0, 0.0, 0.0));
    builder.Add(obstructed_root,
        BRepPrimAPI_MakeCylinder(obstacle_axis, 1.0, 8.0).Shape());
    csgify::CadModel obstructed_model;
    obstructed_model.root = obstructed_root;
    obstructed_model.materials = {water};
    csgify::ComponentInstance obstructed_pipe;
    obstructed_pipe.id = "inst:obstructed-pipe";
    obstructed_pipe.source_shape_id = "solid:1";
    csgify::ComponentInstance obstacle;
    obstacle.id = "inst:probe";
    obstacle.source_shape_id = "solid:2";
    obstructed_model.component_instances = {obstructed_pipe, obstacle};
    csgify::MediumGenerationRequest obstructed_request;
    obstructed_request.id = "medium:pipe-minus-probe";
    obstructed_request.host_component_ids = {obstructed_pipe.id};
    obstructed_request.material_id = water.id;
    obstructed_request.internal_components_to_subtract = {obstacle.id};
    obstructed_request.ports = {
        decision("port:1", csgify::MediumPortDisposition::cap_here),
        decision("port:2", csgify::MediumPortDisposition::cap_here),
    };
    const csgify::MediumGenerationProposal obstructed =
        csgify::propose_straight_pipe_medium(
            obstructed_model, obstructed_request, options);
    assert(obstructed.ok);
    assert(obstructed.certificate.obstacle_subtraction_passed);
    assert(csgify::apply_straight_pipe_medium(
        obstructed_model, obstructed, true, options));
    assert(obstructed_model.cad_boolean_bodies.size() == 1);
    assert(obstructed_model.cad_boolean_bodies[0].negative_cells.size() == 1);
    assert(obstructed_model.derived_regions[0].subtracted_components.size()
           == 1);

}

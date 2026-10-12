#include "engine_model.hpp"
#include "decompose.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>

#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS.hxx>

namespace {

int fail(const char* message) {
    std::cerr << "decomposed_union: " << message << '\n';
    return EXIT_FAILURE;
}

csgify::DecomposedSolidPiece box_piece(const char* id,
                                       const gp_Pnt& origin,
                                       double dx,
                                       double dy,
                                       double dz) {
    csgify::DecomposedSolidPiece piece;
    piece.id = id;
    piece.label = id;
    piece.source_solid_id = "solid:1";
    piece.solid = TopoDS::Solid(BRepPrimAPI_MakeBox(origin, dx, dy, dz).Shape());
    piece.volume = dx * dy * dz;
    return piece;
}

bool contains_union(const alea_system_t* system, alea_node_id_t node) {
    const alea_operation_t op = alea_node_operation(system, node);
    if (op == ALEA_OP_UNION) return true;
    if (op != ALEA_OP_INTERSECTION && op != ALEA_OP_DIFFERENCE
        && op != ALEA_OP_COMPLEMENT) {
        return false;
    }
    if (contains_union(system, alea_node_left(system, node))) return true;
    const alea_node_id_t right = alea_node_right(system, node);
    return right != ALEA_NODE_ID_INVALID && contains_union(system, right);
}

} // namespace

int main() {
    csgify::ConversionOptions options;
    if (!options.union_decomposed_pieces || !options.split_union_cells) {
        return fail("automatic union/split defaults are not enabled");
    }

    // These are the two halves of one box. Their shared decomposition plane
    // must cancel, leaving one intersection cell.
    csgify::CadModel convex;
    csgify::ConversionReport split_report;
    const TopoDS_Solid whole = TopoDS::Solid(
        BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape());
    convex.root = whole;
    convex.removed_solid_ids.push_back("solid:1");
    const std::vector<TopoDS_Solid> halves = csgify::detail::split_solid_by_plane(
        whole, gp_Pln(gp_Pnt(5.0, 0.0, 0.0), gp_Dir(1.0, 0.0, 0.0)),
        options, split_report, 1, "test-cut");
    if (halves.size() != 2) return fail("box fixture did not split in two");
    for (std::size_t i = 0; i < halves.size(); ++i) {
        csgify::DecomposedSolidPiece piece;
        piece.id = "piece:" + std::to_string(i + 1);
        piece.label = piece.id;
        piece.source_solid_id = "solid:1";
        piece.solid = halves[i];
        piece.volume = 500.0;
        convex.decomposed_pieces.push_back(std::move(piece));
    }
    if (csgify::coalesce_decomposed_pieces(convex, options) != 1) {
        return fail("OCCT pass did not merge the two box halves");
    }
    const std::size_t active_convex = std::count_if(
        convex.decomposed_pieces.begin(), convex.decomposed_pieces.end(),
        [](const csgify::DecomposedSolidPiece& piece) { return piece.active; });
    if (active_convex != 1) {
        return fail("OCCT pass did not leave one active CAD piece");
    }
    csgify::ConversionResult reunited = csgify::convert_model(convex, options);
    if (!reunited.report.ok()) return fail("convex partition did not convert");
    if (alea_cell_count(reunited.system.get()) != 1) {
        return fail("convex partition was not reunited into one cell");
    }
    alea_node_id_t root = ALEA_NODE_ID_INVALID;
    if (alea_cell_get(reunited.system.get(), 0, nullptr, nullptr, nullptr,
                      nullptr, nullptr, &root) != 0
        || alea_node_operation(reunited.system.get(), root) != ALEA_OP_INTERSECTION) {
        return fail("reunited convex partition is not one intersection");
    }

    // This partition is an L shape. It must remain a real union and therefore
    // be split back into two convex MCNP cells after simplification.
    csgify::CadModel nonconvex;
    nonconvex.root = whole;
    nonconvex.removed_solid_ids.push_back("solid:1");
    nonconvex.decomposed_pieces.push_back(
        box_piece("piece:1", gp_Pnt(0.0, 0.0, 0.0), 5.0, 10.0, 10.0));
    nonconvex.decomposed_pieces.push_back(
        box_piece("piece:2", gp_Pnt(5.0, 0.0, 0.0), 5.0, 5.0, 10.0));
    if (csgify::coalesce_decomposed_pieces(nonconvex, options) != 0) {
        return fail("OCCT pass merged a genuine non-convex union");
    }
    csgify::ConversionResult retained = csgify::convert_model(nonconvex, options);
    if (!retained.report.ok()) return fail("non-convex partition did not convert");
    if (alea_cell_count(retained.system.get()) != 1) {
        return fail("non-convex source was not retained as one logical cell");
    }
    root = ALEA_NODE_ID_INVALID;
    if (alea_cell_get(retained.system.get(), 0, nullptr, nullptr, nullptr,
                      nullptr, nullptr, &root) != 0
        || !contains_union(retained.system.get(), root)) {
        return fail("genuine non-convex union was incorrectly made an intersection");
    }

    return EXIT_SUCCESS;
}

// Post-decomposition OCCT coalescing. Adjacent partition fragments are fused
// only when their union is itself one certified convex half-space cell.

#include "engine_model.hpp"
#include "engine_internal.hpp"
#include "coalesce_internal.hpp"
#include "occt_boolean.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <Bnd_Box.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBndLib.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Solid.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>

namespace csgify {
namespace {

using namespace csgify::detail;

double volume_of(const TopoDS_Solid& solid) {
    GProp_GProps props;
    BRepGProp::VolumeProperties(solid, props);
    return std::abs(props.Mass());
}

double rounded(double value, double quantum) {
    return std::round(value / quantum) * quantum;
}

std::string planar_support_signature(const TopoDS_Face& face,
                                     double tolerance) {
    BRepAdaptor_Surface adaptor(face, false);
    if (adaptor.GetType() != GeomAbs_Plane) return {};

    const gp_Pln plane = adaptor.Plane();
    double a = plane.Axis().Direction().X();
    double b = plane.Axis().Direction().Y();
    double c = plane.Axis().Direction().Z();
    double d = -(a * plane.Location().X()
               + b * plane.Location().Y()
               + c * plane.Location().Z());
    if (a < 0.0 || (a == 0.0 && (b < 0.0 || (b == 0.0 && c < 0.0)))) {
        a = -a; b = -b; c = -c; d = -d;
    }

    const double q = std::max(tolerance * 10.0, 1.0e-8);
    std::ostringstream out;
    out << std::setprecision(17)
        << rounded(a, q) << ',' << rounded(b, q) << ','
        << rounded(c, q) << ',' << rounded(d, q);
    return out.str();
}

// A cut creates the same bounded planar face on both adjacent pieces. This
// signature is orientation-independent and intentionally includes the face
// bounds, avoiding the quadratic cross-product of every face on a common wall
// plane. Non-planar faces are not decomposition cuts in the prismatic/BSP path.
std::string planar_face_signature(const TopoDS_Face& face, double tolerance) {
    const std::string support = planar_support_signature(face, tolerance);
    if (support.empty()) return {};

    Bnd_Box bounds;
    BRepBndLib::Add(face, bounds);
    if (bounds.IsVoid() || bounds.IsOpen()) return {};
    double xmin, ymin, zmin, xmax, ymax, zmax;
    bounds.Get(xmin, ymin, zmin, xmax, ymax, zmax);
    GProp_GProps props;
    BRepGProp::SurfaceProperties(face, props);

    const double q = std::max(tolerance * 10.0, 1.0e-8);
    const double area_q = std::max(q * q, 1.0e-12);
    std::ostringstream out;
    out << std::setprecision(17)
        << support << ';'
        << rounded(xmin, q) << ',' << rounded(ymin, q) << ','
        << rounded(zmin, q) << ',' << rounded(xmax, q) << ','
        << rounded(ymax, q) << ',' << rounded(zmax, q) << ';'
        << rounded(std::abs(props.Mass()), area_q);
    return out.str();
}

std::vector<std::string> boundary_signatures(const TopoDS_Solid& solid,
                                             double tolerance) {
    std::vector<std::string> result;
    for (TopExp_Explorer it(solid, TopAbs_FACE); it.More(); it.Next()) {
        std::string signature = planar_face_signature(
            TopoDS::Face(it.Current()), tolerance);
        if (!signature.empty()) result.push_back(std::move(signature));
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

struct PlanarRegion {
    std::string support;
    Bnd_Box bounds;
};

std::vector<PlanarRegion> planar_regions(const TopoDS_Solid& solid,
                                         double tolerance) {
    std::vector<PlanarRegion> result;
    for (TopExp_Explorer it(solid, TopAbs_FACE); it.More(); it.Next()) {
        const TopoDS_Face face = TopoDS::Face(it.Current());
        std::string support = planar_support_signature(face, tolerance);
        if (support.empty()) continue;
        Bnd_Box bounds;
        BRepBndLib::Add(face, bounds);
        if (bounds.IsVoid() || bounds.IsOpen()) continue;
        bounds.SetGap(std::max(bounds.GetGap(), tolerance * 10.0));
        result.push_back({std::move(support), bounds});
    }
    return result;
}

bool certified_convex_fuse(const TopoDS_Solid& left,
                           const TopoDS_Solid& right,
                           const ConversionOptions& options,
                           const std::vector<FaceReplacement>* replacements,
                           TopoDS_Solid* result) {
    TopoDS_Shape shape;
    try {
        BRepAlgoAPI_Fuse fuse;
        detail::build_binary_boolean(
            fuse, left, right, options.linear_tolerance);
        if (!fuse.IsDone()) return false;
        shape = fuse.Shape();
        ShapeUpgrade_UnifySameDomain unify(shape, true, true, true);
        unify.Build();
        if (!unify.Shape().IsNull()) shape = unify.Shape();
    } catch (...) {
        return false;
    }

    TopExp_Explorer solids(shape, TopAbs_SOLID);
    if (!solids.More()) return false;
    const TopoDS_Solid fused = TopoDS::Solid(solids.Current());
    solids.Next();
    if (solids.More() || !BRepCheck_Analyzer(fused).IsValid()) return false;

    const double expected = volume_of(left) + volume_of(right);
    const double actual = volume_of(fused);
    const double relative_delta = std::abs(actual - expected)
        / std::max(expected, 1.0e-30);
    if (relative_delta > options.decomposition_relative_volume_tolerance) {
        return false;
    }

    GProp_GProps props;
    BRepGProp::VolumeProperties(fused, props);
    gp_Pnt interior;
    ConversionReport scratch;
    if (!find_interior_point(fused, props, options.linear_tolerance, &interior)
        || !validate_convex_halfspace_cell(
            fused, interior, 100000, options, scratch, false, false, false,
            replacements)) {
        return false;
    }
    *result = fused;
    return true;
}

struct WorkingPiece {
    TopoDS_Solid solid;
    std::vector<std::string> boundaries;
    std::vector<PlanarRegion> regions;
};

WorkingPiece make_working_piece(const TopoDS_Solid& solid,
                                const ConversionOptions& options) {
    return {solid,
            boundary_signatures(solid, options.linear_tolerance),
            planar_regions(solid, options.linear_tolerance)};
}

std::vector<TopoDS_Solid> coalesce_group(
    const std::vector<const DecomposedSolidPiece*>& input,
    const ConversionOptions& options,
    const std::vector<FaceReplacement>* replacements) {
    std::vector<WorkingPiece> work;
    work.reserve(input.size());
    for (const DecomposedSolidPiece* piece : input) {
        work.push_back(make_working_piece(piece->solid, options));
    }

    while (work.size() > 1) {
        std::map<std::string, std::vector<std::size_t>> by_boundary;
        for (std::size_t i = 0; i < work.size(); ++i) {
            for (const std::string& signature : work[i].boundaries) {
                by_boundary[signature].push_back(i);
            }
        }

        std::vector<std::pair<std::size_t, std::size_t>> candidates;
        std::set<std::pair<std::size_t, std::size_t>> seen;
        auto add_candidate = [&](std::size_t left, std::size_t right) {
            if (left == right) return;
            if (left > right) std::swap(left, right);
            if (seen.emplace(left, right).second) {
                candidates.emplace_back(left, right);
            }
        };

        // Fast path: exactly matching bounded interface faces.
        for (const auto& entry : by_boundary) {
            const std::vector<std::size_t>& adjacent = entry.second;
            if (adjacent.size() < 2) continue;
            for (std::size_t a = 0; a < adjacent.size(); ++a) {
                for (std::size_t b = a + 1; b < adjacent.size(); ++b) {
                    add_candidate(adjacent[a], adjacent[b]);
                }
            }
        }

        // Broad path: one side of an interface can be partitioned into
        // differently sized faces. Overlapping bounds on the same plane are
        // only a candidate; the fuse, volume, and convex certificates decide.
        struct RegionRef {
            std::size_t piece = 0;
            const Bnd_Box* bounds = nullptr;
        };
        std::map<std::string, std::vector<RegionRef>> by_support;
        for (std::size_t i = 0; i < work.size(); ++i) {
            for (const PlanarRegion& region : work[i].regions) {
                by_support[region.support].push_back({i, &region.bounds});
            }
        }
        for (const auto& entry : by_support) {
            const std::vector<RegionRef>& regions = entry.second;
            for (std::size_t a = 0; a < regions.size(); ++a) {
                for (std::size_t b = a + 1; b < regions.size(); ++b) {
                    if (regions[a].piece == regions[b].piece
                        || regions[a].bounds->IsOut(*regions[b].bounds)) {
                        continue;
                    }
                    add_candidate(regions[a].piece, regions[b].piece);
                }
            }
        }

        std::vector<bool> consumed(work.size(), false);
        std::vector<WorkingPiece> next;
        bool progress = false;
        for (const auto& candidate : candidates) {
            const std::size_t left = candidate.first;
            const std::size_t right = candidate.second;
            if (consumed[left] || consumed[right]) continue;
            TopoDS_Solid fused;
            if (!certified_convex_fuse(work[left].solid, work[right].solid,
                                       options, replacements, &fused)) {
                continue;
            }
            consumed[left] = true;
            consumed[right] = true;
            next.push_back(make_working_piece(fused, options));
            progress = true;
        }
        if (!progress) break;
        for (std::size_t i = 0; i < work.size(); ++i) {
            if (!consumed[i]) next.push_back(std::move(work[i]));
        }
        work = std::move(next);
    }

    std::vector<TopoDS_Solid> result;
    result.reserve(work.size());
    for (WorkingPiece& piece : work) result.push_back(std::move(piece.solid));
    return result;
}

} // namespace

namespace detail {

std::vector<TopoDS_Solid> coalesce_local_partition(
    const std::vector<TopoDS_Solid>& pieces,
    const std::string& source_solid_id,
    const ConversionOptions& options) {
    std::vector<DecomposedSolidPiece> records;
    records.reserve(pieces.size());
    std::vector<const DecomposedSolidPiece*> input;
    input.reserve(pieces.size());
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        DecomposedSolidPiece record;
        record.id = "__local_coalesce:" + std::to_string(i + 1);
        record.source_solid_id = source_solid_id;
        record.solid = pieces[i];
        record.volume = volume_of(pieces[i]);
        record.active = true;
        records.push_back(std::move(record));
    }
    for (const DecomposedSolidPiece& record : records)
        input.push_back(&record);
    return coalesce_group(input, options, nullptr);
}

} // namespace detail

std::size_t coalesce_decomposed_pieces(CadModel& model,
                                       const ConversionOptions& options) {
    std::map<std::string, std::vector<std::size_t>> groups;
    for (std::size_t i = 0; i < model.decomposed_pieces.size(); ++i) {
        const DecomposedSolidPiece& piece = model.decomposed_pieces[i];
        if (piece.active) {
            const std::string key = piece.source_solid_id.empty()
                ? piece.id : piece.source_solid_id;
            groups[key].push_back(i);
        }
    }

    std::size_t total_before = 0;
    std::size_t total_after = 0;
    for (const auto& group : groups) {
        if (group.second.size() < 2) continue;
        std::vector<const DecomposedSolidPiece*> input;
        input.reserve(group.second.size());
        for (std::size_t index : group.second) {
            input.push_back(&model.decomposed_pieces[index]);
        }
        const std::vector<TopoDS_Solid> merged = coalesce_group(
            input, options, &model.face_replacements);
        if (merged.size() >= group.second.size()) continue;

        total_before += group.second.size();
        total_after += merged.size();
        for (std::size_t index : group.second) {
            model.decomposed_pieces[index].active = false;
        }
        for (const TopoDS_Solid& solid : merged) {
            DecomposedSolidPiece piece;
            piece.id = "piece:" + std::to_string(model.decomposed_pieces.size() + 1);
            piece.label = piece.id + "/coalesced_from_" + group.first;
            piece.source_solid_id = group.first;
            piece.solid = solid;
            piece.volume = volume_of(solid);
            piece.active = true;
            model.decomposed_pieces.push_back(std::move(piece));
        }
    }

    if (total_before > total_after) {
        std::ostringstream message;
        message << "OCCT coalescing reduced " << total_before
                << " decomposed pieces to " << total_after
                << " maximal certified convex pieces.";
        model.diagnostics.push_back({DiagnosticSeverity::info,
            "decomposition_pieces_coalesced", message.str(), {}});
    }
    return total_before - total_after;
}

} // namespace csgify

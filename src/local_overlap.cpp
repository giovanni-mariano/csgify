#include "local_overlap.hpp"

#include "engine_internal.hpp"
#include "occt_boolean.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>
#include <utility>

#include <BRepAlgoAPI_Common.hxx>
#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Shape.hxx>

namespace csgify::detail {
namespace {

struct ActiveEntry {
    std::string id;
    TopoDS_Shape shape;
    Bnd_Box bbox;
    double volume = 0.0;
};

double volume_of(const TopoDS_Shape& shape) {
    if (shape.IsNull()) return 0.0;
    GProp_GProps properties;
    BRepGProp::VolumeProperties(shape, properties);
    return std::abs(properties.Mass());
}

bool intersects(const Bnd_Box& left, const Bnd_Box& right, double pad) {
    if (left.IsVoid() || right.IsVoid()) return false;
    double lx0, ly0, lz0, lx1, ly1, lz1;
    double rx0, ry0, rz0, rx1, ry1, rz1;
    left.Get(lx0, ly0, lz0, lx1, ly1, lz1);
    right.Get(rx0, ry0, rz0, rx1, ry1, rz1);
    return !(lx1 + pad < rx0 || rx1 + pad < lx0
          || ly1 + pad < ry0 || ry1 + pad < ly0
          || lz1 + pad < rz0 || rz1 + pad < lz0);
}

std::pair<std::string, std::string> pair_key(const std::string& left,
                                             const std::string& right) {
    return left < right ? std::make_pair(left, right)
                        : std::make_pair(right, left);
}

} // namespace

LocalOverlapCertificate certify_local_neighbour_overlaps(
    const CadModel& model,
    const std::string& target_id,
    const std::vector<TopoDS_Solid>& children,
    const std::vector<SolidOverlap>& baseline_overlaps,
    const ConversionOptions& options,
    PerformanceMetrics* performance,
    bool children_certified_contained) {
    const auto started = std::chrono::steady_clock::now();
    LocalOverlapCertificate result;
    auto finish = [&]() {
        if (performance) {
            performance->local_overlap_candidates += result.spatial_candidates;
            performance->local_exact_overlap_tests += result.exact_tests;
            if (result.containment_proof_used)
                ++performance->local_overlap_containment_proofs;
            performance->local_overlap_ms +=
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - started).count();
        }
        return result;
    };
    if (!options.check_overlaps) {
        result.safe = true;
        return finish();
    }
    if (target_id.empty() || children.empty()) {
        result.failure_code = "invalid_local_overlap_input";
        return finish();
    }

    std::set<std::pair<std::string, std::string>> inherited_pairs;
    for (const SolidOverlap& overlap : baseline_overlaps) {
        inherited_pairs.insert(pair_key(overlap.solid_a_id,
                                        overlap.solid_b_id));
    }
    if (children_certified_contained) {
        // The exact local partition certificate proves every child is a
        // subset of the target. Set inclusion is sufficient for all external
        // neighbours; rebuilding every active body's bbox and volume would
        // add O(trials * assembly-size) OCCT work without strengthening that
        // proof. Count inherited parent relationships as evidence and let the
        // mandatory terminal reconciliation rebuild fresh child pair counts.
        result.inherited_overlap_hits = std::count_if(
            inherited_pairs.begin(), inherited_pairs.end(),
            [&](const auto& pair) {
                return pair.first == target_id || pair.second == target_id;
            });
        result.containment_proof_used = true;
        result.safe = true;
        return finish();
    }
    std::vector<ActiveEntry> entries;
    std::size_t solid_index = 0;
    for (TopExp_Explorer exp(model.root, TopAbs_SOLID); exp.More(); exp.Next()) {
        ++solid_index;
        const std::string id = solid_id(solid_index);
        if (id == target_id || contains_id(model.removed_solid_ids, id)) continue;
        ActiveEntry entry;
        entry.id = id;
        entry.shape = exp.Current();
        BRepBndLib::Add(entry.shape, entry.bbox);
        entry.volume = volume_of(entry.shape);
        entries.push_back(std::move(entry));
    }
    for (const DecomposedSolidPiece& piece : model.decomposed_pieces) {
        if (!piece.active || piece.id == target_id || piece.solid.IsNull())
            continue;
        ActiveEntry entry;
        entry.id = piece.id;
        entry.shape = piece.solid;
        BRepBndLib::Add(entry.shape, entry.bbox);
        entry.volume = volume_of(entry.shape);
        entries.push_back(std::move(entry));
    }
    for (const CadBooleanBody& body : model.cad_boolean_bodies) {
        if (!body.active || body.id == target_id
            || body.materialized_solid.IsNull()) continue;
        ActiveEntry entry;
        entry.id = body.id;
        entry.shape = body.materialized_solid;
        BRepBndLib::Add(entry.shape, entry.bbox);
        entry.volume = volume_of(entry.shape);
        entries.push_back(std::move(entry));
    }
    for (const SyntheticBody& body : model.synthetic_bodies) {
        if (!body.emit_as_cell || body.id == target_id) continue;
        TopoDS_Shape shape = build_synthetic_shape(model, body);
        if (shape.IsNull()) continue;
        ActiveEntry entry;
        entry.id = body.id;
        entry.shape = shape;
        BRepBndLib::Add(entry.shape, entry.bbox);
        entry.volume = volume_of(entry.shape);
        entries.push_back(std::move(entry));
    }
    std::sort(entries.begin(), entries.end(),
              [](const ActiveEntry& left, const ActiveEntry& right) {
                  return left.id < right.id;
              });

    const double pad = std::max(options.linear_tolerance, 1.0e-9);
    for (const TopoDS_Solid& child : children) {
        Bnd_Box child_bbox;
        BRepBndLib::Add(child, child_bbox);
        const double child_volume = volume_of(child);
        for (const ActiveEntry& entry : entries) {
            if (!intersects(child_bbox, entry.bbox, pad)) continue;
            ++result.spatial_candidates;
            ++result.exact_tests;
            double intersection_volume = 0.0;
            try {
                BRepAlgoAPI_Common common;
                build_binary_boolean(common, child, entry.shape);
                if (!common.IsDone()) {
                    result.unknown = true;
                    result.failure_code = "local_overlap_unknown";
                    result.conflicting_body_id = entry.id;
                    return finish();
                }
                intersection_volume = volume_of(common.Shape());
            } catch (...) {
                result.unknown = true;
                result.failure_code = "local_overlap_unknown";
                result.conflicting_body_id = entry.id;
                return finish();
            }
            const double smaller = std::min(child_volume, entry.volume);
            const double fraction = smaller > 0.0
                ? intersection_volume / smaller : 0.0;
            if (intersection_volume <= options.overlap_volume_threshold
                && fraction <= options.overlap_volume_fraction) {
                continue;
            }
            if (inherited_pairs.count(pair_key(target_id, entry.id)) != 0) {
                ++result.inherited_overlap_hits;
                continue;
            }
            ++result.new_overlap_count;
            result.failure_code = "local_partition_introduced_overlap";
            result.conflicting_body_id = entry.id;
            result.intersection_volume = intersection_volume;
            return finish();
        }
    }
    result.safe = true;
    return finish();
}

} // namespace csgify::detail

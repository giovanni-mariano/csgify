#include "engine_internal.hpp"
#include "engine_model.hpp"

#include <cassert>

#include <BRep_Builder.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS_Compound.hxx>

int main() {
    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);
    for (int index = 0; index < 50; ++index) {
        builder.Add(compound, BRepPrimAPI_MakeBox(
            gp_Pnt(index * 3.0, 0.0, 0.0), 1.0, 1.0, 1.0).Shape());
    }
    builder.Add(compound,
                BRepPrimAPI_MakeBox(gp_Pnt(0.5, 0.0, 0.0),
                                    1.0, 1.0, 1.0).Shape());

    csgify::CadModel model;
    model.root = compound;
    csgify::ConversionOptions options;
    options.overlap_volume_threshold = 1.0e-12;
    options.overlap_volume_fraction = 1.0e-12;
    csgify::ConversionReport report = csgify::analyze_model(model, options);
    csgify::detail::OverlapPairCache cache;
    csgify::detail::detect_solid_overlaps(model, options, report, nullptr,
                                          &cache);

    assert(report.solids_seen == 51);
    assert(report.overlaps.size() == 1);
    assert(report.performance.spatial_candidate_pairs == 1);
    assert(report.performance.exact_overlap_tests == 1);
    assert(report.performance.overlap_pair_cache_misses == 1);

    csgify::ConversionReport repeated = csgify::analyze_model(model, options);
    csgify::detail::OverlapPairCache repeated_cache;
    csgify::detail::detect_solid_overlaps(model, options, repeated, &cache,
                                          &repeated_cache);
    assert(repeated.overlaps.size() == 1);
    assert(repeated.performance.spatial_candidate_pairs == 1);
    assert(repeated.performance.exact_overlap_tests == 0);
    assert(repeated.performance.overlap_pair_cache_hits == 1);
    assert(repeated_cache.size() == cache.size());
    return 0;
}

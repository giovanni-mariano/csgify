#include "csgify/project_store.hpp"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <string>

#include <unistd.h>

namespace {

csgify::SpatialEntry entry(const char* id,
                            const char* kind,
                            double xmin,
                            double xmax) {
    csgify::SpatialEntry result;
    result.entity_id = id;
    result.owner_id = std::string("owner:") + id;
    result.kind = kind;
    result.bounds = {xmin, 0.0, 0.0, xmax, 1.0, 1.0, true};
    return result;
}

bool has_pair(const std::vector<csgify::SpatialCandidatePair>& pairs,
              const char* first,
              const char* second) {
    return std::find(pairs.begin(), pairs.end(),
                     csgify::SpatialCandidatePair{first, second}) != pairs.end();
}

} // namespace

int main() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path()
        / ("csgify-spatial-index-test-" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::string error;

    csgify::ProjectStore project(root);
    assert(project.open(&error));
    assert(project.schema_version(&error) == 8);
    const std::vector<csgify::SpatialEntry> entries = {
        entry("solid:a", "instance_solid", 0.0, 1.0),
        entry("solid:b", "instance_solid", 0.5, 2.0),
        entry("solid:c", "instance_solid", 2.1, 3.0),
        entry("port:a", "pipe_port", 0.0, 1.0),
    };
    assert(project.replace_spatial_entries("model:1", entries, &error));
    assert(project.counts(&error).spatial_entities == 4);
    const auto namespaces = project.list_spatial_namespaces(&error);
    assert(namespaces.size() == 1 && namespaces[0] == "model:1");
    const auto listed = project.list_spatial_entries(
        "model:1", 2, 0, "instance_solid", "solid", &error);
    assert(listed.size() == 2);
    assert(listed[0].bounds.valid);
    assert(project.list_spatial_entries(
        "model:1", 10, 0, {}, "port:a", &error).size() == 1);

    csgify::SpatialBounds probe{0.75, 0.25, 0.25, 0.8, 0.75, 0.75, true};
    auto hits = project.query_spatial_entries(
        "model:1", probe, "instance_solid", 0.0, &error);
    assert(hits.size() == 2);
    assert(hits[0].entity_id == "solid:a");
    assert(hits[1].entity_id == "solid:b");

    auto pairs = project.spatial_overlap_candidates(
        "model:1", "instance_solid", 0.0, 0, &error);
    assert(pairs.size() == 1);
    assert(has_pair(pairs, "solid:a", "solid:b"));

    pairs = project.spatial_overlap_candidates(
        "model:1", "instance_solid", 0.11, 0, &error);
    assert(pairs.size() == 2);
    assert(has_pair(pairs, "solid:a", "solid:b"));
    assert(has_pair(pairs, "solid:b", "solid:c"));
    assert(project.spatial_overlap_candidates(
        "model:1", "instance_solid", 0.11, 1, &error).size() == 1);

    csgify::SpatialEntry invalid = entry("bad", "instance_solid", 2.0, 1.0);
    assert(!project.replace_spatial_entries("model:1", {invalid}, &error));
    assert(project.counts(&error).spatial_entities == 4);

    assert(project.replace_spatial_entries(
        "model:1", {entry("solid:c", "instance_solid", 2.1, 3.0)}, &error));
    assert(project.counts(&error).spatial_entities == 1);
    assert(project.spatial_overlap_candidates(
        "model:1", "instance_solid", 0.0, 0, &error).empty());

    std::filesystem::remove_all(root, ec);
    return 0;
}

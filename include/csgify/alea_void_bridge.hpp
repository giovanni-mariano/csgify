#pragma once

#include "csgify/alea_bridge.hpp"

#include <alea.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace csgify {

inline constexpr const char* kAleaCoverageSchema =
    "csgify.alea-coverage-certificate";
inline constexpr std::size_t kAleaCoverageSchemaMajor = 1;
inline constexpr std::size_t kAleaCoverageSchemaMinor = 1;

struct AleaVoidDomain {
    std::string domain_id = "domain:modeled";
    BBox bounds;
};

struct AleaVoidPolicy {
    int max_depth = 8;
    double min_size = 0.0;
    int probes_per_axis = 3;
    std::size_t requested_workers = 0;
    std::uint64_t max_parallel_scratch_bytes = 0;
    std::size_t parallel_frontier_depth = 0;
    bool merge = true;
    bool add_graveyard = true;
    std::size_t coverage_samples_per_axis = 7;
    std::size_t max_coverage_hits = 256;
};

struct AleaVoidExecutionEvidence {
    std::size_t requested_workers = 0;
    std::size_t actual_workers = 0;
    std::size_t frontier_task_count = 0;
    std::size_t parallel_batch_count = 0;
    std::uint64_t scratch_bytes_per_worker = 0;
    std::uint64_t reserved_parallel_scratch_bytes = 0;
};

struct AleaCoverageEvidence {
    // Each retained point is classified with libalea's complete occurrence
    // chain. The lattice supplements the structural complement invariant; it
    // is never presented as an exhaustive proof of the continuous domain.
    std::string method = "deterministic_cell_center_lattice";
    std::size_t samples_per_axis = 0;
    std::uint64_t sample_count = 0;
    std::uint64_t unique_count = 0;
    std::uint64_t gap_count = 0;
    std::uint64_t overlap_count = 0;
    std::uint64_t undefined_fill_count = 0;
    std::uint64_t unresolved_count = 0;
    std::uint64_t truncated_count = 0;
};

struct AleaSourceOverlapEvidence {
    std::string component_a;
    std::string component_b;
    double intersection_volume = 0.0;
    double minimum_component_fraction = 0.0;
};

enum class AleaCoverageCertificateStatus {
    certified,
    blocked_overlap,
    blocked_gap,
    blocked_undefined_fill,
    unresolved,
    failed,
};

struct AleaCoverageCertificate {
    std::string schema = kAleaCoverageSchema;
    std::size_t schema_major = kAleaCoverageSchemaMajor;
    std::size_t schema_minor = kAleaCoverageSchemaMinor;
    std::string producer = "csgify";
    std::string producer_version;
    std::string libalea_version;
    std::string handoff_contract_digest;
    std::string domain_digest;
    std::string policy_digest;
    std::string certificate_digest;
    AleaCoverageCertificateStatus status =
        AleaCoverageCertificateStatus::failed;
    AleaVoidDomain domain;
    std::size_t material_cell_count = 0;
    std::size_t intentional_void_cell_count = 0;
    std::size_t graveyard_cell_count = 0;
    std::size_t fill_instance_count = 0;
    std::size_t reused_universe_count = 0;
    std::size_t estimated_duplicated_cells_avoided = 0;
    std::size_t source_overlap_count = 0;
    std::size_t source_overlap_unknown_count = 0;
    std::vector<AleaSourceOverlapEvidence> source_overlaps;
    std::size_t exact_checks_performed = 0;
    bool structural_exact_complement = false;
    bool committed = false;
    bool merged = false;
    AleaVoidExecutionEvidence execution;
    AleaCoverageEvidence coverage;
    std::vector<int> void_cell_ids;
    std::vector<int> graveyard_cell_ids;
    std::vector<std::string> limitations;
    std::vector<Diagnostic> diagnostics;
};

struct AleaCoverageReviewItem {
    std::string id;
    std::string category;
    std::string severity;
    std::uint64_t count = 0;
    std::string summary;
    std::string recommended_action;
    bool blocking = false;
    bool auto_action_allowed = false;
    std::vector<std::string> target_ids;
};

const char* alea_coverage_status_name(AleaCoverageCertificateStatus status);

std::string alea_void_domain_digest(const AleaVoidDomain& domain);
std::string alea_void_policy_digest(const AleaVoidPolicy& policy);

// Build exact analytical void cells inside a finite modeled domain using
// libalea. Geometry mutations remain transactional until libalea commits the
// complete generated cell set. A blocking certificate is still returned when
// source overlaps or complete-point diagnostics prevent safe MCNP ownership.
bool generate_alea_domain_voids(alea_system_t* system,
                                const ConversionReport& source_report,
                                const AleaHandoffManifest& handoff,
                                const AleaVoidDomain& domain,
                                const AleaVoidPolicy& policy,
                                AleaCoverageCertificate* certificate,
                                std::string* error = nullptr);

bool validate_alea_coverage_certificate(
    const AleaCoverageCertificate& certificate,
    std::string* error = nullptr);

std::string serialize_alea_coverage_certificate(
    const AleaCoverageCertificate& certificate);

AleaCoverageCertificate parse_alea_coverage_certificate_json(
    const std::string& json);
AleaCoverageCertificate read_alea_coverage_certificate_json(
    const std::string& path);

std::vector<AleaCoverageReviewItem> alea_coverage_review_items(
    const AleaCoverageCertificate& certificate);

bool publish_alea_coverage_certificate(
    const ArtifactStore& store,
    AleaCoverageCertificate certificate,
    ArtifactRecord* artifact,
    std::string* error = nullptr);

// Deterministic default modeled domain: union of analyzed solid bounds plus a
// fractional diagonal margin. Explicit domains should be preferred for
// production transport models.
AleaVoidDomain default_alea_void_domain(const ConversionReport& report,
                                        double margin_fraction = 0.05);

AleaVoidDomain default_alea_void_domain(const AleaHandoffManifest& handoff,
                                        double margin_fraction = 0.05);

} // namespace csgify

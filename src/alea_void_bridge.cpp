#include "csgify/alea_void_bridge.hpp"

#include "fnv.hpp"
#include "json_parser.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <vector>

namespace csgify {
namespace {

void set_error(std::string* error, const std::string& message) {
    if (error) *error = message;
}

std::string digest_text(const std::string& value) {
    return "fnv1a128:" + fnv1a128_hex(fnv1a128(value.data(), value.size()));
}

std::string json_escape(const std::string& value) {
    std::ostringstream out;
    for (const unsigned char ch : value) {
        switch (ch) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (ch < 0x20) {
                out << "\\u" << std::hex << std::setw(4)
                    << std::setfill('0') << static_cast<int>(ch)
                    << std::dec << std::setfill(' ');
            } else {
                out << static_cast<char>(ch);
            }
        }
    }
    return out.str();
}

const char* severity_name(DiagnosticSeverity severity) {
    switch (severity) {
    case DiagnosticSeverity::info: return "info";
    case DiagnosticSeverity::warning: return "warning";
    case DiagnosticSeverity::error: return "error";
    }
    return "error";
}

Diagnostic diagnostic(DiagnosticSeverity severity,
                      std::string code,
                      std::string message,
                      const std::string& domain_id) {
    Diagnostic value;
    value.severity = severity;
    value.code = std::move(code);
    value.message = std::move(message);
    value.shape_id = domain_id;
    return value;
}

AleaCoverageCertificateStatus coverage_status_from_text(
    const std::string& value) {
    if (value == "certified") return AleaCoverageCertificateStatus::certified;
    if (value == "blocked_overlap")
        return AleaCoverageCertificateStatus::blocked_overlap;
    if (value == "blocked_gap")
        return AleaCoverageCertificateStatus::blocked_gap;
    if (value == "blocked_undefined_fill")
        return AleaCoverageCertificateStatus::blocked_undefined_fill;
    if (value == "unresolved") return AleaCoverageCertificateStatus::unresolved;
    return AleaCoverageCertificateStatus::failed;
}

DiagnosticSeverity severity_from_text(const std::string& value) {
    return value == "info" ? DiagnosticSeverity::info
        : value == "warning" ? DiagnosticSeverity::warning
                              : DiagnosticSeverity::error;
}

std::size_t json_size(const JsonValue* value) {
    return value && value->kind == JsonKind::number && value->num >= 0.0
        ? static_cast<std::size_t>(value->num) : 0;
}

std::uint64_t json_u64(const JsonValue* value) {
    return value && value->kind == JsonKind::number && value->num >= 0.0
        ? static_cast<std::uint64_t>(value->num) : 0;
}

bool valid_bounds(const BBox& b) {
    return b.valid
        && std::isfinite(b.xmin) && std::isfinite(b.ymin)
        && std::isfinite(b.zmin) && std::isfinite(b.xmax)
        && std::isfinite(b.ymax) && std::isfinite(b.zmax)
        && b.xmin < b.xmax && b.ymin < b.ymax && b.zmin < b.zmax;
}

std::string serialize_impl(const AleaCoverageCertificate& c,
                           bool include_digest) {
    std::ostringstream out;
    out << std::setprecision(17);
    out << "{\n"
        << "  \"schema\": \"" << json_escape(c.schema) << "\",\n"
        << "  \"schema_major\": " << c.schema_major << ",\n"
        << "  \"schema_minor\": " << c.schema_minor << ",\n"
        << "  \"producer\": \"" << json_escape(c.producer) << "\",\n"
        << "  \"producer_version\": \"" << json_escape(c.producer_version) << "\",\n"
        << "  \"libalea_version\": \"" << json_escape(c.libalea_version) << "\",\n"
        << "  \"handoff_contract_digest\": \""
        << json_escape(c.handoff_contract_digest) << "\",\n"
        << "  \"domain_digest\": \"" << json_escape(c.domain_digest) << "\",\n"
        << "  \"policy_digest\": \"" << json_escape(c.policy_digest) << "\",\n"
        << "  \"certificate_digest\": \""
        << (include_digest ? json_escape(c.certificate_digest) : std::string())
        << "\",\n"
        << "  \"status\": \"" << alea_coverage_status_name(c.status) << "\",\n"
        << "  \"domain\": {\"id\": \"" << json_escape(c.domain.domain_id)
        << "\", \"bounds\": [" << c.domain.bounds.xmin << ','
        << c.domain.bounds.ymin << ',' << c.domain.bounds.zmin << ','
        << c.domain.bounds.xmax << ',' << c.domain.bounds.ymax << ','
        << c.domain.bounds.zmax << "]},\n"
        << "  \"material_cell_count\": " << c.material_cell_count << ",\n"
        << "  \"intentional_void_cell_count\": "
        << c.intentional_void_cell_count << ",\n"
        << "  \"graveyard_cell_count\": " << c.graveyard_cell_count << ",\n";
    if (c.schema_minor >= 1) {
        out << "  \"fill_instance_count\": " << c.fill_instance_count << ",\n"
            << "  \"reused_universe_count\": " << c.reused_universe_count << ",\n"
            << "  \"estimated_duplicated_cells_avoided\": "
            << c.estimated_duplicated_cells_avoided << ",\n";
    }
    out
        << "  \"source_overlap_count\": " << c.source_overlap_count << ",\n"
        << "  \"source_overlap_unknown_count\": "
        << c.source_overlap_unknown_count << ",\n";
    if (c.schema_minor >= 1) {
        out << "  \"source_overlaps\": [";
        for (std::size_t i = 0; i < c.source_overlaps.size(); ++i) {
            if (i) out << ',';
            const AleaSourceOverlapEvidence& overlap = c.source_overlaps[i];
            out << "{\"component_a\":\"" << json_escape(overlap.component_a)
                << "\",\"component_b\":\"" << json_escape(overlap.component_b)
                << "\",\"intersection_volume\":" << overlap.intersection_volume
                << ",\"minimum_component_fraction\":"
                << overlap.minimum_component_fraction << '}';
        }
        out << "],\n";
    }
    out
        << "  \"exact_checks_performed\": " << c.exact_checks_performed << ",\n"
        << "  \"structural_exact_complement\": "
        << (c.structural_exact_complement ? "true" : "false") << ",\n"
        << "  \"committed\": " << (c.committed ? "true" : "false") << ",\n"
        << "  \"merged\": " << (c.merged ? "true" : "false") << ",\n"
        << "  \"execution\": {\"requested_workers\": "
        << c.execution.requested_workers << ", \"actual_workers\": "
        << c.execution.actual_workers << ", \"frontier_task_count\": "
        << c.execution.frontier_task_count << ", \"parallel_batch_count\": "
        << c.execution.parallel_batch_count << ", \"scratch_bytes_per_worker\": "
        << c.execution.scratch_bytes_per_worker
        << ", \"reserved_parallel_scratch_bytes\": "
        << c.execution.reserved_parallel_scratch_bytes << "},\n"
        << "  \"coverage\": {\"method\": \""
        << json_escape(c.coverage.method) << "\", \"samples_per_axis\": "
        << c.coverage.samples_per_axis << ", \"sample_count\": "
        << c.coverage.sample_count << ", \"unique_count\": "
        << c.coverage.unique_count << ", \"gap_count\": "
        << c.coverage.gap_count << ", \"overlap_count\": "
        << c.coverage.overlap_count << ", \"undefined_fill_count\": "
        << c.coverage.undefined_fill_count << ", \"unresolved_count\": "
        << c.coverage.unresolved_count << ", \"truncated_count\": "
        << c.coverage.truncated_count << "},\n";
    const auto write_ints = [&](const char* name, const std::vector<int>& values) {
        out << "  \"" << name << "\": [";
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i) out << ',';
            out << values[i];
        }
        out << "],\n";
    };
    write_ints("void_cell_ids", c.void_cell_ids);
    write_ints("graveyard_cell_ids", c.graveyard_cell_ids);
    out << "  \"limitations\": [";
    for (std::size_t i = 0; i < c.limitations.size(); ++i) {
        if (i) out << ',';
        out << "\"" << json_escape(c.limitations[i]) << "\"";
    }
    out << "],\n  \"diagnostics\": [";
    for (std::size_t i = 0; i < c.diagnostics.size(); ++i) {
        if (i) out << ',';
        const Diagnostic& d = c.diagnostics[i];
        out << "{\"severity\":\"" << severity_name(d.severity)
            << "\",\"code\":\"" << json_escape(d.code)
            << "\",\"message\":\"" << json_escape(d.message)
            << "\",\"entity_id\":\"" << json_escape(d.shape_id) << "\"}";
    }
    out << "]\n}\n";
    return out.str();
}

void collect_cell_ids(const alea_system_t* system,
                      std::size_t begin,
                      std::size_t end,
                      std::vector<int>* ids) {
    for (std::size_t index = begin; index < end; ++index) {
        alea_cell_info_t info{};
        if (alea_cell_get_info(system, index, &info) == 0) {
            ids->push_back(info.cell_id);
        }
    }
}

bool sample_complete_coverage(alea_system_t* system,
                              const AleaVoidDomain& domain,
                              const AleaVoidPolicy& policy,
                              AleaCoverageEvidence* evidence,
                              std::string* error) {
    const std::size_t n = policy.coverage_samples_per_axis;
    evidence->samples_per_axis = n;
    if (n == 0) return true;
    if (policy.max_coverage_hits == 0) {
        set_error(error, "max_coverage_hits must be positive when coverage sampling is enabled");
        return false;
    }
    std::vector<alea_cell_hit_t> hits(policy.max_coverage_hits);
    std::vector<std::uint64_t> occurrences(policy.max_coverage_hits);
    std::vector<std::uint64_t> parents(policy.max_coverage_hits);
    std::vector<std::uint8_t> owners(policy.max_coverage_hits);
    const BBox& b = domain.bounds;
    for (std::size_t iz = 0; iz < n; ++iz) {
        for (std::size_t iy = 0; iy < n; ++iy) {
            for (std::size_t ix = 0; ix < n; ++ix) {
                const double x = b.xmin + (b.xmax - b.xmin)
                    * (static_cast<double>(ix) + 0.5) / static_cast<double>(n);
                const double y = b.ymin + (b.ymax - b.ymin)
                    * (static_cast<double>(iy) + 0.5) / static_cast<double>(n);
                const double z = b.zmin + (b.zmax - b.zmin)
                    * (static_cast<double>(iz) + 0.5) / static_cast<double>(n);
                const int retained = alea_find_all_cells_coverage_chain(
                    system, x, y, z, hits.data(), occurrences.data(),
                    parents.data(), hits.size());
                ++evidence->sample_count;
                if (retained < 0) {
                    set_error(error, alea_error()[0] ? alea_error()
                                                     : "Alea coverage query failed");
                    return false;
                }
                if (static_cast<std::size_t>(retained) >= hits.size()) {
                    ++evidence->truncated_count;
                    continue;
                }
                if (retained == 0) {
                    ++evidence->gap_count;
                    continue;
                }
                alea_point_coverage_classification_t classification{};
                if (alea_classify_point_coverage_chain(
                        hits.data(), occurrences.data(), parents.data(),
                        static_cast<std::size_t>(retained), -1, owners.data(),
                        &classification) != 0) {
                    set_error(error, alea_error()[0] ? alea_error()
                                                     : "Alea coverage classification failed");
                    return false;
                }
                switch (classification.kind) {
                case ALEA_POINT_COVERAGE_UNIQUE: ++evidence->unique_count; break;
                case ALEA_POINT_COVERAGE_GAP: ++evidence->gap_count; break;
                case ALEA_POINT_COVERAGE_OVERLAP: ++evidence->overlap_count; break;
                case ALEA_POINT_COVERAGE_UNDEFINED_FILL:
                    ++evidence->undefined_fill_count; break;
                case ALEA_POINT_COVERAGE_UNRESOLVED:
                    ++evidence->unresolved_count; break;
                }
            }
        }
    }
    return true;
}

} // namespace

const char* alea_coverage_status_name(AleaCoverageCertificateStatus status) {
    switch (status) {
    case AleaCoverageCertificateStatus::certified: return "certified";
    case AleaCoverageCertificateStatus::blocked_overlap: return "blocked_overlap";
    case AleaCoverageCertificateStatus::blocked_gap: return "blocked_gap";
    case AleaCoverageCertificateStatus::blocked_undefined_fill:
        return "blocked_undefined_fill";
    case AleaCoverageCertificateStatus::unresolved: return "unresolved";
    case AleaCoverageCertificateStatus::failed: return "failed";
    }
    return "failed";
}

std::string alea_void_domain_digest(const AleaVoidDomain& domain) {
    std::ostringstream out;
    out << std::setprecision(17) << domain.domain_id << '|'
        << domain.bounds.xmin << '|' << domain.bounds.ymin << '|'
        << domain.bounds.zmin << '|' << domain.bounds.xmax << '|'
        << domain.bounds.ymax << '|' << domain.bounds.zmax;
    return digest_text(out.str());
}

std::string alea_void_policy_digest(const AleaVoidPolicy& p) {
    std::ostringstream out;
    out << std::setprecision(17) << p.max_depth << '|' << p.min_size << '|'
        << p.probes_per_axis << '|' << p.requested_workers << '|'
        << p.max_parallel_scratch_bytes << '|' << p.parallel_frontier_depth
        << '|' << p.merge << '|' << p.add_graveyard << '|'
        << p.coverage_samples_per_axis << '|' << p.max_coverage_hits;
    return digest_text(out.str());
}

AleaVoidDomain default_alea_void_domain(const ConversionReport& report,
                                        double margin_fraction) {
    AleaVoidDomain domain;
    BBox bounds;
    for (const AnalyzedSolid& solid : report.solids) {
        if (!solid.bbox.valid) continue;
        if (!bounds.valid) {
            bounds = solid.bbox;
        } else {
            bounds.xmin = std::min(bounds.xmin, solid.bbox.xmin);
            bounds.ymin = std::min(bounds.ymin, solid.bbox.ymin);
            bounds.zmin = std::min(bounds.zmin, solid.bbox.zmin);
            bounds.xmax = std::max(bounds.xmax, solid.bbox.xmax);
            bounds.ymax = std::max(bounds.ymax, solid.bbox.ymax);
            bounds.zmax = std::max(bounds.zmax, solid.bbox.zmax);
        }
        bounds.valid = true;
    }
    if (bounds.valid) {
        const double dx = bounds.xmax - bounds.xmin;
        const double dy = bounds.ymax - bounds.ymin;
        const double dz = bounds.zmax - bounds.zmin;
        const double margin = std::sqrt(dx * dx + dy * dy + dz * dz)
            * std::max(0.0, margin_fraction);
        bounds.xmin -= margin; bounds.ymin -= margin; bounds.zmin -= margin;
        bounds.xmax += margin; bounds.ymax += margin; bounds.zmax += margin;
    }
    domain.bounds = bounds;
    return domain;
}

AleaVoidDomain default_alea_void_domain(const AleaHandoffManifest& handoff,
                                        double margin_fraction) {
    AleaVoidDomain domain;
    BBox bounds;
    for (const AleaUniverseRecord& universe : handoff.universes) {
        if (universe.universe_id != 0 || !universe.bbox.valid) continue;
        if (!bounds.valid) {
            bounds = universe.bbox;
        } else {
            bounds.xmin = std::min(bounds.xmin, universe.bbox.xmin);
            bounds.ymin = std::min(bounds.ymin, universe.bbox.ymin);
            bounds.zmin = std::min(bounds.zmin, universe.bbox.zmin);
            bounds.xmax = std::max(bounds.xmax, universe.bbox.xmax);
            bounds.ymax = std::max(bounds.ymax, universe.bbox.ymax);
            bounds.zmax = std::max(bounds.zmax, universe.bbox.zmax);
        }
        bounds.valid = true;
    }
    if (bounds.valid) {
        const double dx = bounds.xmax - bounds.xmin;
        const double dy = bounds.ymax - bounds.ymin;
        const double dz = bounds.zmax - bounds.zmin;
        const double margin = std::sqrt(dx * dx + dy * dy + dz * dz)
            * std::max(0.0, margin_fraction);
        bounds.xmin -= margin; bounds.ymin -= margin; bounds.zmin -= margin;
        bounds.xmax += margin; bounds.ymax += margin; bounds.zmax += margin;
    }
    domain.bounds = bounds;
    return domain;
}

bool generate_alea_domain_voids(alea_system_t* system,
                                const ConversionReport& source_report,
                                const AleaHandoffManifest& handoff,
                                const AleaVoidDomain& domain,
                                const AleaVoidPolicy& policy,
                                AleaCoverageCertificate* certificate,
                                std::string* error) {
    if (!system || !certificate || !valid_bounds(domain.bounds)
        || domain.domain_id.empty() || policy.max_depth < 0
        || policy.probes_per_axis <= 0) {
        set_error(error, "invalid Alea void bridge argument or finite domain");
        return false;
    }
    std::string handoff_error;
    if (!validate_alea_handoff_manifest(handoff, &handoff_error)) {
        set_error(error, "invalid Alea handoff: " + handoff_error);
        return false;
    }
    AleaBridgeRequirements requirements;
    requirements.void_generation = true;
    requirements.complete_coverage = true;
    const AleaBridgeNegotiation negotiation = negotiate_alea_bridge(
        query_alea_bridge_capabilities(), requirements);
    if (!negotiation.compatible) {
        set_error(error, "libalea lacks required void or complete-coverage capability");
        return false;
    }
    AleaCoverageCertificate result;
    result.producer_version = "0.1.0";
    result.libalea_version = alea_version();
    result.handoff_contract_digest = handoff.contract_digest;
    result.domain = domain;
    result.domain_digest = alea_void_domain_digest(domain);
    result.policy_digest = alea_void_policy_digest(policy);
    result.material_cell_count = alea_cell_count(system);
    std::map<int, std::size_t> fill_counts;
    std::map<int, std::size_t> universe_cells;
    for (const AleaUniverseRecord& universe : handoff.universes) {
        universe_cells[universe.universe_id] = universe.cell_count;
    }
    for (const AleaCellRecord& cell : handoff.cells) {
        if (cell.fill_universe >= 0) {
            ++result.fill_instance_count;
            ++fill_counts[cell.fill_universe];
        }
    }
    for (const auto& entry : fill_counts) {
        if (entry.second < 2) continue;
        ++result.reused_universe_count;
        result.estimated_duplicated_cells_avoided +=
            (entry.second - 1) * universe_cells[entry.first];
    }
    result.source_overlap_count = std::max(
        source_report.overlaps.size(),
        source_report.mcnp_geometry_safety.overlap_count);
    result.source_overlap_unknown_count =
        source_report.mcnp_geometry_safety.overlap_unknown_count;
    for (const SolidOverlap& overlap : source_report.overlaps) {
        result.source_overlaps.push_back({overlap.solid_a_id,
            overlap.solid_b_id, overlap.intersection_volume,
            overlap.min_fraction});
    }
    result.exact_checks_performed = source_report.performance.exact_overlap_tests;

    alea_void_options_t options;
    alea_void_options_init(&options);
    options.max_depth = policy.max_depth;
    options.min_size = policy.min_size;
    options.probes_per_axis = policy.probes_per_axis;
    options.requested_workers = policy.requested_workers;
    options.max_parallel_scratch_bytes = policy.max_parallel_scratch_bytes;
    options.parallel_frontier_depth = policy.parallel_frontier_depth;
    const alea_bbox_t bounds = {
        domain.bounds.xmin, domain.bounds.xmax,
        domain.bounds.ymin, domain.bounds.ymax,
        domain.bounds.zmin, domain.bounds.zmax,
    };
    void_result_t* generated = alea_void_generate_in_bbox_ex(
        system, &bounds, &options);
    if (!generated) {
        set_error(error, alea_error()[0] ? alea_error()
                                         : "libalea void generation failed");
        result.diagnostics.push_back(diagnostic(
            DiagnosticSeverity::error, "alea_void_generation_failed",
            error ? *error : "libalea void generation failed", domain.domain_id));
        *certificate = std::move(result);
        return false;
    }
    alea_void_execution_stats_t stats{};
    if (alea_void_get_execution_stats(generated, &stats) != 0) {
        const std::string message = alea_error()[0] ? alea_error()
                                                    : "cannot read void execution statistics";
        alea_void_free(generated);
        set_error(error, message);
        *certificate = std::move(result);
        return false;
    }
    result.execution = {stats.requested_workers, stats.actual_workers,
        stats.frontier_task_count, stats.parallel_batch_count,
        stats.scratch_bytes_per_worker, stats.reserved_parallel_scratch_bytes};
    if (policy.merge && alea_void_merge(system, generated) < 0) {
        const std::string message = alea_error()[0] ? alea_error()
                                                    : "libalea void merge failed";
        alea_void_free(generated);
        set_error(error, message);
        *certificate = std::move(result);
        return false;
    }
    result.merged = policy.merge;
    const std::size_t before_void = alea_cell_count(system);
    const int added = alea_void_add_cells(system, generated);
    if (added < 0) {
        const std::string message = alea_error()[0] ? alea_error()
                                                    : "libalea void commit failed";
        alea_void_free(generated);
        set_error(error, message);
        *certificate = std::move(result);
        return false;
    }
    result.committed = true;
    result.intentional_void_cell_count = static_cast<std::size_t>(added);
    const std::size_t after_void = alea_cell_count(system);
    collect_cell_ids(system, before_void, after_void, &result.void_cell_ids);
    for (std::size_t index = before_void; index < after_void; ++index) {
        const std::string comment = "csgify:void-domain=" + domain.domain_id;
        if (alea_cell_set_inline_comment(system, static_cast<int>(index),
                                         comment.c_str()) != 0) {
            alea_void_free(generated);
            set_error(error, "failed to attach void provenance comment");
            *certificate = std::move(result);
            return false;
        }
    }
    if (policy.add_graveyard) {
        const std::size_t before_graveyard = alea_cell_count(system);
        if (alea_void_add_graveyard(system, generated) < 0) {
            alea_void_free(generated);
            set_error(error, alea_error()[0] ? alea_error()
                                             : "libalea graveyard construction failed");
            *certificate = std::move(result);
            return false;
        }
        const std::size_t after_graveyard = alea_cell_count(system);
        result.graveyard_cell_count = after_graveyard - before_graveyard;
        collect_cell_ids(system, before_graveyard, after_graveyard,
                         &result.graveyard_cell_ids);
        for (std::size_t index = before_graveyard; index < after_graveyard; ++index) {
            const std::string comment = "csgify:graveyard-domain=" + domain.domain_id;
            if (alea_cell_set_inline_comment(system, static_cast<int>(index),
                                             comment.c_str()) != 0) {
                alea_void_free(generated);
                set_error(error, "failed to attach graveyard provenance comment");
                *certificate = std::move(result);
                return false;
            }
        }
    }
    alea_void_free(generated);
    if (alea_build_universe_index(system) != 0) {
        set_error(error, alea_error()[0] ? alea_error()
                                         : "libalea universe index rebuild failed");
        *certificate = std::move(result);
        return false;
    }
    result.structural_exact_complement = true;
    result.limitations.push_back(
        "Void cells are exact analytical complements inside deterministic octree regions; coverage lattice evidence is diagnostic, not a continuum proof of source-cell disjointness.");
    if (!sample_complete_coverage(system, domain, policy, &result.coverage, error)) {
        result.status = AleaCoverageCertificateStatus::failed;
        *certificate = std::move(result);
        return false;
    }
    result.exact_checks_performed += result.coverage.sample_count;

    if (result.source_overlap_count > 0 || result.coverage.overlap_count > 0) {
        result.status = AleaCoverageCertificateStatus::blocked_overlap;
        result.diagnostics.push_back(diagnostic(
            DiagnosticSeverity::error, "alea_coverage_overlap",
            "Exact-one ownership is blocked by source or sampled Alea overlap evidence.",
            domain.domain_id));
    } else if (result.coverage.gap_count > 0) {
        result.status = AleaCoverageCertificateStatus::blocked_gap;
        result.diagnostics.push_back(diagnostic(
            DiagnosticSeverity::error, "alea_coverage_gap",
            "The complete-ownership lattice found unowned points inside the modeled domain.",
            domain.domain_id));
    } else if (result.coverage.undefined_fill_count > 0) {
        result.status = AleaCoverageCertificateStatus::blocked_undefined_fill;
        result.diagnostics.push_back(diagnostic(
            DiagnosticSeverity::error, "alea_coverage_undefined_fill",
            "The complete-ownership lattice found undefined universe fills.",
            domain.domain_id));
    } else if (result.source_overlap_unknown_count > 0
               || result.coverage.unresolved_count > 0
               || result.coverage.truncated_count > 0) {
        result.status = AleaCoverageCertificateStatus::unresolved;
        result.diagnostics.push_back(diagnostic(
            DiagnosticSeverity::error, "alea_coverage_unresolved",
            "Coverage evidence is truncated, unknown, or unresolved.",
            domain.domain_id));
    } else {
        result.status = AleaCoverageCertificateStatus::certified;
        result.diagnostics.push_back(diagnostic(
            DiagnosticSeverity::info, "alea_coverage_certified",
            "The modeled domain has an exact analytical void complement and no blocking ownership evidence.",
            domain.domain_id));
    }
    result.certificate_digest = digest_text(serialize_impl(result, false));
    *certificate = std::move(result);
    return true;
}

bool validate_alea_coverage_certificate(
    const AleaCoverageCertificate& c,
    std::string* error) {
    if (c.schema != kAleaCoverageSchema
        || c.schema_major != kAleaCoverageSchemaMajor
        || c.schema_minor > kAleaCoverageSchemaMinor) {
        set_error(error, "unsupported Alea coverage certificate schema");
        return false;
    }
    if (!valid_bounds(c.domain.bounds) || c.domain.domain_id.empty()
        || c.handoff_contract_digest.empty() || c.domain_digest.empty()
        || c.policy_digest.empty() || c.certificate_digest.empty()) {
        set_error(error, "Alea coverage certificate has an empty required field");
        return false;
    }
    if (c.domain_digest != alea_void_domain_digest(c.domain)) {
        set_error(error, "Alea coverage domain digest mismatch");
        return false;
    }
    if (c.source_overlaps.size() > c.source_overlap_count) {
        set_error(error, "Alea coverage overlap detail exceeds its reported count");
        return false;
    }
    for (const AleaSourceOverlapEvidence& overlap : c.source_overlaps) {
        if (overlap.component_a.empty() || overlap.component_b.empty()
            || overlap.component_a == overlap.component_b
            || !std::isfinite(overlap.intersection_volume)
            || overlap.intersection_volume < 0.0
            || !std::isfinite(overlap.minimum_component_fraction)
            || overlap.minimum_component_fraction < 0.0) {
            set_error(error, "Alea coverage certificate has invalid overlap detail");
            return false;
        }
    }
    if (c.certificate_digest != digest_text(serialize_impl(c, false))) {
        set_error(error, "Alea coverage certificate digest mismatch");
        return false;
    }
    if (c.status == AleaCoverageCertificateStatus::certified
        && (!c.committed || !c.structural_exact_complement
            || c.source_overlap_count || c.source_overlap_unknown_count
            || c.coverage.gap_count || c.coverage.overlap_count
            || c.coverage.undefined_fill_count || c.coverage.unresolved_count
            || c.coverage.truncated_count)) {
        set_error(error, "certified Alea coverage contains blocking evidence");
        return false;
    }
    return true;
}

std::string serialize_alea_coverage_certificate(
    const AleaCoverageCertificate& certificate) {
    return serialize_impl(certificate, true);
}

AleaCoverageCertificate parse_alea_coverage_certificate_json(
    const std::string& json) {
    const std::shared_ptr<JsonValue> root = JsonParser(json).parse();
    if (!root || root->kind != JsonKind::object) {
        throw std::runtime_error("Alea coverage certificate root must be an object");
    }
    AleaCoverageCertificate c;
    const auto string_field = [&](const char* key, std::string* target) {
        if (const JsonValue* value = root->get(key))
            *target = value->as_string();
    };
    string_field("schema", &c.schema);
    c.schema_major = json_size(root->get("schema_major"));
    c.schema_minor = json_size(root->get("schema_minor"));
    string_field("producer", &c.producer);
    string_field("producer_version", &c.producer_version);
    string_field("libalea_version", &c.libalea_version);
    string_field("handoff_contract_digest", &c.handoff_contract_digest);
    string_field("domain_digest", &c.domain_digest);
    string_field("policy_digest", &c.policy_digest);
    string_field("certificate_digest", &c.certificate_digest);
    if (const JsonValue* value = root->get("status"))
        c.status = coverage_status_from_text(value->as_string());
    if (const JsonValue* domain = root->get("domain");
        domain && domain->kind == JsonKind::object) {
        if (const JsonValue* value = domain->get("id"))
            c.domain.domain_id = value->as_string();
        if (const JsonValue* bounds = domain->get("bounds");
            bounds && bounds->kind == JsonKind::array
            && bounds->arr.size() == 6) {
            c.domain.bounds = {bounds->arr[0]->as_number(),
                bounds->arr[1]->as_number(), bounds->arr[2]->as_number(),
                bounds->arr[3]->as_number(), bounds->arr[4]->as_number(),
                bounds->arr[5]->as_number(), true};
        }
    }
    c.material_cell_count = json_size(root->get("material_cell_count"));
    c.intentional_void_cell_count =
        json_size(root->get("intentional_void_cell_count"));
    c.graveyard_cell_count = json_size(root->get("graveyard_cell_count"));
    c.fill_instance_count = json_size(root->get("fill_instance_count"));
    c.reused_universe_count = json_size(root->get("reused_universe_count"));
    c.estimated_duplicated_cells_avoided =
        json_size(root->get("estimated_duplicated_cells_avoided"));
    c.source_overlap_count = json_size(root->get("source_overlap_count"));
    c.source_overlap_unknown_count =
        json_size(root->get("source_overlap_unknown_count"));
    if (const JsonValue* overlaps = root->get("source_overlaps");
        overlaps && overlaps->kind == JsonKind::array) {
        for (const auto& value : overlaps->arr) {
            if (!value || value->kind != JsonKind::object) continue;
            AleaSourceOverlapEvidence overlap;
            if (const JsonValue* field = value->get("component_a"))
                overlap.component_a = field->as_string();
            if (const JsonValue* field = value->get("component_b"))
                overlap.component_b = field->as_string();
            if (const JsonValue* field = value->get("intersection_volume"))
                overlap.intersection_volume = field->as_number();
            if (const JsonValue* field = value->get("minimum_component_fraction"))
                overlap.minimum_component_fraction = field->as_number();
            c.source_overlaps.push_back(std::move(overlap));
        }
    }
    c.exact_checks_performed = json_size(root->get("exact_checks_performed"));
    if (const JsonValue* value = root->get("structural_exact_complement"))
        c.structural_exact_complement = value->as_bool();
    if (const JsonValue* value = root->get("committed"))
        c.committed = value->as_bool();
    if (const JsonValue* value = root->get("merged"))
        c.merged = value->as_bool();
    if (const JsonValue* execution = root->get("execution");
        execution && execution->kind == JsonKind::object) {
        c.execution.requested_workers =
            json_size(execution->get("requested_workers"));
        c.execution.actual_workers = json_size(execution->get("actual_workers"));
        c.execution.frontier_task_count =
            json_size(execution->get("frontier_task_count"));
        c.execution.parallel_batch_count =
            json_size(execution->get("parallel_batch_count"));
        c.execution.scratch_bytes_per_worker =
            json_u64(execution->get("scratch_bytes_per_worker"));
        c.execution.reserved_parallel_scratch_bytes =
            json_u64(execution->get("reserved_parallel_scratch_bytes"));
    }
    if (const JsonValue* coverage = root->get("coverage");
        coverage && coverage->kind == JsonKind::object) {
        if (const JsonValue* value = coverage->get("method"))
            c.coverage.method = value->as_string();
        c.coverage.samples_per_axis =
            json_size(coverage->get("samples_per_axis"));
        c.coverage.sample_count = json_u64(coverage->get("sample_count"));
        c.coverage.unique_count = json_u64(coverage->get("unique_count"));
        c.coverage.gap_count = json_u64(coverage->get("gap_count"));
        c.coverage.overlap_count = json_u64(coverage->get("overlap_count"));
        c.coverage.undefined_fill_count =
            json_u64(coverage->get("undefined_fill_count"));
        c.coverage.unresolved_count =
            json_u64(coverage->get("unresolved_count"));
        c.coverage.truncated_count =
            json_u64(coverage->get("truncated_count"));
    }
    const auto read_ids = [&](const char* key, std::vector<int>* target) {
        const JsonValue* values = root->get(key);
        if (!values || values->kind != JsonKind::array) return;
        for (const auto& value : values->arr) {
            if (value && value->kind == JsonKind::number)
                target->push_back(static_cast<int>(value->num));
        }
    };
    read_ids("void_cell_ids", &c.void_cell_ids);
    read_ids("graveyard_cell_ids", &c.graveyard_cell_ids);
    if (const JsonValue* values = root->get("limitations");
        values && values->kind == JsonKind::array) {
        for (const auto& value : values->arr)
            if (value && value->kind == JsonKind::string)
                c.limitations.push_back(value->str);
    }
    if (const JsonValue* values = root->get("diagnostics");
        values && values->kind == JsonKind::array) {
        for (const auto& value : values->arr) {
            if (!value || value->kind != JsonKind::object) continue;
            Diagnostic d;
            if (const JsonValue* field = value->get("severity"))
                d.severity = severity_from_text(field->as_string());
            if (const JsonValue* field = value->get("code"))
                d.code = field->as_string();
            if (const JsonValue* field = value->get("message"))
                d.message = field->as_string();
            if (const JsonValue* field = value->get("entity_id"))
                d.shape_id = field->as_string();
            c.diagnostics.push_back(std::move(d));
        }
    }
    std::string error;
    if (!validate_alea_coverage_certificate(c, &error)) {
        throw std::runtime_error("invalid Alea coverage certificate: " + error);
    }
    return c;
}

AleaCoverageCertificate read_alea_coverage_certificate_json(
    const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open coverage certificate: " + path);
    const std::string json((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    return parse_alea_coverage_certificate_json(json);
}

std::vector<AleaCoverageReviewItem> alea_coverage_review_items(
    const AleaCoverageCertificate& c) {
    std::vector<AleaCoverageReviewItem> items;
    const auto add = [&](std::string id, std::string category,
                         std::uint64_t count, std::string summary,
                         std::string action, bool blocking) {
        if (count == 0) return;
        items.push_back({std::move(id), std::move(category),
            blocking ? "error" : "info", count, std::move(summary),
            std::move(action), blocking, false});
    };
    for (std::size_t i = 0; i < c.source_overlaps.size(); ++i) {
        const AleaSourceOverlapEvidence& overlap = c.source_overlaps[i];
        AleaCoverageReviewItem item;
        item.id = "coverage:source-overlap:" + std::to_string(i + 1);
        item.category = "overlap";
        item.severity = "error";
        item.count = 1;
        std::ostringstream summary;
        summary << "Source components " << overlap.component_a << " and "
                << overlap.component_b << " overlap by "
                << std::setprecision(17) << overlap.intersection_volume
                << " volume units (smaller-component fraction "
                << overlap.minimum_component_fraction << ").";
        item.summary = summary.str();
        item.recommended_action =
            "Choose exactly one of the two components as owner of the overlapping spatial region, apply the removal-only repair, then regenerate coverage.";
        item.blocking = true;
        item.auto_action_allowed = false;
        item.target_ids = {overlap.component_a, overlap.component_b};
        items.push_back(std::move(item));
    }
    const std::uint64_t unpaired_source_overlaps = c.source_overlap_count
            > c.source_overlaps.size()
        ? c.source_overlap_count - c.source_overlaps.size() : 0;
    add("coverage:source-overlap", "overlap", unpaired_source_overlaps,
        "Source components have positive-volume overlap without retained pair detail.",
        "Re-run exact source overlap analysis, then choose one owning component per region.", true);
    add("coverage:sampled-overlap", "overlap", c.coverage.overlap_count,
        "Libalea found multiple concrete owners at sampled domain points.",
        "Inspect the owner components and assign each overlapping region to exactly one owner.", true);
    add("coverage:gap", "gap", c.coverage.gap_count,
        "Points inside the modeled domain have no owner.",
        "Regenerate voids or correct the declared domain/geometry before MCNP export.", true);
    add("coverage:undefined-fill", "undefined_fill",
        c.coverage.undefined_fill_count,
        "A universe fill does not resolve to a defined terminal owner.",
        "Repair the fill/universe mapping and regenerate the certificate.", true);
    add("coverage:source-unknown", "unresolved",
        c.source_overlap_unknown_count,
        "Source overlap analysis returned an unknown result.",
        "Retry the exact overlap check with sufficient resources or review the source CAD.", true);
    add("coverage:unresolved", "unresolved", c.coverage.unresolved_count,
        "Complete ownership could not be resolved at sampled points.",
        "Inspect hierarchy diagnostics and repair ambiguous fills or geometry.", true);
    add("coverage:truncated", "resource_limit", c.coverage.truncated_count,
        "The configured owner-hit budget truncated complete ownership queries.",
        "Rerun with a larger coverage-hit budget under an approved resource policy.", true);
    if (items.empty()) {
        items.push_back({"coverage:certified", "coverage", "info",
            c.coverage.unique_count,
            "The modeled domain has an exact analytical void complement and no blocking ownership evidence.",
            "Proceed to final MCNP assembly and transport-policy review.", false, false});
    }
    items.push_back({"coverage:void-generation", "void_generation", "info",
        c.intentional_void_cell_count,
        "Libalea committed domain-scoped analytical void cells.",
        "Review cell provenance and execution limits; no decision is required.", false, false});
    if (c.fill_instance_count > 0) {
        items.push_back({"coverage:instance-reuse", "instance_reuse", "info",
            c.fill_instance_count,
            "Universe fills preserve repeated component instances.",
            "Review the reported duplicated-cell savings before optional flattening.", false, false});
    }
    return items;
}

bool publish_alea_coverage_certificate(
    const ArtifactStore& store,
    AleaCoverageCertificate certificate,
    ArtifactRecord* artifact,
    std::string* error) {
    if (!validate_alea_coverage_certificate(certificate, error)) return false;
    return store.publish_bytes("alea_coverage_certificate", "json",
                               serialize_alea_coverage_certificate(certificate),
                               artifact, error);
}

} // namespace csgify

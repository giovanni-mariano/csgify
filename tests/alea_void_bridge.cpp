#include "csgify/alea_void_bridge.hpp"
#include "csgify/autonomy.hpp"

#include <alea.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

namespace {

using System = std::unique_ptr<alea_system_t, decltype(&alea_destroy)>;

int fail(const std::string& message) {
    std::cerr << "alea_void_bridge: " << message << '\n';
    return EXIT_FAILURE;
}

System make_box_system() {
    System system(alea_create(), &alea_destroy);
    const int surface = alea_box_surface(system.get(), 1, -1, 1, -1, 1, -1, 1);
    const alea_node_id_t root = alea_halfspace(system.get(), surface, -1);
    const int material = alea_add_material(system.get(), 1);
    const int cell = alea_add_cell(system.get(), 1, root, material, -1.0, 0);
    if (surface < 0 || root == ALEA_NODE_ID_INVALID || material < 0 || cell < 0
        || alea_cell_set_inline_comment(system.get(), cell,
                                        "csgify:id=solid:1") != 0
        || alea_build_universe_index(system.get()) != 0) {
        return System(nullptr, &alea_destroy);
    }
    return system;
}

csgify::ConversionReport make_report() {
    csgify::ConversionReport report;
    report.solids_seen = 1;
    report.solids_converted = 1;
    report.performance.exact_overlap_tests = 1;
    csgify::AnalyzedSolid solid;
    solid.id = "solid:1";
    solid.bbox = {-1, -1, -1, 1, 1, 1, true};
    report.solids.push_back(solid);
    return report;
}

} // namespace

int main() {
    csgify::ConversionReport report = make_report();
    System system = make_box_system();
    if (!system) return fail("could not create reference system");
    const csgify::AleaHandoffManifest handoff =
        csgify::make_alea_handoff_manifest(system.get(), report,
                                           "fnv1a128:source",
                                           "fnv1a128:conversion-policy");
    csgify::AleaVoidDomain domain;
    domain.domain_id = "domain:test";
    domain.bounds = {-3, -3, -3, 3, 3, 3, true};
    csgify::AleaVoidPolicy policy;
    policy.max_depth = 4;
    policy.min_size = 0.25;
    policy.requested_workers = 1;
    policy.max_parallel_scratch_bytes = 64 * 1024;
    policy.coverage_samples_per_axis = 5;
    policy.add_graveyard = false;

    csgify::AleaCoverageCertificate certificate;
    std::string error;
    if (!csgify::generate_alea_domain_voids(
            system.get(), report, handoff, domain, policy, &certificate, &error)) {
        return fail("generation failed: " + error);
    }
    if (certificate.status != csgify::AleaCoverageCertificateStatus::certified
        || !certificate.committed || !certificate.structural_exact_complement
        || certificate.intentional_void_cell_count == 0
        || certificate.coverage.sample_count != 125
        || certificate.coverage.unique_count != 125
        || certificate.coverage.gap_count != 0
        || certificate.coverage.overlap_count != 0
        || certificate.void_cell_ids.size()
               != certificate.intentional_void_cell_count) {
        return fail("reference domain was not certified exact-one");
    }
    if (!csgify::validate_alea_coverage_certificate(certificate, &error)) {
        return fail("fresh certificate failed validation: " + error);
    }
    const std::string json =
        csgify::serialize_alea_coverage_certificate(certificate);
    if (json.find("\"status\": \"certified\"") == std::string::npos
        || json.find("\"structural_exact_complement\": true")
               == std::string::npos) {
        return fail("serialized certificate omitted required evidence");
    }
    const csgify::AleaCoverageCertificate parsed =
        csgify::parse_alea_coverage_certificate_json(json);
    if (parsed.certificate_digest != certificate.certificate_digest
        || csgify::serialize_alea_coverage_certificate(parsed) != json) {
        return fail("coverage certificate JSON round trip changed content");
    }
    const auto certified_items = csgify::alea_coverage_review_items(parsed);
    if (certified_items.empty() || certified_items.front().blocking
        || certified_items.front().id != "coverage:certified") {
        return fail("certified coverage did not produce a safe review item");
    }
    csgify::AleaCoverageCertificate corrupt = certificate;
    corrupt.coverage.gap_count = 1;
    if (csgify::validate_alea_coverage_certificate(corrupt, nullptr)) {
        return fail("certificate content mutation did not invalidate its digest");
    }

    const std::filesystem::path artifact_root =
        std::filesystem::temp_directory_path() / "csgify-alea-void-bridge-test";
    std::filesystem::remove_all(artifact_root);
    csgify::ArtifactStore store(artifact_root);
    csgify::ArtifactRecord artifact;
    if (!store.initialize(&error)
        || !csgify::publish_alea_coverage_certificate(
            store, certificate, &artifact, &error)
        || artifact.kind != "alea_coverage_certificate"
        || !store.validate(artifact, &error)) {
        return fail("certificate artifact publication failed: " + error);
    }

    System overlapping = make_box_system();
    if (!overlapping) return fail("could not create blocking reference system");
    csgify::SolidOverlap overlap;
    overlap.solid_a_id = "solid:1";
    overlap.solid_b_id = "solid:2";
    report.overlaps.push_back(overlap);
    const csgify::AleaHandoffManifest overlap_handoff =
        csgify::make_alea_handoff_manifest(overlapping.get(), report,
                                           "fnv1a128:source",
                                           "fnv1a128:conversion-policy");
    csgify::AleaCoverageCertificate blocked;
    if (!csgify::generate_alea_domain_voids(
            overlapping.get(), report, overlap_handoff, domain, policy,
            &blocked, &error)
        || blocked.status
               != csgify::AleaCoverageCertificateStatus::blocked_overlap
        || blocked.source_overlap_count != 1
        || !csgify::validate_alea_coverage_certificate(blocked, &error)) {
        return fail("source overlap did not produce a valid blocking certificate");
    }
    const auto blocked_items = csgify::alea_coverage_review_items(blocked);
    if (blocked_items.empty() || !blocked_items.front().blocking
        || blocked_items.front().category != "overlap"
        || blocked_items.front().auto_action_allowed) {
        return fail("overlap review item was not safely escalated");
    }
    csgify::AutonomyPolicy queue_policy;
    queue_policy.mode = csgify::AutonomyMode::automatic;
    const std::filesystem::path queue_path = artifact_root / "coverage-queue.json";
    csgify::AutonomyQueue queue = csgify::make_coverage_autonomy_queue(
        blocked, "coverage.json", "final.step", queue_policy);
    if (queue.items.size() != 1 || queue.human_review_count != 1
        || queue.auto_apply_count != 0
        || queue.items.front().proposal.op
               != "coverage_resolve_overlap_ownership"
        || !queue.items.front().requires_followup
        || queue.coverage_certificate_digest != blocked.certificate_digest
        || csgify::write_autonomy_queue_json(queue, queue_path.string()) != 0) {
        return fail("blocking certificate was not safely mapped into autonomy");
    }
    if (csgify::reject_autonomy_queue_item(
            queue, queue.items.front().proposal.id, "ignore overlap")) {
        return fail("unsafe coverage blocker could be dismissed without evidence");
    }
    csgify::AutonomyQueue restored =
        csgify::read_autonomy_queue_json(queue_path.string());
    if (restored.items.size() != 1 || restored.human_review_count != 1
        || restored.coverage_certificate_path != "coverage.json"
        || restored.coverage_certificate_digest != blocked.certificate_digest
        || restored.items.front().disposition
               != csgify::AutonomyDisposition::human_review) {
        return fail("coverage autonomy queue did not survive persistence");
    }
    csgify::reconcile_coverage_autonomy_queue(
        restored, certificate, "coverage-replacement.json");
    if (restored.human_review_count != 0 || restored.applied_count != 1
        || !restored.items.front().executed
        || !restored.items.front().execution_ok
        || !restored.items.front().evidence.certificate_passed
        || restored.items.front().requires_followup
        || restored.coverage_certificate_digest
               != certificate.certificate_digest) {
        return fail("replacement certificate did not verify queue resolution");
    }

    std::filesystem::remove_all(artifact_root);
    return EXIT_SUCCESS;
}

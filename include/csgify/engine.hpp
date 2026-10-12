#pragma once

#include "csgify/assembly.hpp"
#include "csgify/approximation.hpp"
#include "csgify/medium.hpp"

#include <memory>
#include <string>
#include <vector>

namespace csgify {

const char* csgify_version();
const char* occt_version();

enum class DiagnosticSeverity {
    info,
    warning,
    error,
};

struct Diagnostic {
    DiagnosticSeverity severity = DiagnosticSeverity::info;
    std::string code;
    std::string message;
    std::string shape_label;
    std::string shape_id;
    std::string document_id;
    std::string local_shape_id;
};

struct BBox {
    double xmin = 0.0;
    double ymin = 0.0;
    double zmin = 0.0;
    double xmax = 0.0;
    double ymax = 0.0;
    double zmax = 0.0;
    bool valid = false;
};

struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct PlaneSpec {
    Vec3 origin;
    Vec3 normal;
};

struct SolidVolume {
    std::string shape_label;
    double volume = 0.0;
};

enum class SplineFitKind {
    none,
    plane,
    sphere,
    cylinder,
    cone,
};

struct SplineFaceFit {
    SplineFitKind kind = SplineFitKind::none;
    bool available = false;
    // True only when max_deviation is an independently established upper
    // bound. A sampled residual alone must never be promoted to exactness.
    bool deviation_certified = false;
    std::size_t sample_count = 0;
    double max_deviation = 0.0;
    double rms_deviation = 0.0;
    double relative_max_deviation = 0.0;
    Vec3 origin;
    Vec3 direction;
    double radius = 0.0;
    double half_angle = 0.0; // radians; populated for cone only
};

struct SplineFitCandidates {
    bool attempted = false;
    SplineFaceFit plane;
    SplineFaceFit sphere;
    SplineFaceFit cylinder;
    SplineFaceFit cone;
};

struct AnalyzedFace {
    std::string id;
    std::string label;
    std::string solid_id;
    std::size_t index = 0;
    std::string surface_type;
    std::string support_kind;
    double area = 0.0;
    BBox bbox;
    bool exact_csg_supported = false;
    bool needs_spline_replacement = false;
    bool needs_surface_replacement = false;
    bool needs_emitter = false;
    SplineFitCandidates spline_fits;
    std::string name;
    std::string hierarchy_path;
};

struct GeometryCertificate {
    bool attempted = false;
    bool certified = false;
    std::size_t faces_checked = 0;
    std::size_t boolean_halfspace_checks = 0;
    double source_volume = 0.0;
    double maximum_outside_volume = 0.0;
    double allowed_outside_volume = 0.0;
    std::string failure_code;
};

struct ShellTopology {
    std::size_t index = 0;
    std::string role;
    std::size_t vertex_count = 0;
    std::size_t edge_count = 0;
    std::size_t face_count = 0;
    std::size_t face_inner_wire_count = 0;
    long long raw_euler_characteristic = 0;
    long long euler_characteristic = 0;
    bool closed = false;
    bool genus_known = false;
    long long genus = 0;
};

struct RecognitionCandidate {
    std::string kind;
    std::string strategy;
    std::string confidence;
    double score = 0.0;
    bool exact = false;
    std::vector<std::string> reason_codes;
};

struct PipePortTopology {
    std::string id;
    Vec3 center;
    Vec3 normal;
    double radius = 0.0;
};

struct PipeAnnulusTopology {
    bool candidate = false;
    bool conical = false;
    Vec3 axis_origin;
    Vec3 axis_direction;
    double inner_radius = 0.0;
    double outer_radius = 0.0;
    double top_inner_radius = 0.0;
    double top_outer_radius = 0.0;
    double axial_length = 0.0;
    std::size_t cylindrical_face_count = 0;
    std::size_t conical_face_count = 0;
    std::size_t planar_end_face_count = 0;
    std::vector<PipePortTopology> ports;
};

struct RevolutionTopology {
    bool candidate = false;
    bool full_revolution = false;
    Vec3 axis_origin;
    Vec3 axis_direction;
    double angular_extent = 0.0;
    std::size_t axial_level_count = 0;
    std::size_t cylindrical_face_count = 0;
    std::size_t conical_face_count = 0;
    std::size_t toroidal_face_count = 0;
    std::size_t spherical_face_count = 0;
    std::size_t radial_seam_face_count = 0;
};

struct SolidTopology {
    std::size_t vertex_count = 0;
    std::size_t edge_count = 0;
    std::size_t face_count = 0;
    std::size_t shell_count = 0;
    std::size_t closed_shell_count = 0;
    std::size_t cavity_shell_count = 0;
    std::size_t face_inner_wire_count = 0;
    std::size_t manifold_edge_count = 0;
    std::size_t boundary_edge_count = 0;
    std::size_t nonmanifold_edge_count = 0;
    std::size_t linear_edge_count = 0;
    std::size_t planar_face_count = 0;
    std::size_t concave_edge_count = 0;
    std::size_t convex_edge_count = 0;
    std::size_t tangent_or_unknown_edge_count = 0;
    long long raw_euler_characteristic = 0;
    long long euler_characteristic = 0;
    std::size_t unique_plane_count = 0;
    std::size_t normal_direction_count = 0;
    double bbox_fill_ratio = 0.0;
    bool prismatic_candidate = false;
    Vec3 prism_axis;
    std::size_t axial_level_count = 0;
    std::size_t axial_end_face_count = 0;
    std::size_t extrusion_profile_hole_count = 0;
    bool constant_cross_section_candidate = false;
    PipeAnnulusTopology pipe_annulus;
    RevolutionTopology revolution;
    std::vector<RecognitionCandidate> recognition_candidates;
    std::string selected_recognition_kind;
    std::string recommended_strategy;
    std::vector<std::string> reason_codes;
    std::vector<ShellTopology> shells;
};

struct ProvenanceRecord {
    std::string entity_id;
    std::string operation;
    std::vector<std::string> source_ids;
    bool exact = true;
    bool approximation = false;
    bool has_tolerance_budget = false;
    double tolerance_budget = 0.0;
};

struct ReplacementMetrics {
    bool available = false;
    double volume = 0.0;
    double absolute_delta = 0.0;
    double relative_delta = 0.0;
    double fill_ratio = 0.0;
};

enum class ReplacementOperation {
    bbox,
    bbox_cylinder,
    fitted_cylinder,
    fitted_sphere,
    fitted_cone,
};

struct ReplacementLimits {
    bool has_absolute_delta = false;
    double max_absolute_delta = 0.0;
    bool has_relative_delta = false;
    double max_relative_delta = 0.0;
};

struct ReplacementCheck {
    bool ok = false;
    ReplacementMetrics metrics;
    Diagnostic diagnostic;
};

struct FittedCylinderCandidate {
    ReplacementMetrics metrics;
    Vec3 axis_origin;
    Vec3 axis_direction;
    Vec3 base;
    Vec3 height;
    double radius = 0.0;
    double length = 0.0;
    double cylindrical_area = 0.0;
    std::string confidence;
};

struct FittedSphereCandidate {
    ReplacementMetrics metrics;
    Vec3 center;
    double radius = 0.0;
    double spherical_area = 0.0;
    std::string confidence;
};

struct FittedConeCandidate {
    ReplacementMetrics metrics;
    Vec3 axis_origin;
    Vec3 axis_direction;
    Vec3 base;
    Vec3 height;
    double base_radius = 0.0;
    double top_radius = 0.0;
    double length = 0.0;
    double conical_area = 0.0;
    std::string confidence;
};

struct AnalyzedSolid {
    std::string id;
    std::string label;
    ComponentInstanceId component_instance_id;
    ComponentDefinitionId component_definition_id;
    std::size_t index = 0;
    double volume = 0.0;
    double volume_fraction = 0.0;
    BBox bbox;
    double bbox_diagonal = 0.0;
    double bbox_diagonal_fraction = 0.0;
    double bbox_min_extent = 0.0;
    double bbox_mid_extent = 0.0;
    double bbox_max_extent = 0.0;
    double bbox_aspect_ratio = 0.0;
    bool small_volume_candidate = false;
    bool small_bbox_candidate = false;
    bool thin_plate_candidate = false;
    bool long_body_candidate = false;
    ReplacementMetrics bbox_replacement;
    ReplacementMetrics bbox_cylinder_replacement;
    FittedCylinderCandidate fitted_cylinder_replacement;
    FittedSphereCandidate fitted_sphere_replacement;
    FittedConeCandidate fitted_cone_replacement;
    std::size_t face_count = 0;
    bool exact_csg_supported = false;
    bool needs_decomposition = false;
    bool needs_spline_replacement = false;
    bool needs_surface_replacement = false;
    bool needs_user_decision = false;
    // The lean fixed-point pass may postpone curved face/half-space OCCT
    // Booleans. This is never an exact result and cannot pass emission.
    bool convexity_certificate_deferred = false;
    GeometryCertificate certificate;
    SolidTopology topology;
    std::vector<std::string> face_ids;
    std::string name;
    std::string hierarchy_path;
};

struct AnalyzedDecomposedPiece {
    std::string id;
    std::string label;
    std::string source_solid_id;
    std::size_t index = 0;
    double volume = 0.0;
    BBox bbox;
    std::size_t face_count = 0;
    bool exact_csg_supported = false;
    bool needs_decomposition = false;
    bool needs_spline_replacement = false;
    bool needs_surface_replacement = false;
    bool needs_user_decision = false;
    GeometryCertificate certificate;
};

struct DecompositionCandidatePiece {
    std::size_t index = 0;
    double volume = 0.0;
    double volume_fraction = 0.0;
    BBox bbox;
    std::size_t face_count = 0;
    bool exact_csg_supported = false;
    bool needs_decomposition = false;
    bool needs_spline_replacement = false;
    bool needs_surface_replacement = false;
    bool needs_user_decision = false;
    std::size_t halfspace_violation_count = 0;
};

struct DecompositionCandidate {
    std::string id;
    std::string target_id;
    // For face-derived candidates (tool_kind starts with `face_` or
    // `torus_axis_plane`) this points to an existing face id on the
    // target solid/piece. For synthetic-plane candidates (tool_kind
    // `bbox_plane_x|y|z`) source_face_id is empty and the geometry is
    // carried by plane_origin / plane_normal below.
    std::string source_face_id;
    std::string source_face_label;
    std::string surface_type;
    std::string tool_kind;
    std::string reason;
    // Explicit plane parameters for synthetic-plane candidates. Valid
    // when the dispatcher must construct the cutter from coordinates
    // rather than from a face. plane_valid implies plane_origin and
    // plane_normal are populated. Used for single-tool plane cuts; for
    // multi-tool cuts (several planes applied simultaneously to one
    // BRepAlgoAPI_Splitter call) see `planes` below.
    bool plane_valid = false;
    Vec3 plane_origin;
    Vec3 plane_normal;
    // Multi-tool plane cutter. When non-empty, the dispatcher routes
    // through decompose_solid_by_planes which hands all of these to
    // OCCT's splitter in one call. Higher first-cut yield on parts
    // with multiple parallel concavities (rails, slotted brackets,
    // E-shaped frames). plane_valid/plane_origin/plane_normal are
    // ignored when this vector is non-empty.
    std::vector<PlaneSpec> planes;
    bool usable = false;
    std::string confidence;
    double score = 0.0;
    std::string rejection_reason;
    std::size_t piece_count = 0;
    std::size_t exact_piece_count = 0;
    std::size_t pieces_requiring_decomposition = 0;
    std::size_t pieces_requiring_spline_replacement = 0;
    std::size_t pieces_requiring_user_decision = 0;
    std::size_t sliver_piece_count = 0;
    double original_volume = 0.0;
    double pieces_volume = 0.0;
    double absolute_delta = 0.0;
    double relative_delta = 0.0;
    double min_piece_volume_fraction = 0.0;
    double max_piece_volume_fraction = 0.0;
    double volume_balance = 0.0;
    std::size_t parent_halfspace_violation_count = 0;
    std::size_t max_piece_halfspace_violation_count = 0;
    double progress_ratio = 0.0;
    std::vector<DecompositionCandidatePiece> pieces;
    std::vector<Diagnostic> diagnostics;
};

struct DecompositionProposal {
    bool ok = false;
    std::string target_id;
    std::string best_candidate_id;
    std::vector<DecompositionCandidate> candidates;
    std::vector<Diagnostic> diagnostics;
};

struct RemovalCandidate {
    std::string id;
    std::string solid_id;
    ComponentInstanceId component_instance_id;
    ComponentDefinitionId component_definition_id;
    std::string label;
    std::string name;
    std::string hierarchy_path;
    double volume = 0.0;
    double volume_fraction = 0.0;
    BBox bbox;
    double bbox_diagonal_fraction = 0.0;
    std::vector<std::string> reason_codes;
    std::vector<std::string> blockers;
    std::string confidence;
    double score = 0.0;
    std::string suggested_action;
};

struct RemovalProposal {
    bool ok = false;
    std::vector<RemovalCandidate> candidates;
    std::vector<Diagnostic> diagnostics;
    double total_volume = 0.0;
    double high_confidence_volume = 0.0;
    double high_confidence_volume_fraction = 0.0;
    std::size_t high_confidence_count = 0;
};

enum class HoleFillKind {
    cylindrical_through,
    cylindrical_blind,
    coaxial_stack,
    planar_wire_patch,
    unknown,
};

struct HoleFillOptions {
    std::string host_solid_id;
    std::string near_removed_solid_id;
    bool has_max_radius = false;
    double max_radius = 0.0;
    bool has_max_volume_fraction = false;
    double max_volume_fraction = 0.0;
    std::size_t max_candidates = 64;
};

struct HoleFillCandidate {
    std::string id;
    std::string host_solid_id;
    std::vector<std::string> seed_removed_solid_ids;
    std::vector<std::string> face_ids;
    HoleFillKind kind = HoleFillKind::unknown;
    Vec3 axis_origin;
    Vec3 axis_direction;
    Vec3 base;
    Vec3 height;
    double radius = 0.0;
    double top_radius = 0.0;
    double length = 0.0;
    double plug_volume = 0.0;
    double host_volume = 0.0;
    double plug_volume_fraction_of_host = 0.0;
    double estimated_host_overlap_fraction = 0.0;
    double estimated_other_overlap_fraction = 0.0;
    std::string confidence;
    double score = 0.0;
    std::vector<std::string> reason_codes;
    std::vector<std::string> blockers;
    std::string suggested_action;
};

struct DecisionMetric {
    std::string name;
    double value = 0.0;
    std::string unit;
};

struct DecisionCandidateView {
    std::string id;
    std::string action;
    std::string confidence;
    std::string reason;
    std::vector<DecisionMetric> metrics;
};

struct DecisionRequest {
    std::string id;
    std::string kind;
    std::string target_id;
    std::string reason;
    std::string recommended_action;
    std::string summary;
    std::vector<DecisionMetric> metrics;
    std::vector<DecisionCandidateView> candidates;
};

struct HoleFillProposal {
    bool ok = false;
    std::vector<HoleFillCandidate> candidates;
    std::vector<DecisionRequest> decision_requests;
    std::vector<Diagnostic> diagnostics;
};

struct MediumGenerationProposal {
    bool ok = false;
    bool conical = false;
    MediumGenerationRequest request;
    std::string host_shape_id;
    std::string host_component_id;
    Vec3 base;
    Vec3 height;
    double radius = 0.0;
    double top_radius = 0.0;
    MediumNetworkTopology network;
    DerivedRegionCertificate certificate;
    std::vector<DecisionRequest> decision_requests;
    std::vector<Diagnostic> diagnostics;
};

struct SurfaceReplacementCandidate {
    std::string id;
    std::string face_id;
    std::string owner_target_id;
    std::string kind;
    std::string action;
    std::string confidence;
    std::string reason;
    bool exact = false;
    bool deviation_certified = false;
    bool usable = false;
    double score = 0.0;
    double max_deviation = 0.0;
    double rms_deviation = 0.0;
    double relative_max_deviation = 0.0;
    double owner_relative_volume_delta = 0.0;
    std::size_t segment_count = 0;
    Vec3 origin;
    Vec3 direction;
    double radius = 0.0;
    double half_angle = 0.0;
    std::vector<Diagnostic> diagnostics;
};

struct SurfaceReplacementProposal {
    bool ok = false;
    std::string face_id;
    std::string owner_target_id;
    std::string support_kind;
    std::string best_candidate_id;
    std::vector<SurfaceReplacementCandidate> candidates;
    std::vector<Diagnostic> diagnostics;
};

struct DecompositionPolicy {
    bool allow_high_confidence_face_auto = true;
    bool allow_bbox_fallback_auto = false;
    bool allow_medium_confidence_auto = false;
    bool allow_low_confidence_auto = false;
    double max_relative_delta = 1.0e-8;
    double min_piece_volume_fraction = 1.0e-8;
    std::size_t max_splits_per_target = 16;
};

struct AssemblyNode {
    std::string id;
    std::string parent_id;
    std::string name;
    std::string label_entry;
    std::string shape_id;
    std::string kind;
    std::string path;
    // Additive semantic identity fields. Legacy asm:N IDs remain available
    // while definitions and occurrences gain stable, source-derived IDs.
    std::string definition_entry;
    ComponentDefinitionId definition_id;
    ComponentInstanceId instance_id;
    std::string geometry_digest;
    bool geometry_digest_verified = false;
    InstanceTransform transform;
    MetadataId metadata_id = kInvalidMetadataId;
};

struct PerformanceMetrics {
    double load_step_ms = 0.0;
    double analyze_ms = 0.0;
    double propose_ms = 0.0;
    double split_ms = 0.0;
    double emit_ms = 0.0;
    double export_step_ms = 0.0;
    std::size_t candidates_evaluated = 0;
    std::size_t splitter_calls = 0;
    std::size_t pieces_produced = 0;
    std::size_t spatial_candidate_pairs = 0;
    std::size_t exact_overlap_tests = 0;
    // Recognition is computed once per verified geometry definition and then
    // transported to rigidly transformed occurrences of that definition.
    std::size_t definition_recognition_cache_hits = 0;
    std::size_t definition_recognition_cache_misses = 0;
    std::size_t definition_conversion_cache_hits = 0;
    std::size_t definition_conversion_cache_published = 0;
    // Fixed-point decomposition diagnostics. Exposing local proof and global
    // reconciliation costs guards against accidentally reintroducing copied
    // whole-model candidate trials.
    double local_analysis_ms = 0.0;
    double local_certificate_ms = 0.0;
    double local_overlap_ms = 0.0;
    double full_reconciliation_ms = 0.0;
    double full_analysis_ms = 0.0;
    double full_overlap_ms = 0.0;
    double full_ledger_ms = 0.0;
    std::size_t local_trials_generated = 0;
    std::size_t local_trials_certified = 0;
    std::size_t local_trials_rejected = 0;
    std::size_t parallel_plane_batches = 0;
    std::size_t local_overlap_candidates = 0;
    std::size_t local_exact_overlap_tests = 0;
    std::size_t local_overlap_containment_proofs = 0;
    std::size_t overlap_pair_cache_hits = 0;
    std::size_t overlap_pair_cache_misses = 0;
    std::size_t local_partition_boolean_calls = 0;
    std::size_t body_certificate_cache_hits = 0;
    std::size_t body_certificate_cache_misses = 0;
    std::size_t full_reconciliations = 0;
    std::size_t whole_model_trial_copies = 0;
    std::size_t peak_local_pieces = 0;
};

struct ConversionOptions {
    double linear_tolerance = 1.0e-7;
    double angular_tolerance = 1.0e-8;
    double small_volume_fraction = 1.0e-4;
    double small_bbox_fraction = 1.0e-2;
    double thin_plate_ratio = 1.0e-2;
    double long_body_ratio = 10.0;
    int default_material_id = 1;
    double default_density = 0.0;
    ApproximationPolicy approximation;
    bool heal_shape = true;
    bool require_convex_cells = true;
    // Detailed topology probes, face areas/bboxes, spline fit proposals, and
    // fitted primitive replacement candidates are useful for interactive
    // inspection but are not inputs to exact fixed-point decomposition. The
    // coordinator disables them on its internal passes for large-model
    // throughput. Exact convexity and partition certificates remain enabled.
    bool analyze_topology_metrics = true;
    bool analyze_face_metrics = true;
    bool analyze_fitted_replacements = true;
    bool defer_curved_halfspace_certificates = false;
    // Geometry mutation belongs in explicit preprocessing operations.  Emit
    // is pure by default; the legacy transient planar split remains available
    // only to callers that deliberately opt in.
    bool try_planar_decomposition = false;
    // Decomposed pieces are first reunited and Boolean-simplified.  Only a
    // genuine union that cannot be reduced to one half-space intersection is
    // split into separate output cells.
    bool split_union_cells = true;
    // Dimensionless conservation guard shared by directed planar/face/plane
    // decomposition. A split that exceeds it is rejected before mutation.
    double decomposition_relative_volume_tolerance = 1.0e-6;
    // Exhaustive surface-driven subdivision (the `shatter` op). Each solid
    // is cut by the bounded cutter of every one of its own faces until each
    // piece lies on one definite side of every surface. `shatter_max_depth`
    // bounds the recursion that re-feeds any piece still failing convex
    // validation. `shatter_relative_volume_tolerance` is dimensionless and
    // independently controls the volume-conservation guard;
    // `allow_shatter_volume_drift` explicitly overrides that guard. When
    // Group pieces sharing a source_solid_id into one Alea union before final
    // simplification.  This lets complementary artificial cut surfaces cancel
    // and coalesces every union that is representable as one intersection.
    // Set false only when deliberately preserving the raw partition.
    int shatter_max_depth = 8;
    double shatter_relative_volume_tolerance = 1.0e-7;
    bool allow_shatter_volume_drift = false;
    bool union_decomposed_pieces = true;
    std::size_t prismatic_max_pieces = 4096;
    std::size_t prismatic_max_splitter_calls = 10000;
    // Optional read-only analysis filter. IDs retain their original solid:N
    // numbering; conversion and mutation never consult this field.
    std::vector<std::string> analysis_target_ids;
    // Overlap detection. Runs during analyze; emits `solid_overlap`
    // diagnostics for pairs whose intersection volume exceeds
    // overlap_volume_threshold (absolute) OR overlap_volume_fraction
    // of the smaller solid (whichever is larger). Disable with
    // check_overlaps=false on huge assemblies where the bbox prescreen
    // does not help. Pairs are reported in report.overlaps[].
    //
    // Severity is `error` by default -- MCNP cannot tolerate
    // overlapping cells, so report.ok() returns false and
    // emit_document refuses to write the MCNP deck. Set
    // allow_overlaps=true (or pass --allow-overlaps on the CLI) to
    // downgrade both `solid_overlap` and `mutation_introduced_overlap`
    // to warnings; only use this if you understand the consequences
    // (e.g., during incremental simplification where the overlap is
    // expected to be resolved by a later op).
    bool check_overlaps = true;
    // Independent OCCT Common operations are evaluated concurrently, while
    // results are merged in deterministic pair order. Keep this bounded: one
    // Boolean can itself use substantial memory on ITER-scale components.
    std::size_t overlap_parallelism = 4;
    bool allow_overlaps = false;
    double overlap_volume_threshold = 1.0e-9;
    double overlap_volume_fraction = 1.0e-6;
    // Optional non-emitting container used to prove modeled-domain coverage.
    // The selected active body is excluded from MCNP material-cell emission.
    std::string mcnp_domain_id;
    bool require_domain_coverage = false;
    // Unit overrides applied at STEP import. Per docs/PHILOSOPHY.md
    // ("make failure visible"), the default is report-only: the
    // importer captures the declared STEP unit but does not silently
    // rescale geometry. The user opts into rescaling explicitly via
    // --normalize-units, and the change is recorded in the decision
    // log when one is attached.
    std::string unit_override;        // assert the source unit (no rescale)
    double unit_scale_to_mm_override = 0.0;  // assert scale (no rescale)
    std::string normalize_to_unit;    // actually rescale geometry to this unit
};

// One detected pair of overlapping solids. Reported by the analyze
// stage. `intersection_volume` is in the report's length unit cubed.
// `min_fraction` is intersection_volume / min(volume(a), volume(b)) --
// the worst-case "how much of the smaller solid is double-counted".
struct SolidOverlap {
    std::string solid_a_id;
    std::string solid_b_id;
    double volume_a = 0.0;
    double volume_b = 0.0;
    double intersection_volume = 0.0;
    double min_fraction = 0.0;
};

enum class CoverageStatus { not_configured, certified, gap, unknown };

struct McnpGeometrySafetyReport {
    bool validated = false;
    bool safe_to_emit = false;
    std::size_t overlap_count = 0;
    std::size_t overlap_unknown_count = 0;
    CoverageStatus coverage_status = CoverageStatus::not_configured;
    std::string domain_id;
    double domain_volume = 0.0;
    double uncovered_volume = 0.0;
    double outside_domain_volume = 0.0;
    std::string failure_code;
};

struct AleaHandoffSummary {
    bool attempted = false;
    bool compatible = false;
    std::size_t schema_major = 0;
    std::size_t schema_minor = 0;
    std::string libalea_version;
    std::string contract_digest;
    std::size_t cell_count = 0;
    std::size_t surface_count = 0;
    std::size_t universe_count = 0;
    std::size_t sidecar_entity_count = 0;
    std::size_t chunk_count = 0;
    std::vector<std::string> missing_capabilities;
};

struct ConversionReport {
    // Stable machine-readable contract version. Increment only for breaking
    // JSON/report-shape changes; additive fields keep the same version.
    std::size_t schema_version = 1;
    // Monotonic session revision of the document represented by this report.
    std::size_t document_revision = 0;
    std::string document_id;
    std::string source_path;
    // STEP length unit metadata. `length_unit` is a short label
    // ("mm", "cm", "m", "in", "ft", ...); `length_unit_to_mm` is the
    // factor that converts a value in `length_unit` to millimetres.
    // Default 1.0 / "mm" matches OCCT's internal representation: when
    // STEP units are unknown the model stays in OCCT's native units
    // and no rescaling happens.
    std::string length_unit;
    double length_unit_to_mm = 1.0;
    std::vector<Diagnostic> diagnostics;
    std::size_t solids_seen = 0;
    std::size_t solids_converted = 0;
    std::size_t solids_requiring_decomposition = 0;
    std::size_t decomposition_attempts = 0;
    std::size_t decomposition_pieces = 0;
    std::size_t faces_seen = 0;
    std::size_t analytic_faces = 0;
    std::size_t spline_faces = 0;
    double total_volume = 0.0;
    std::vector<SolidVolume> solid_volumes;
    std::vector<AnalyzedSolid> solids;
    std::vector<AnalyzedFace> faces;
    std::vector<AnalyzedDecomposedPiece> decomposed_pieces;
    std::vector<AssemblyNode> assembly;
    StringTable strings;
    std::vector<EntityMetadata> metadata;
    std::vector<ComponentDefinition> component_definitions;
    std::vector<ComponentInstance> component_instances;
    std::vector<MaterialDefinition> materials;
    std::vector<MediumGenerationRequest> medium_requests;
    std::vector<DerivedRegionRecord> derived_regions;
    std::vector<DecisionRequest> decision_requests;
    std::vector<SolidOverlap> overlaps;
    std::vector<ProvenanceRecord> provenance;
    ApproximationPolicy approximation_policy;
    ApproximationLedger approximation_ledger;
    McnpGeometrySafetyReport mcnp_geometry_safety;
    AleaHandoffSummary alea_handoff;
    PerformanceMetrics performance;

    bool ok() const;
};

struct VolumeComparison {
    ConversionReport original;
    ConversionReport candidate;
    double absolute_delta = 0.0;
    double relative_delta = 0.0;
    double absolute_tolerance = 0.0;
    double relative_tolerance = 0.0;
    bool within_tolerance = false;
};

struct AutoFitSummary {
    std::size_t spline_faces_seen = 0;
    std::size_t applied = 0;
    std::size_t skipped_no_fit = 0;
    std::size_t skipped_over_budget = 0;
};

int write_report_json(const ConversionReport& report, const std::string& output_path);
std::string report_json_string(const ConversionReport& report);
int write_decomposition_proposal_json(const DecompositionProposal& proposal,
                                      const std::string& output_path);
std::string decomposition_proposal_json_string(const DecompositionProposal& proposal);
int write_removal_proposal_json(const RemovalProposal& proposal,
                                const std::string& output_path);
std::string removal_proposal_json_string(const RemovalProposal& proposal);
int write_hole_fill_proposal_json(const HoleFillProposal& proposal,
                                  const std::string& output_path);
std::string hole_fill_proposal_json_string(const HoleFillProposal& proposal);
int write_surface_replacement_proposal_json(
    const SurfaceReplacementProposal& proposal,
    const std::string& output_path);
std::string surface_replacement_proposal_json_string(
    const SurfaceReplacementProposal& proposal);

} // namespace csgify

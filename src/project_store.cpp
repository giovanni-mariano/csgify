#include "csgify/project_store.hpp"

#include "csgify/engine.hpp"
#include "fnv.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <utility>

#include <sqlite3.h>
#include <unistd.h>

namespace csgify {
namespace {

std::atomic<std::uint64_t> next_temporary_id{1};

void set_error(std::string* error, const std::string& value) {
    if (error) *error = value;
}

std::string digest_text(const std::string& value) {
    std::uint64_t low = kFnv1aOffset64;
    constexpr std::uint64_t high_seed = 0x2523224284e89cdfULL;
    std::uint64_t high = high_seed;
    for (const unsigned char byte : value) {
        low ^= byte;
        low *= kFnv1aPrime64;
        high ^= byte;
        high *= kFnv1aPrime64;
    }
    return to_hex64(low) + to_hex64(high);
}

std::string recognition_policy_digest(const ConversionOptions& options) {
    std::ostringstream policy;
    policy << std::setprecision(17)
           << "definition-recognition-v1|linear=" << options.linear_tolerance
           << "|angular=" << options.angular_tolerance
           << "|small_volume=" << options.small_volume_fraction
           << "|small_bbox=" << options.small_bbox_fraction
           << "|thin_plate=" << options.thin_plate_ratio
           << "|long_body=" << options.long_body_ratio
           << "|require_convex=" << options.require_convex_cells;
    return digest_text(policy.str());
}

std::string recognition_certificate_json(const ComponentDefinition& definition,
                                         const AnalyzedSolid& solid,
                                         const std::string& policy_digest) {
    const SolidTopology& topology = solid.topology;
    std::ostringstream out;
    out << std::setprecision(17)
        << "{\n  \"schema\": \"csgify-definition-recognition-v1\",\n"
        << "  \"geometry_digest\": \"" << definition.geometry_digest << "\",\n"
        << "  \"policy_digest\": \"" << policy_digest << "\",\n"
        << "  \"topology\": {\n"
        << "    \"vertices\": " << topology.vertex_count << ",\n"
        << "    \"edges\": " << topology.edge_count << ",\n"
        << "    \"faces\": " << topology.face_count << ",\n"
        << "    \"shells\": " << topology.shell_count << ",\n"
        << "    \"cavity_shells\": " << topology.cavity_shell_count << ",\n"
        << "    \"concave_edges\": " << topology.concave_edge_count << ",\n"
        << "    \"face_inner_wires\": " << topology.face_inner_wire_count << ",\n"
        << "    \"bbox_fill_ratio\": " << topology.bbox_fill_ratio << ",\n"
        << "    \"selected_kind\": \"" << topology.selected_recognition_kind << "\",\n"
        << "    \"recommended_strategy\": \"" << topology.recommended_strategy << "\",\n"
        << "    \"prismatic\": " << (topology.prismatic_candidate ? "true" : "false") << ",\n"
        << "    \"constant_cross_section\": "
        << (topology.constant_cross_section_candidate ? "true" : "false") << ",\n"
        << "    \"pipe_annulus\": " << (topology.pipe_annulus.candidate ? "true" : "false") << ",\n"
        << "    \"pipe_conical\": " << (topology.pipe_annulus.conical ? "true" : "false") << ",\n"
        << "    \"pipe_inner_radius\": " << topology.pipe_annulus.inner_radius << ",\n"
        << "    \"pipe_outer_radius\": " << topology.pipe_annulus.outer_radius << ",\n"
        << "    \"pipe_top_inner_radius\": " << topology.pipe_annulus.top_inner_radius << ",\n"
        << "    \"pipe_top_outer_radius\": " << topology.pipe_annulus.top_outer_radius << ",\n"
        << "    \"pipe_axial_length\": " << topology.pipe_annulus.axial_length << ",\n"
        << "    \"pipe_port_count\": " << topology.pipe_annulus.ports.size() << ",\n"
        << "    \"revolution\": " << (topology.revolution.candidate ? "true" : "false") << ",\n"
        << "    \"full_revolution\": " << (topology.revolution.full_revolution ? "true" : "false") << ",\n"
        << "    \"revolution_angular_extent\": " << topology.revolution.angular_extent << ",\n"
        << "    \"revolution_axial_levels\": " << topology.revolution.axial_level_count << ",\n"
        << "    \"revolution_cylindrical_faces\": " << topology.revolution.cylindrical_face_count << ",\n"
        << "    \"revolution_conical_faces\": " << topology.revolution.conical_face_count << ",\n"
        << "    \"revolution_toroidal_faces\": " << topology.revolution.toroidal_face_count << "\n"
        << "  },\n"
        << "  \"certificate\": {\n"
        << "    \"exact_csg_supported\": " << (solid.exact_csg_supported ? "true" : "false") << ",\n"
        << "    \"needs_decomposition\": " << (solid.needs_decomposition ? "true" : "false") << ",\n"
        << "    \"faces_checked\": " << solid.certificate.faces_checked << ",\n"
        << "    \"boolean_halfspace_checks\": "
        << solid.certificate.boolean_halfspace_checks << "\n"
        << "  }\n}\n";
    return out.str();
}

std::string normalized_extension(const std::string& extension) {
    std::string result;
    result.reserve(extension.size() + 1);
    for (const unsigned char ch : extension) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
            || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-') {
            result.push_back(static_cast<char>(ch));
        }
    }
    if (result.empty()) return {};
    return "." + result;
}

bool digest_file(const std::filesystem::path& path,
                 std::string* digest,
                 std::uint64_t* size,
                 std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        set_error(error, "cannot open artifact for hashing: " + path.string());
        return false;
    }
    std::uint64_t low = kFnv1aOffset64;
    constexpr std::uint64_t high_seed = 0x2523224284e89cdfULL;
    std::uint64_t high = high_seed;
    std::uint64_t total = 0;
    char buffer[64 * 1024];
    while (in) {
        in.read(buffer, sizeof(buffer));
        const std::streamsize count = in.gcount();
        for (std::streamsize i = 0; i < count; ++i) {
            const auto byte = static_cast<unsigned char>(buffer[i]);
            low ^= byte;
            low *= kFnv1aPrime64;
            high ^= byte;
            high *= kFnv1aPrime64;
        }
        total += static_cast<std::uint64_t>(count);
    }
    if (!in.eof()) {
        set_error(error, "failed while hashing artifact: " + path.string());
        return false;
    }
    *digest = to_hex64(low) + to_hex64(high);
    *size = total;
    return true;
}

std::filesystem::path temporary_path(const std::filesystem::path& root,
                                     const std::string& digest) {
    std::ostringstream name;
    name << digest << '.' << static_cast<unsigned long>(::getpid()) << '.'
         << next_temporary_id.fetch_add(1) << ".tmp";
    return root / "tmp" / name.str();
}

bool commit_temporary(const std::filesystem::path& root,
                      const std::filesystem::path& temporary,
                      const std::string& kind,
                      const std::string& extension,
                      const std::string& digest,
                      std::uint64_t size,
                      ArtifactRecord* record,
                      std::string* error) {
    const std::string suffix = normalized_extension(extension);
    const std::filesystem::path relative =
        std::filesystem::path("objects") / digest.substr(0, 2)
        / (digest + suffix);
    const std::filesystem::path target = root / relative;
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) {
        set_error(error, "cannot create artifact directory: " + ec.message());
        std::filesystem::remove(temporary, ec);
        return false;
    }

    bool deduplicated = false;
    if (std::filesystem::exists(target, ec) && !ec) {
        std::string existing_digest;
        std::uint64_t existing_size = 0;
        if (!digest_file(target, &existing_digest, &existing_size, error)
            || existing_digest != digest || existing_size != size) {
            set_error(error, "existing content-addressed artifact is corrupt: "
                             + target.string());
            std::filesystem::remove(temporary, ec);
            return false;
        }
        deduplicated = true;
        std::filesystem::remove(temporary, ec);
    } else {
        std::filesystem::rename(temporary, target, ec);
        if (ec) {
            // Another process may have won the same publication race.
            if (!std::filesystem::exists(target)) {
                set_error(error, "cannot atomically publish artifact: " + ec.message());
                std::filesystem::remove(temporary, ec);
                return false;
            }
            std::string existing_digest;
            std::uint64_t existing_size = 0;
            if (!digest_file(target, &existing_digest, &existing_size, error)
                || existing_digest != digest || existing_size != size) {
                std::filesystem::remove(temporary, ec);
                return false;
            }
            deduplicated = true;
            std::filesystem::remove(temporary, ec);
        }
    }

    if (record) {
        record->digest_algorithm = "fnv1a128";
        record->digest = digest;
        record->kind = kind;
        record->relative_path = relative.generic_string();
        record->size = size;
        record->deduplicated = deduplicated;
    }
    return true;
}

} // namespace

ArtifactStore::ArtifactStore(std::filesystem::path root) : root_(std::move(root)) {}

bool ArtifactStore::initialize(std::string* error) const {
    std::error_code ec;
    std::filesystem::create_directories(root_ / "objects", ec);
    if (!ec) std::filesystem::create_directories(root_ / "tmp", ec);
    if (ec) {
        set_error(error, "cannot initialize artifact store: " + ec.message());
        return false;
    }
    return true;
}

bool ArtifactStore::publish_bytes(const std::string& kind,
                                  const std::string& extension,
                                  const std::string& contents,
                                  ArtifactRecord* record,
                                  std::string* error) const {
    if (!initialize(error)) return false;
    const std::string digest =
        fnv1a128_hex(fnv1a128(contents.data(), contents.size()));
    const std::filesystem::path temporary = temporary_path(root_, digest);
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            set_error(error, "cannot create temporary artifact: " + temporary.string());
            return false;
        }
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        out.flush();
        if (!out) {
            set_error(error, "cannot write temporary artifact: " + temporary.string());
            std::error_code ec;
            std::filesystem::remove(temporary, ec);
            return false;
        }
    }
    std::string written_digest;
    std::uint64_t written_size = 0;
    if (!digest_file(temporary, &written_digest, &written_size, error)
        || written_digest != digest
        || written_size != contents.size()) {
        std::error_code ec;
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return commit_temporary(root_, temporary, kind, extension, digest,
                            written_size, record, error);
}

bool ArtifactStore::publish_file(const std::string& kind,
                                 const std::string& extension,
                                 const std::filesystem::path& source,
                                 ArtifactRecord* record,
                                 std::string* error) const {
    if (!initialize(error)) return false;
    std::string digest;
    std::uint64_t size = 0;
    if (!digest_file(source, &digest, &size, error)) return false;
    const std::filesystem::path temporary = temporary_path(root_, digest);
    std::error_code ec;
    std::filesystem::copy_file(source, temporary,
                               std::filesystem::copy_options::none, ec);
    if (ec) {
        set_error(error, "cannot copy artifact into store: " + ec.message());
        return false;
    }
    std::string copied_digest;
    std::uint64_t copied_size = 0;
    if (!digest_file(temporary, &copied_digest, &copied_size, error)
        || copied_digest != digest || copied_size != size) {
        std::filesystem::remove(temporary, ec);
        set_error(error, "artifact changed or was corrupted while being copied");
        return false;
    }
    return commit_temporary(root_, temporary, kind, extension, digest, size,
                            record, error);
}

bool ArtifactStore::validate(const ArtifactRecord& record,
                             std::string* error) const {
    if (record.digest_algorithm != "fnv1a128") {
        set_error(error, "unsupported artifact digest algorithm: "
                         + record.digest_algorithm);
        return false;
    }
    const std::filesystem::path path = absolute_path(record);
    std::string digest;
    std::uint64_t size = 0;
    if (!digest_file(path, &digest, &size, error)) return false;
    if (digest != record.digest || size != record.size) {
        set_error(error, "artifact digest or size mismatch: " + path.string());
        return false;
    }
    return true;
}

std::filesystem::path ArtifactStore::absolute_path(
    const ArtifactRecord& record) const {
    const std::filesystem::path relative(record.relative_path);
    if (relative.is_absolute()) return {};
    for (const auto& component : relative) {
        if (component == "..") return {};
    }
    return root_ / relative;
}

struct ProjectStore::Impl {
    sqlite3* database = nullptr;

    ~Impl() {
        if (database) sqlite3_close(database);
    }
};

namespace {

class Statement {
public:
    Statement(sqlite3* database, const char* sql) : database_(database) {
        if (sqlite3_prepare_v2(database, sql, -1, &statement_, nullptr) != SQLITE_OK) {
            statement_ = nullptr;
        }
    }
    ~Statement() { if (statement_) sqlite3_finalize(statement_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    bool valid() const { return statement_ != nullptr; }
    sqlite3_stmt* get() const { return statement_; }
    const char* error() const { return sqlite3_errmsg(database_); }

private:
    sqlite3* database_ = nullptr;
    sqlite3_stmt* statement_ = nullptr;
};

bool execute_sql(sqlite3* database, const char* sql, std::string* error) {
    char* sqlite_error = nullptr;
    const int status = sqlite3_exec(database, sql, nullptr, nullptr, &sqlite_error);
    if (status == SQLITE_OK) return true;
    set_error(error, sqlite_error ? sqlite_error : sqlite3_errmsg(database));
    sqlite3_free(sqlite_error);
    return false;
}

bool bind_text(sqlite3_stmt* statement, int index, const std::string& value) {
    return sqlite3_bind_text(statement, index, value.c_str(),
                             static_cast<int>(value.size()), SQLITE_TRANSIENT)
        == SQLITE_OK;
}

bool step_done(Statement& statement, std::string* error) {
    if (!statement.valid()) {
        set_error(error, statement.error());
        return false;
    }
    if (sqlite3_step(statement.get()) == SQLITE_DONE) return true;
    set_error(error, statement.error());
    return false;
}

std::string column_text(sqlite3_stmt* statement, int column) {
    const unsigned char* value = sqlite3_column_text(statement, column);
    return value ? reinterpret_cast<const char*>(value) : std::string{};
}

std::string transform_text(const InstanceTransform& transform) {
    std::ostringstream out;
    out << std::setprecision(17);
    for (std::size_t i = 0; i < transform.values.size(); ++i) {
        if (i) out << ',';
        out << transform.values[i];
    }
    return out.str();
}

bool valid_spatial_bounds(const SpatialBounds& bounds) {
    return bounds.valid
        && std::isfinite(bounds.xmin) && std::isfinite(bounds.ymin)
        && std::isfinite(bounds.zmin) && std::isfinite(bounds.xmax)
        && std::isfinite(bounds.ymax) && std::isfinite(bounds.zmax)
        && bounds.xmin <= bounds.xmax
        && bounds.ymin <= bounds.ymax
        && bounds.zmin <= bounds.zmax;
}

constexpr int kProjectSchemaVersion = 8;

const char* kProjectSchema = R"sql(
CREATE TABLE IF NOT EXISTS source_documents (
    source_id INTEGER PRIMARY KEY,
    source_path TEXT NOT NULL,
    source_digest TEXT NOT NULL UNIQUE
);
CREATE TABLE IF NOT EXISTS interned_strings (
    source_id INTEGER NOT NULL REFERENCES source_documents(source_id) ON DELETE CASCADE,
    string_id INTEGER NOT NULL,
    value TEXT NOT NULL,
    PRIMARY KEY(source_id, string_id)
);
CREATE TABLE IF NOT EXISTS metadata_entities (
    source_id INTEGER NOT NULL REFERENCES source_documents(source_id) ON DELETE CASCADE,
    metadata_id INTEGER NOT NULL,
    stable_name INTEGER NOT NULL,
    display_name INTEGER NOT NULL,
    description INTEGER NOT NULL,
    engineering_role INTEGER NOT NULL,
    subsystem INTEGER NOT NULL,
    source_cad_label INTEGER NOT NULL,
    source_component_path INTEGER NOT NULL,
    PRIMARY KEY(source_id, metadata_id)
);
CREATE TABLE IF NOT EXISTS metadata_tags (
    source_id INTEGER NOT NULL,
    metadata_id INTEGER NOT NULL,
    ordinal INTEGER NOT NULL,
    string_id INTEGER NOT NULL,
    PRIMARY KEY(source_id, metadata_id, ordinal),
    FOREIGN KEY(source_id, metadata_id)
        REFERENCES metadata_entities(source_id, metadata_id) ON DELETE CASCADE
);
CREATE TABLE IF NOT EXISTS annotations (
    source_id INTEGER NOT NULL,
    metadata_id INTEGER NOT NULL,
    ordinal INTEGER NOT NULL,
    key_string_id INTEGER NOT NULL,
    value_string_id INTEGER NOT NULL,
    origin TEXT NOT NULL,
    review_state TEXT NOT NULL,
    user_verified INTEGER NOT NULL,
    PRIMARY KEY(source_id, metadata_id, ordinal),
    FOREIGN KEY(source_id, metadata_id)
        REFERENCES metadata_entities(source_id, metadata_id) ON DELETE CASCADE
);
CREATE TABLE IF NOT EXISTS component_definitions (
    source_id INTEGER NOT NULL REFERENCES source_documents(source_id) ON DELETE CASCADE,
    definition_id TEXT NOT NULL,
    source_definition_key INTEGER NOT NULL,
    geometry_digest TEXT NOT NULL,
    geometry_digest_verified INTEGER NOT NULL,
    metadata_id INTEGER NOT NULL,
    PRIMARY KEY(source_id, definition_id)
);
CREATE TABLE IF NOT EXISTS component_instances (
    source_id INTEGER NOT NULL REFERENCES source_documents(source_id) ON DELETE CASCADE,
    instance_id TEXT NOT NULL,
    definition_id TEXT NOT NULL,
    parent_assembly_id TEXT NOT NULL,
    source_shape_id TEXT NOT NULL,
    transform TEXT NOT NULL,
    metadata_id INTEGER NOT NULL,
    material_override TEXT NOT NULL,
    active INTEGER NOT NULL,
    PRIMARY KEY(source_id, instance_id),
    FOREIGN KEY(source_id, definition_id)
        REFERENCES component_definitions(source_id, definition_id)
);
CREATE TABLE IF NOT EXISTS materials (
    source_id INTEGER NOT NULL REFERENCES source_documents(source_id) ON DELETE CASCADE,
    material_id TEXT NOT NULL,
    metadata_id INTEGER NOT NULL,
    phase TEXT NOT NULL,
    density REAL NOT NULL,
    density_unit INTEGER NOT NULL,
    composition_reference INTEGER NOT NULL,
    PRIMARY KEY(source_id, material_id)
);
CREATE TABLE IF NOT EXISTS artifacts (
    digest_algorithm TEXT NOT NULL,
    digest TEXT NOT NULL,
    kind TEXT NOT NULL,
    relative_path TEXT NOT NULL UNIQUE,
    size INTEGER NOT NULL,
    PRIMARY KEY(digest_algorithm, digest, kind)
);
CREATE TABLE IF NOT EXISTS conversion_cache (
    cache_key TEXT PRIMARY KEY,
    geometry_digest TEXT NOT NULL,
    policy_digest TEXT NOT NULL,
    artifact_algorithm TEXT NOT NULL,
    artifact_digest TEXT NOT NULL,
    artifact_kind TEXT NOT NULL,
    certificate_digest TEXT NOT NULL,
    FOREIGN KEY(artifact_algorithm, artifact_digest, artifact_kind)
        REFERENCES artifacts(digest_algorithm, digest, kind)
);
CREATE TABLE IF NOT EXISTS autonomy_requests (
    request_id TEXT PRIMARY KEY,
    request_digest TEXT NOT NULL,
    source_digest TEXT NOT NULL,
    policy_digest TEXT NOT NULL,
    conversion_policy_digest TEXT NOT NULL,
    state TEXT NOT NULL,
    queue_artifact_algorithm TEXT,
    queue_artifact_digest TEXT,
    queue_artifact_kind TEXT,
    lease_owner TEXT NOT NULL,
    lease_expires_at INTEGER NOT NULL,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    FOREIGN KEY(queue_artifact_algorithm, queue_artifact_digest,
                queue_artifact_kind)
        REFERENCES artifacts(digest_algorithm, digest, kind),
    CHECK(state IN ('running','awaiting_decision','completed','failed'))
);
CREATE INDEX IF NOT EXISTS idx_autonomy_requests_state_updated
    ON autonomy_requests(state, updated_at, request_id);
CREATE TABLE IF NOT EXISTS conversion_jobs (
    job_id TEXT PRIMARY KEY,
    target_id TEXT NOT NULL,
    operation TEXT NOT NULL,
    input_digest TEXT NOT NULL,
    policy_digest TEXT NOT NULL,
    stable_order INTEGER NOT NULL,
    state TEXT NOT NULL,
    attempts INTEGER NOT NULL,
    max_attempts INTEGER NOT NULL,
    memory_bytes INTEGER NOT NULL,
    wall_time_ms INTEGER NOT NULL,
    cpu_time_seconds INTEGER NOT NULL,
    file_size_bytes INTEGER NOT NULL,
    worker_id TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    started_at INTEGER NOT NULL,
    heartbeat_at INTEGER NOT NULL,
    finished_at INTEGER NOT NULL,
    cancel_requested INTEGER NOT NULL,
    error_code TEXT NOT NULL,
    error_message TEXT NOT NULL,
    output_algorithm TEXT NOT NULL,
    output_digest TEXT NOT NULL,
    output_kind TEXT NOT NULL,
    certificate_digest TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS job_dependencies (
    job_id TEXT NOT NULL REFERENCES conversion_jobs(job_id) ON DELETE CASCADE,
    depends_on_job_id TEXT NOT NULL REFERENCES conversion_jobs(job_id),
    PRIMARY KEY(job_id, depends_on_job_id),
    CHECK(job_id <> depends_on_job_id)
);
CREATE INDEX IF NOT EXISTS idx_jobs_claim
    ON conversion_jobs(state, stable_order, target_id, job_id);
CREATE INDEX IF NOT EXISTS idx_job_dependencies_parent
    ON job_dependencies(depends_on_job_id);
CREATE TABLE IF NOT EXISTS job_events (
    job_id TEXT NOT NULL REFERENCES conversion_jobs(job_id) ON DELETE CASCADE,
    sequence INTEGER NOT NULL,
    emitted_at INTEGER NOT NULL,
    phase TEXT NOT NULL,
    completed INTEGER NOT NULL,
    total INTEGER NOT NULL,
    total_known INTEGER NOT NULL,
    target_id TEXT NOT NULL,
    message_code TEXT NOT NULL,
    payload_json TEXT NOT NULL,
    artifact_digest TEXT NOT NULL,
    PRIMARY KEY(job_id, sequence)
);
CREATE INDEX IF NOT EXISTS idx_job_events_emitted
    ON job_events(job_id, emitted_at, sequence);
CREATE TABLE IF NOT EXISTS spatial_entities (
    spatial_id INTEGER PRIMARY KEY,
    model_namespace TEXT NOT NULL,
    entity_id TEXT NOT NULL,
    owner_id TEXT NOT NULL,
    kind TEXT NOT NULL,
    UNIQUE(model_namespace, entity_id, kind)
);
CREATE VIRTUAL TABLE IF NOT EXISTS spatial_rtree USING rtree(
    spatial_id,
    xmin, xmax,
    ymin, ymax,
    zmin, zmax
);
CREATE INDEX IF NOT EXISTS idx_spatial_entity_namespace_kind
    ON spatial_entities(model_namespace, kind, entity_id);
CREATE TABLE IF NOT EXISTS agent_conversations (
    conversation_id TEXT PRIMARY KEY,
    title TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_agent_conversations_updated
    ON agent_conversations(updated_at DESC, conversation_id);
CREATE TABLE IF NOT EXISTS agent_messages (
    message_id TEXT PRIMARY KEY,
    conversation_id TEXT NOT NULL
        REFERENCES agent_conversations(conversation_id) ON DELETE CASCADE,
    ordinal INTEGER NOT NULL,
    parent_message_id TEXT NOT NULL,
    role TEXT NOT NULL,
    state TEXT NOT NULL,
    text TEXT NOT NULL,
    context_id TEXT NOT NULL,
    context_digest TEXT NOT NULL,
    document_id TEXT NOT NULL,
    document_revision INTEGER NOT NULL,
    target_id TEXT NOT NULL,
    run_id TEXT NOT NULL,
    provider_id TEXT NOT NULL,
    model_id TEXT NOT NULL,
    simulated INTEGER NOT NULL,
    created_at INTEGER NOT NULL,
    UNIQUE(conversation_id, ordinal),
    CHECK(role IN ('user','assistant')),
    CHECK(state IN ('complete','interrupted','failed'))
);
CREATE INDEX IF NOT EXISTS idx_agent_messages_conversation
    ON agent_messages(conversation_id, ordinal);
CREATE TABLE IF NOT EXISTS agent_message_references (
    message_id TEXT NOT NULL
        REFERENCES agent_messages(message_id) ON DELETE CASCADE,
    ordinal INTEGER NOT NULL,
    kind TEXT NOT NULL,
    reference_id TEXT NOT NULL,
    label TEXT NOT NULL,
    PRIMARY KEY(message_id, ordinal)
);
CREATE INDEX IF NOT EXISTS idx_definitions_geometry_digest
    ON component_definitions(geometry_digest);
CREATE INDEX IF NOT EXISTS idx_instances_definition
    ON component_instances(source_id, definition_id);
CREATE INDEX IF NOT EXISTS idx_annotations_key
    ON annotations(source_id, key_string_id);
)sql";

} // namespace

const char* project_job_state_name(ProjectJobState state) {
    switch (state) {
    case ProjectJobState::queued: return "queued";
    case ProjectJobState::running: return "running";
    case ProjectJobState::succeeded: return "succeeded";
    case ProjectJobState::failed: return "failed";
    case ProjectJobState::cancelled: return "cancelled";
    }
    return "queued";
}

const char* project_autonomy_request_state_name(
    ProjectAutonomyRequestState state) {
    switch (state) {
    case ProjectAutonomyRequestState::running: return "running";
    case ProjectAutonomyRequestState::awaiting_decision:
        return "awaiting_decision";
    case ProjectAutonomyRequestState::completed: return "completed";
    case ProjectAutonomyRequestState::failed: return "failed";
    }
    return "failed";
}

const char* project_agent_message_role_name(ProjectAgentMessageRole role) {
    switch (role) {
    case ProjectAgentMessageRole::user: return "user";
    case ProjectAgentMessageRole::assistant: return "assistant";
    }
    return "user";
}

const char* project_agent_message_state_name(ProjectAgentMessageState state) {
    switch (state) {
    case ProjectAgentMessageState::complete: return "complete";
    case ProjectAgentMessageState::interrupted: return "interrupted";
    case ProjectAgentMessageState::failed: return "failed";
    }
    return "failed";
}

ProjectStore::ProjectStore(std::filesystem::path root)
    : root_(std::move(root)), artifacts_(root_), impl_(std::make_unique<Impl>()) {}

ProjectStore::~ProjectStore() = default;
ProjectStore::ProjectStore(ProjectStore&&) noexcept = default;
ProjectStore& ProjectStore::operator=(ProjectStore&&) noexcept = default;

ProjectSnapshot project_snapshot_from_report(const ConversionReport& report,
                                             std::string source_digest) {
    ProjectSnapshot snapshot;
    snapshot.source_path = report.source_path;
    snapshot.source_digest = source_digest.empty()
        ? fnv1a128_file_hex(report.source_path) : std::move(source_digest);
    snapshot.strings = report.strings;
    snapshot.metadata = report.metadata;
    snapshot.component_definitions = report.component_definitions;
    snapshot.component_instances = report.component_instances;
    snapshot.materials = report.materials;
    return snapshot;
}

std::vector<SpatialEntry> spatial_entries_from_report(
    const ConversionReport& report) {
    std::vector<SpatialEntry> entries;
    entries.reserve(report.solids.size() + report.decomposed_pieces.size());
    for (const AnalyzedSolid& solid : report.solids) {
        if (!solid.bbox.valid) continue;
        SpatialEntry entry;
        entry.entity_id = solid.id;
        entry.owner_id = solid.component_instance_id.empty()
            ? solid.id : solid.component_instance_id;
        entry.kind = "instance_solid";
        entry.bounds = {solid.bbox.xmin, solid.bbox.ymin, solid.bbox.zmin,
                        solid.bbox.xmax, solid.bbox.ymax, solid.bbox.zmax, true};
        const std::string owner_id = entry.owner_id;
        entries.push_back(std::move(entry));
        for (const PipePortTopology& port : solid.topology.pipe_annulus.ports) {
            SpatialEntry port_entry;
            port_entry.entity_id = solid.id + "/" + port.id;
            port_entry.owner_id = owner_id;
            port_entry.kind = "pipe_port";
            const double radius = std::max(port.radius, 1.0e-9);
            port_entry.bounds = {
                port.center.x - radius, port.center.y - radius,
                port.center.z - radius, port.center.x + radius,
                port.center.y + radius, port.center.z + radius, true};
            entries.push_back(std::move(port_entry));
        }
    }
    for (const AnalyzedDecomposedPiece& piece : report.decomposed_pieces) {
        if (!piece.bbox.valid) continue;
        SpatialEntry entry;
        entry.entity_id = piece.id;
        entry.owner_id = piece.source_solid_id;
        entry.kind = "material_region_piece";
        entry.bounds = {piece.bbox.xmin, piece.bbox.ymin, piece.bbox.zmin,
                        piece.bbox.xmax, piece.bbox.ymax, piece.bbox.zmax, true};
        entries.push_back(std::move(entry));
    }
    std::sort(entries.begin(), entries.end(), [](const SpatialEntry& a,
                                                  const SpatialEntry& b) {
        if (a.kind != b.kind) return a.kind < b.kind;
        return a.entity_id < b.entity_id;
    });
    return entries;
}

std::filesystem::path ProjectStore::database_path() const {
    return root_ / "project.sqlite";
}

bool ProjectStore::open(std::string* error) {
    if (is_open()) return true;
    if (!artifacts_.initialize(error)) return false;
    sqlite3* database = nullptr;
    const int status = sqlite3_open_v2(
        database_path().string().c_str(), &database,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr);
    if (status != SQLITE_OK) {
        set_error(error, database ? sqlite3_errmsg(database)
                                  : "cannot allocate SQLite database");
        if (database) sqlite3_close(database);
        return false;
    }
    impl_->database = database;
    sqlite3_busy_timeout(database, 5000);
    if (!execute_sql(database, "PRAGMA foreign_keys=ON;", error)
        || !execute_sql(database, "PRAGMA journal_mode=WAL;", error)) {
        sqlite3_close(database);
        impl_->database = nullptr;
        return false;
    }

    const int version = schema_version(error);
    if (version < 0) {
        sqlite3_close(database);
        impl_->database = nullptr;
        return false;
    }
    if (version < 0 || version > kProjectSchemaVersion) {
        set_error(error, "unsupported csgify project schema version: "
                         + std::to_string(version));
        sqlite3_close(database);
        impl_->database = nullptr;
        return false;
    }
    if (version < kProjectSchemaVersion) {
        if (!execute_sql(database, "BEGIN IMMEDIATE;", error)) {
            sqlite3_close(database);
            impl_->database = nullptr;
            return false;
        }
        bool migrated = execute_sql(database, kProjectSchema, error);
        if (migrated && version == 6) {
            migrated = execute_sql(
                database,
                "ALTER TABLE autonomy_requests ADD COLUMN lease_owner TEXT NOT NULL DEFAULT '';"
                "ALTER TABLE autonomy_requests ADD COLUMN lease_expires_at INTEGER NOT NULL DEFAULT 0;",
                error);
        }
        if (!migrated
            || !execute_sql(database, "PRAGMA user_version=8;", error)
            || !execute_sql(database, "COMMIT;", error)) {
            execute_sql(database, "ROLLBACK;", nullptr);
            sqlite3_close(database);
            impl_->database = nullptr;
            return false;
        }
    }
    return true;
}

bool ProjectStore::open_read_only(std::string* error) {
    if (is_open()) return true;
    std::error_code filesystem_error;
    const bool database_exists = std::filesystem::exists(
        database_path(), filesystem_error);
    if (filesystem_error) {
        set_error(error, "cannot inspect project database: "
                         + filesystem_error.message());
        return false;
    }
    const bool database_is_file = database_exists
        && std::filesystem::is_regular_file(database_path(), filesystem_error);
    if (filesystem_error) {
        set_error(error, "cannot inspect project database: "
                         + filesystem_error.message());
        return false;
    }
    if (!database_is_file) {
        set_error(error, "project is not initialized; run `csgify project init "
                         + root_.string() + "`");
        return false;
    }

    sqlite3* database = nullptr;
    const int status = sqlite3_open_v2(
        database_path().string().c_str(), &database,
        SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (status != SQLITE_OK) {
        set_error(error, database ? sqlite3_errmsg(database)
                                  : "cannot allocate SQLite database");
        if (database) sqlite3_close(database);
        return false;
    }
    impl_->database = database;
    sqlite3_busy_timeout(database, 5000);

    const int version = schema_version(error);
    if (version < 0 || version > kProjectSchemaVersion) {
        if (version > kProjectSchemaVersion) {
            set_error(error, "unsupported csgify project schema version: "
                             + std::to_string(version));
        }
        sqlite3_close(database);
        impl_->database = nullptr;
        return false;
    }
    if (version < kProjectSchemaVersion) {
        set_error(error, "project schema requires migration; run `csgify project init "
                         + root_.string() + "`");
        sqlite3_close(database);
        impl_->database = nullptr;
        return false;
    }
    return true;
}

bool ProjectStore::is_open() const {
    return impl_ && impl_->database;
}

int ProjectStore::schema_version(std::string* error) const {
    if (!is_open()) {
        set_error(error, "project store is not open");
        return -1;
    }
    Statement statement(impl_->database, "PRAGMA user_version;");
    if (!statement.valid() || sqlite3_step(statement.get()) != SQLITE_ROW) {
        set_error(error, statement.error());
        return -1;
    }
    return sqlite3_column_int(statement.get(), 0);
}

bool ProjectStore::persist_snapshot(const ProjectSnapshot& snapshot,
                                    std::string* error) {
    if (!is_open() && !open(error)) return false;
    if (snapshot.source_digest.empty()) {
        set_error(error, "project snapshot source digest is empty");
        return false;
    }
    sqlite3* database = impl_->database;
    if (!execute_sql(database, "BEGIN IMMEDIATE;", error)) return false;
    bool success = false;
    do {
        Statement source(database,
            "INSERT INTO source_documents(source_path,source_digest) VALUES(?,?) "
            "ON CONFLICT(source_digest) DO UPDATE SET source_path=excluded.source_path;");
        if (!source.valid()
            || !bind_text(source.get(), 1, snapshot.source_path)
            || !bind_text(source.get(), 2, snapshot.source_digest)
            || !step_done(source, error)) break;

        Statement source_id_query(database,
            "SELECT source_id FROM source_documents WHERE source_digest=?;");
        if (!source_id_query.valid()
            || !bind_text(source_id_query.get(), 1, snapshot.source_digest)
            || sqlite3_step(source_id_query.get()) != SQLITE_ROW) {
            set_error(error, source_id_query.error());
            break;
        }
        const sqlite3_int64 source_id = sqlite3_column_int64(source_id_query.get(), 0);

        const char* clear_tables[] = {
            "DELETE FROM component_instances WHERE source_id=?;",
            "DELETE FROM component_definitions WHERE source_id=?;",
            "DELETE FROM materials WHERE source_id=?;",
            "DELETE FROM annotations WHERE source_id=?;",
            "DELETE FROM metadata_tags WHERE source_id=?;",
            "DELETE FROM metadata_entities WHERE source_id=?;",
            "DELETE FROM interned_strings WHERE source_id=?;",
        };
        bool clear_ok = true;
        for (const char* sql : clear_tables) {
            Statement clear(database, sql);
            if (!clear.valid()
                || sqlite3_bind_int64(clear.get(), 1, source_id) != SQLITE_OK
                || !step_done(clear, error)) {
                clear_ok = false;
                break;
            }
        }
        if (!clear_ok) break;

        bool insert_ok = true;
        for (std::size_t id = 0; id < snapshot.strings.values().size(); ++id) {
            Statement insert(database,
                "INSERT INTO interned_strings(source_id,string_id,value) VALUES(?,?,?);");
            if (!insert.valid()
                || sqlite3_bind_int64(insert.get(), 1, source_id) != SQLITE_OK
                || sqlite3_bind_int64(insert.get(), 2, static_cast<sqlite3_int64>(id)) != SQLITE_OK
                || !bind_text(insert.get(), 3, snapshot.strings.values()[id])
                || !step_done(insert, error)) {
                insert_ok = false;
                break;
            }
        }
        if (!insert_ok) break;

        for (const EntityMetadata& metadata : snapshot.metadata) {
            Statement insert(database,
                "INSERT INTO metadata_entities VALUES(?,?,?,?,?,?,?,?,?);");
            if (!insert.valid()
                || sqlite3_bind_int64(insert.get(), 1, source_id) != SQLITE_OK
                || sqlite3_bind_int64(insert.get(), 2, metadata.id) != SQLITE_OK
                || sqlite3_bind_int64(insert.get(), 3, metadata.stable_name) != SQLITE_OK
                || sqlite3_bind_int64(insert.get(), 4, metadata.display_name) != SQLITE_OK
                || sqlite3_bind_int64(insert.get(), 5, metadata.description) != SQLITE_OK
                || sqlite3_bind_int64(insert.get(), 6, metadata.engineering_role) != SQLITE_OK
                || sqlite3_bind_int64(insert.get(), 7, metadata.subsystem) != SQLITE_OK
                || sqlite3_bind_int64(insert.get(), 8, metadata.source_cad_label) != SQLITE_OK
                || sqlite3_bind_int64(insert.get(), 9, metadata.source_component_path) != SQLITE_OK
                || !step_done(insert, error)) {
                insert_ok = false;
                break;
            }
            for (std::size_t ordinal = 0; ordinal < metadata.tags.size(); ++ordinal) {
                Statement tag(database,
                    "INSERT INTO metadata_tags VALUES(?,?,?,?);");
                if (!tag.valid()
                    || sqlite3_bind_int64(tag.get(), 1, source_id) != SQLITE_OK
                    || sqlite3_bind_int64(tag.get(), 2, metadata.id) != SQLITE_OK
                    || sqlite3_bind_int64(tag.get(), 3, ordinal) != SQLITE_OK
                    || sqlite3_bind_int64(tag.get(), 4, metadata.tags[ordinal]) != SQLITE_OK
                    || !step_done(tag, error)) {
                    insert_ok = false;
                    break;
                }
            }
            if (!insert_ok) break;
            for (std::size_t ordinal = 0;
                 ordinal < metadata.annotations.size(); ++ordinal) {
                const Annotation& annotation = metadata.annotations[ordinal];
                Statement annotation_insert(database,
                    "INSERT INTO annotations VALUES(?,?,?,?,?,?,?,?);");
                if (!annotation_insert.valid()
                    || sqlite3_bind_int64(annotation_insert.get(), 1, source_id) != SQLITE_OK
                    || sqlite3_bind_int64(annotation_insert.get(), 2, metadata.id) != SQLITE_OK
                    || sqlite3_bind_int64(annotation_insert.get(), 3, ordinal) != SQLITE_OK
                    || sqlite3_bind_int64(annotation_insert.get(), 4, annotation.key) != SQLITE_OK
                    || sqlite3_bind_int64(annotation_insert.get(), 5, annotation.value) != SQLITE_OK
                    || !bind_text(annotation_insert.get(), 6,
                                  annotation_origin_name(annotation.origin))
                    || !bind_text(annotation_insert.get(), 7,
                                  metadata_review_state_name(annotation.review_state))
                    || sqlite3_bind_int(annotation_insert.get(), 8,
                                        annotation.user_verified ? 1 : 0) != SQLITE_OK
                    || !step_done(annotation_insert, error)) {
                    insert_ok = false;
                    break;
                }
            }
            if (!insert_ok) break;
        }
        if (!insert_ok) break;

        for (const ComponentDefinition& definition : snapshot.component_definitions) {
            Statement insert(database,
                "INSERT INTO component_definitions VALUES(?,?,?,?,?,?);");
            if (!insert.valid()
                || sqlite3_bind_int64(insert.get(), 1, source_id) != SQLITE_OK
                || !bind_text(insert.get(), 2, definition.id)
                || sqlite3_bind_int64(insert.get(), 3,
                                      definition.source_definition_key) != SQLITE_OK
                || !bind_text(insert.get(), 4, definition.geometry_digest)
                || sqlite3_bind_int(insert.get(), 5,
                                    definition.geometry_digest_verified ? 1 : 0) != SQLITE_OK
                || sqlite3_bind_int64(insert.get(), 6,
                                      definition.metadata_id) != SQLITE_OK
                || !step_done(insert, error)) {
                insert_ok = false;
                break;
            }
        }
        if (!insert_ok) break;

        for (const ComponentInstance& instance : snapshot.component_instances) {
            Statement insert(database,
                "INSERT INTO component_instances VALUES(?,?,?,?,?,?,?,?,?);");
            if (!insert.valid()
                || sqlite3_bind_int64(insert.get(), 1, source_id) != SQLITE_OK
                || !bind_text(insert.get(), 2, instance.id)
                || !bind_text(insert.get(), 3, instance.definition_id)
                || !bind_text(insert.get(), 4, instance.parent_assembly_id)
                || !bind_text(insert.get(), 5, instance.source_shape_id)
                || !bind_text(insert.get(), 6, transform_text(instance.transform))
                || sqlite3_bind_int64(insert.get(), 7, instance.metadata_id) != SQLITE_OK
                || !bind_text(insert.get(), 8, instance.material_override)
                || sqlite3_bind_int(insert.get(), 9, instance.active ? 1 : 0) != SQLITE_OK
                || !step_done(insert, error)) {
                insert_ok = false;
                break;
            }
        }
        if (!insert_ok) break;

        for (const MaterialDefinition& material : snapshot.materials) {
            Statement insert(database,
                "INSERT INTO materials VALUES(?,?,?,?,?,?,?);");
            if (!insert.valid()
                || sqlite3_bind_int64(insert.get(), 1, source_id) != SQLITE_OK
                || !bind_text(insert.get(), 2, material.id)
                || sqlite3_bind_int64(insert.get(), 3, material.metadata_id) != SQLITE_OK
                || !bind_text(insert.get(), 4, material_phase_name(material.phase))
                || sqlite3_bind_double(insert.get(), 5, material.density) != SQLITE_OK
                || sqlite3_bind_int64(insert.get(), 6, material.density_unit) != SQLITE_OK
                || sqlite3_bind_int64(insert.get(), 7,
                                      material.composition_reference) != SQLITE_OK
                || !step_done(insert, error)) {
                insert_ok = false;
                break;
            }
        }
        if (!insert_ok) break;
        success = true;
    } while (false);

    if (success) {
        if (!execute_sql(database, "COMMIT;", error)) {
            execute_sql(database, "ROLLBACK;", nullptr);
            return false;
        }
        return true;
    }
    execute_sql(database, "ROLLBACK;", nullptr);
    return false;
}

bool ProjectStore::record_artifact(const ArtifactRecord& artifact,
                                   std::string* error) {
    if (!is_open() && !open(error)) return false;
    if (!artifacts_.validate(artifact, error)) return false;
    Statement insert(impl_->database,
        "INSERT INTO artifacts VALUES(?,?,?,?,?) "
        "ON CONFLICT(digest_algorithm,digest,kind) DO UPDATE SET "
        "relative_path=excluded.relative_path,size=excluded.size;");
    if (!insert.valid()
        || !bind_text(insert.get(), 1, artifact.digest_algorithm)
        || !bind_text(insert.get(), 2, artifact.digest)
        || !bind_text(insert.get(), 3, artifact.kind)
        || !bind_text(insert.get(), 4, artifact.relative_path)
        || sqlite3_bind_int64(insert.get(), 5,
                              static_cast<sqlite3_int64>(artifact.size)) != SQLITE_OK) {
        set_error(error, insert.error());
        return false;
    }
    return step_done(insert, error);
}

std::optional<ArtifactRecord> ProjectStore::find_artifact(
    const std::string& digest,
    const std::string& kind,
    std::string* error) const {
    if (!is_open()) {
        set_error(error, "project store is not open");
        return std::nullopt;
    }
    Statement query(impl_->database,
        "SELECT digest_algorithm,digest,kind,relative_path,size FROM artifacts "
        "WHERE digest=? AND kind=? ORDER BY digest_algorithm LIMIT 1;");
    if (!query.valid() || !bind_text(query.get(), 1, digest)
        || !bind_text(query.get(), 2, kind)) {
        set_error(error, query.error());
        return std::nullopt;
    }
    const int status = sqlite3_step(query.get());
    if (status == SQLITE_DONE) return std::nullopt;
    if (status != SQLITE_ROW) {
        set_error(error, query.error());
        return std::nullopt;
    }
    ArtifactRecord result;
    result.digest_algorithm = reinterpret_cast<const char*>(sqlite3_column_text(query.get(), 0));
    result.digest = reinterpret_cast<const char*>(sqlite3_column_text(query.get(), 1));
    result.kind = reinterpret_cast<const char*>(sqlite3_column_text(query.get(), 2));
    result.relative_path = reinterpret_cast<const char*>(sqlite3_column_text(query.get(), 3));
    result.size = static_cast<std::uint64_t>(sqlite3_column_int64(query.get(), 4));
    result.deduplicated = true;
    return result;
}

bool ProjectStore::record_conversion_cache(const ConversionCacheEntry& entry,
                                           std::string* error) {
    if (!is_open() && !open(error)) return false;
    if (entry.cache_key.empty() || entry.geometry_digest.empty()
        || entry.policy_digest.empty() || entry.certificate_digest.empty()) {
        set_error(error, "conversion cache entry has an empty required digest");
        return false;
    }
    const auto artifact = find_artifact(
        entry.artifact.digest, entry.artifact.kind, error);
    if (!artifact || artifact->digest_algorithm != entry.artifact.digest_algorithm
        || artifact->relative_path != entry.artifact.relative_path
        || !artifacts_.validate(*artifact, error)) {
        if (error && error->empty()) {
            *error = "conversion cache artifact is not indexed or valid";
        }
        return false;
    }
    Statement insert(impl_->database,
        "INSERT INTO conversion_cache VALUES(?,?,?,?,?,?,?) "
        "ON CONFLICT(cache_key) DO UPDATE SET "
        "geometry_digest=excluded.geometry_digest,"
        "policy_digest=excluded.policy_digest,"
        "artifact_algorithm=excluded.artifact_algorithm,"
        "artifact_digest=excluded.artifact_digest,"
        "artifact_kind=excluded.artifact_kind,"
        "certificate_digest=excluded.certificate_digest;");
    if (!insert.valid()
        || !bind_text(insert.get(), 1, entry.cache_key)
        || !bind_text(insert.get(), 2, entry.geometry_digest)
        || !bind_text(insert.get(), 3, entry.policy_digest)
        || !bind_text(insert.get(), 4, entry.artifact.digest_algorithm)
        || !bind_text(insert.get(), 5, entry.artifact.digest)
        || !bind_text(insert.get(), 6, entry.artifact.kind)
        || !bind_text(insert.get(), 7, entry.certificate_digest)) {
        set_error(error, insert.error());
        return false;
    }
    return step_done(insert, error);
}

std::optional<ConversionCacheEntry> ProjectStore::find_conversion_cache(
    const std::string& cache_key,
    std::string* error) const {
    if (!is_open()) {
        set_error(error, "project store is not open");
        return std::nullopt;
    }
    Statement query(impl_->database,
        "SELECT c.geometry_digest,c.policy_digest,c.artifact_algorithm,"
        "c.artifact_digest,c.artifact_kind,a.relative_path,a.size,"
        "c.certificate_digest FROM conversion_cache c JOIN artifacts a ON "
        "a.digest_algorithm=c.artifact_algorithm AND "
        "a.digest=c.artifact_digest AND a.kind=c.artifact_kind "
        "WHERE c.cache_key=?;");
    if (!query.valid() || !bind_text(query.get(), 1, cache_key)) {
        set_error(error, query.error());
        return std::nullopt;
    }
    const int status = sqlite3_step(query.get());
    if (status == SQLITE_DONE) return std::nullopt;
    if (status != SQLITE_ROW) {
        set_error(error, query.error());
        return std::nullopt;
    }
    auto text_column = [&](int column) -> std::string {
        const unsigned char* value = sqlite3_column_text(query.get(), column);
        return value ? reinterpret_cast<const char*>(value) : std::string{};
    };
    ConversionCacheEntry result;
    result.cache_key = cache_key;
    result.geometry_digest = text_column(0);
    result.policy_digest = text_column(1);
    result.artifact.digest_algorithm = text_column(2);
    result.artifact.digest = text_column(3);
    result.artifact.kind = text_column(4);
    result.artifact.relative_path = text_column(5);
    result.artifact.size = static_cast<std::uint64_t>(
        sqlite3_column_int64(query.get(), 6));
    result.artifact.deduplicated = true;
    result.certificate_digest = text_column(7);
    if (!artifacts_.validate(result.artifact, error)) return std::nullopt;
    return result;
}

bool ProjectStore::upsert_agent_conversation(
    const ProjectAgentConversationRecord& conversation,
    std::string* error) {
    if (!is_open() && !open(error)) return false;
    if (conversation.conversation_id.empty()
        || conversation.conversation_id.size() > 256
        || conversation.title.size() > 1024
        || conversation.created_at < 0 || conversation.updated_at < 0
        || conversation.updated_at < conversation.created_at) {
        set_error(error, "invalid agent conversation record");
        return false;
    }
    Statement statement(impl_->database,
        "INSERT INTO agent_conversations("
        "conversation_id,title,created_at,updated_at) VALUES(?,?,?,?) "
        "ON CONFLICT(conversation_id) DO UPDATE SET "
        "title=excluded.title,updated_at=MAX(updated_at,excluded.updated_at);");
    if (!statement.valid()
        || !bind_text(statement.get(), 1, conversation.conversation_id)
        || !bind_text(statement.get(), 2, conversation.title)
        || sqlite3_bind_int64(statement.get(), 3, conversation.created_at)
               != SQLITE_OK
        || sqlite3_bind_int64(statement.get(), 4, conversation.updated_at)
               != SQLITE_OK) {
        set_error(error, statement.error());
        return false;
    }
    return step_done(statement, error);
}

bool ProjectStore::append_agent_message(
    const ProjectAgentMessageRecord& message,
    std::string* error) {
    if (!is_open() && !open(error)) return false;
    const std::size_t text_limit = message.role == ProjectAgentMessageRole::user
        ? 64 * 1024 : 256 * 1024;
    if (message.message_id.empty() || message.message_id.size() > 256
        || message.conversation_id.empty()
        || message.conversation_id.size() > 256
        || message.parent_message_id.size() > 256
        || message.text.empty() || message.text.size() > text_limit
        || message.context_id.size() > 256
        || message.context_digest.size() > 128
        || message.document_id.size() > 256
        || message.document_revision
            > static_cast<std::size_t>(std::numeric_limits<sqlite3_int64>::max())
        || message.target_id.size() > 512 || message.run_id.size() > 256
        || message.provider_id.size() > 256 || message.model_id.size() > 256
        || message.created_at < 0 || message.references.size() > 128) {
        set_error(error, "invalid agent message record");
        return false;
    }
    for (const ProjectAgentEvidenceReference& reference : message.references) {
        if (reference.kind.empty() || reference.kind.size() > 32
            || reference.id.empty() || reference.id.size() > 512
            || reference.label.size() > 512) {
            set_error(error, "invalid agent message evidence reference");
            return false;
        }
    }
    if (!execute_sql(impl_->database, "BEGIN IMMEDIATE;", error)) return false;
    const auto rollback = [&]() {
        execute_sql(impl_->database, "ROLLBACK;", nullptr);
        return false;
    };

    Statement next(impl_->database,
        "SELECT COALESCE(MAX(ordinal),0)+1 FROM agent_messages "
        "WHERE conversation_id=?;");
    if (!next.valid() || !bind_text(next.get(), 1, message.conversation_id)
        || sqlite3_step(next.get()) != SQLITE_ROW) {
        set_error(error, next.error());
        return rollback();
    }
    const sqlite3_int64 ordinal = sqlite3_column_int64(next.get(), 0);

    Statement insert(impl_->database,
        "INSERT INTO agent_messages("
        "message_id,conversation_id,ordinal,parent_message_id,role,state,text,"
        "context_id,context_digest,document_id,document_revision,target_id,"
        "run_id,provider_id,model_id,simulated,created_at) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);");
    if (!insert.valid()
        || !bind_text(insert.get(), 1, message.message_id)
        || !bind_text(insert.get(), 2, message.conversation_id)
        || sqlite3_bind_int64(insert.get(), 3, ordinal) != SQLITE_OK
        || !bind_text(insert.get(), 4, message.parent_message_id)
        || !bind_text(insert.get(), 5,
                      project_agent_message_role_name(message.role))
        || !bind_text(insert.get(), 6,
                      project_agent_message_state_name(message.state))
        || !bind_text(insert.get(), 7, message.text)
        || !bind_text(insert.get(), 8, message.context_id)
        || !bind_text(insert.get(), 9, message.context_digest)
        || !bind_text(insert.get(), 10, message.document_id)
        || sqlite3_bind_int64(
               insert.get(), 11,
               static_cast<sqlite3_int64>(message.document_revision))
               != SQLITE_OK
        || !bind_text(insert.get(), 12, message.target_id)
        || !bind_text(insert.get(), 13, message.run_id)
        || !bind_text(insert.get(), 14, message.provider_id)
        || !bind_text(insert.get(), 15, message.model_id)
        || sqlite3_bind_int(insert.get(), 16, message.simulated ? 1 : 0)
               != SQLITE_OK
        || sqlite3_bind_int64(insert.get(), 17, message.created_at)
               != SQLITE_OK
        || !step_done(insert, error))
        return rollback();

    for (std::size_t i = 0; i < message.references.size(); ++i) {
        const ProjectAgentEvidenceReference& reference = message.references[i];
        Statement reference_insert(impl_->database,
            "INSERT INTO agent_message_references("
            "message_id,ordinal,kind,reference_id,label) VALUES(?,?,?,?,?);");
        if (!reference_insert.valid()
            || !bind_text(reference_insert.get(), 1, message.message_id)
            || sqlite3_bind_int64(reference_insert.get(), 2,
                                  static_cast<sqlite3_int64>(i)) != SQLITE_OK
            || !bind_text(reference_insert.get(), 3, reference.kind)
            || !bind_text(reference_insert.get(), 4, reference.id)
            || !bind_text(reference_insert.get(), 5, reference.label)
            || !step_done(reference_insert, error))
            return rollback();
    }

    Statement update(impl_->database,
        "UPDATE agent_conversations SET updated_at=MAX(updated_at,?) "
        "WHERE conversation_id=?;");
    if (!update.valid()
        || sqlite3_bind_int64(update.get(), 1, message.created_at) != SQLITE_OK
        || !bind_text(update.get(), 2, message.conversation_id)
        || !step_done(update, error)
        || sqlite3_changes(impl_->database) != 1)
        return rollback();
    if (!execute_sql(impl_->database, "COMMIT;", error)) return rollback();
    return true;
}

std::vector<ProjectAgentConversationRecord>
ProjectStore::list_agent_conversations(
    std::size_t limit, std::size_t offset, std::string* error) const {
    std::vector<ProjectAgentConversationRecord> result;
    if (!is_open()) {
        set_error(error, "project store is not open");
        return result;
    }
    limit = std::min<std::size_t>(limit, 1000);
    Statement query(impl_->database,
        "SELECT conversation_id,title,created_at,updated_at "
        "FROM agent_conversations ORDER BY updated_at DESC,conversation_id "
        "LIMIT ? OFFSET ?;");
    if (!query.valid()
        || sqlite3_bind_int64(query.get(), 1,
                              static_cast<sqlite3_int64>(limit)) != SQLITE_OK
        || sqlite3_bind_int64(query.get(), 2,
                              static_cast<sqlite3_int64>(offset)) != SQLITE_OK) {
        set_error(error, query.error());
        return {};
    }
    int status = SQLITE_ROW;
    while ((status = sqlite3_step(query.get())) == SQLITE_ROW) {
        ProjectAgentConversationRecord row;
        row.conversation_id = column_text(query.get(), 0);
        row.title = column_text(query.get(), 1);
        row.created_at = sqlite3_column_int64(query.get(), 2);
        row.updated_at = sqlite3_column_int64(query.get(), 3);
        result.push_back(std::move(row));
    }
    if (status != SQLITE_DONE) {
        set_error(error, query.error());
        return {};
    }
    return result;
}

std::vector<ProjectAgentMessageRecord> ProjectStore::list_agent_messages(
    const std::string& conversation_id,
    std::size_t limit, std::size_t offset, std::string* error) const {
    std::vector<ProjectAgentMessageRecord> result;
    if (!is_open()) {
        set_error(error, "project store is not open");
        return result;
    }
    if (conversation_id.empty() || conversation_id.size() > 256) {
        set_error(error, "invalid agent conversation id");
        return result;
    }
    limit = std::min<std::size_t>(limit, 1000);
    Statement query(impl_->database,
        "SELECT message_id,parent_message_id,role,state,text,context_id,"
        "context_digest,document_id,document_revision,target_id,run_id,"
        "provider_id,model_id,simulated,created_at,ordinal "
        "FROM agent_messages WHERE conversation_id=? ORDER BY ordinal "
        "LIMIT ? OFFSET ?;");
    if (!query.valid() || !bind_text(query.get(), 1, conversation_id)
        || sqlite3_bind_int64(query.get(), 2,
                              static_cast<sqlite3_int64>(limit)) != SQLITE_OK
        || sqlite3_bind_int64(query.get(), 3,
                              static_cast<sqlite3_int64>(offset)) != SQLITE_OK) {
        set_error(error, query.error());
        return {};
    }
    int status = SQLITE_ROW;
    while ((status = sqlite3_step(query.get())) == SQLITE_ROW) {
        ProjectAgentMessageRecord row;
        row.message_id = column_text(query.get(), 0);
        row.conversation_id = conversation_id;
        row.parent_message_id = column_text(query.get(), 1);
        row.role = column_text(query.get(), 2) == "assistant"
            ? ProjectAgentMessageRole::assistant : ProjectAgentMessageRole::user;
        const std::string state = column_text(query.get(), 3);
        row.state = state == "interrupted"
            ? ProjectAgentMessageState::interrupted
            : state == "failed" ? ProjectAgentMessageState::failed
                                  : ProjectAgentMessageState::complete;
        row.text = column_text(query.get(), 4);
        row.context_id = column_text(query.get(), 5);
        row.context_digest = column_text(query.get(), 6);
        row.document_id = column_text(query.get(), 7);
        row.document_revision = static_cast<std::size_t>(
            sqlite3_column_int64(query.get(), 8));
        row.target_id = column_text(query.get(), 9);
        row.run_id = column_text(query.get(), 10);
        row.provider_id = column_text(query.get(), 11);
        row.model_id = column_text(query.get(), 12);
        row.simulated = sqlite3_column_int(query.get(), 13) != 0;
        row.created_at = sqlite3_column_int64(query.get(), 14);
        row.ordinal = static_cast<std::uint64_t>(
            sqlite3_column_int64(query.get(), 15));

        Statement references(impl_->database,
            "SELECT kind,reference_id,label FROM agent_message_references "
            "WHERE message_id=? ORDER BY ordinal;");
        if (!references.valid()
            || !bind_text(references.get(), 1, row.message_id)) {
            set_error(error, references.error());
            return {};
        }
        int reference_status = SQLITE_ROW;
        while ((reference_status = sqlite3_step(references.get()))
               == SQLITE_ROW) {
            row.references.push_back({column_text(references.get(), 0),
                                      column_text(references.get(), 1),
                                      column_text(references.get(), 2)});
        }
        if (reference_status != SQLITE_DONE) {
            set_error(error, references.error());
            return {};
        }
        result.push_back(std::move(row));
    }
    if (status != SQLITE_DONE) {
        set_error(error, query.error());
        return {};
    }
    return result;
}

std::optional<ProjectAutonomyRequestRecord>
ProjectStore::find_autonomy_request(const std::string& request_id,
                                    std::string* error) const {
    if (!is_open()) {
        set_error(error, "project store is not open");
        return std::nullopt;
    }
    Statement query(impl_->database,
        "SELECT r.request_id,r.request_digest,r.source_digest,r.policy_digest,"
        "r.conversion_policy_digest,r.state,r.lease_owner,r.lease_expires_at,"
        "r.created_at,r.updated_at,"
        "a.digest_algorithm,a.digest,a.kind,a.relative_path,a.size "
        "FROM autonomy_requests r LEFT JOIN artifacts a ON "
        "a.digest_algorithm=r.queue_artifact_algorithm AND "
        "a.digest=r.queue_artifact_digest AND a.kind=r.queue_artifact_kind "
        "WHERE r.request_id=?;");
    if (!query.valid() || !bind_text(query.get(), 1, request_id)) {
        set_error(error, query.error());
        return std::nullopt;
    }
    const int status = sqlite3_step(query.get());
    if (status == SQLITE_DONE) return std::nullopt;
    if (status != SQLITE_ROW) {
        set_error(error, query.error());
        return std::nullopt;
    }
    ProjectAutonomyRequestRecord result;
    result.request_id = column_text(query.get(), 0);
    result.request_digest = column_text(query.get(), 1);
    result.source_digest = column_text(query.get(), 2);
    result.policy_digest = column_text(query.get(), 3);
    result.conversion_policy_digest = column_text(query.get(), 4);
    const std::string state = column_text(query.get(), 5);
    result.state = state == "running"
        ? ProjectAutonomyRequestState::running
        : state == "awaiting_decision"
            ? ProjectAutonomyRequestState::awaiting_decision
            : state == "completed"
                ? ProjectAutonomyRequestState::completed
                : ProjectAutonomyRequestState::failed;
    result.lease_owner = column_text(query.get(), 6);
    result.lease_expires_at = sqlite3_column_int64(query.get(), 7);
    result.created_at = sqlite3_column_int64(query.get(), 8);
    result.updated_at = sqlite3_column_int64(query.get(), 9);
    if (sqlite3_column_type(query.get(), 10) != SQLITE_NULL) {
        ArtifactRecord artifact;
        artifact.digest_algorithm = column_text(query.get(), 10);
        artifact.digest = column_text(query.get(), 11);
        artifact.kind = column_text(query.get(), 12);
        artifact.relative_path = column_text(query.get(), 13);
        artifact.size = static_cast<std::uint64_t>(
            sqlite3_column_int64(query.get(), 14));
        artifact.deduplicated = true;
        result.queue_artifact = std::move(artifact);
    }
    return result;
}

ProjectAutonomyReservation ProjectStore::reserve_autonomy_request(
    const ProjectAutonomyRequestRecord& request,
    std::string* error) {
    ProjectAutonomyReservation result;
    if (!is_open() && !open(error)) return result;
    if (request.request_id.empty() || request.request_digest.empty()
        || request.source_digest.empty() || request.policy_digest.empty()
        || request.conversion_policy_digest.empty()
        || request.lease_owner.empty() || request.lease_expires_at <= 0) {
        set_error(error, "autonomy request reservation has an empty identity field");
        return result;
    }
    const std::int64_t now = request.created_at > 0
        ? request.created_at : static_cast<std::int64_t>(std::time(nullptr));
    Statement insert(impl_->database,
        "INSERT INTO autonomy_requests("
        "request_id,request_digest,source_digest,policy_digest,"
        "conversion_policy_digest,state,queue_artifact_algorithm,"
        "queue_artifact_digest,queue_artifact_kind,lease_owner,"
        "lease_expires_at,created_at,updated_at) "
        "VALUES(?,?,?,?,?,'running',NULL,NULL,NULL,?,?,?,?) "
        "ON CONFLICT(request_id) DO NOTHING;");
    if (!insert.valid()
        || !bind_text(insert.get(), 1, request.request_id)
        || !bind_text(insert.get(), 2, request.request_digest)
        || !bind_text(insert.get(), 3, request.source_digest)
        || !bind_text(insert.get(), 4, request.policy_digest)
        || !bind_text(insert.get(), 5, request.conversion_policy_digest)
        || !bind_text(insert.get(), 6, request.lease_owner)
        || sqlite3_bind_int64(insert.get(), 7,
                              request.lease_expires_at) != SQLITE_OK
        || sqlite3_bind_int64(insert.get(), 8, now) != SQLITE_OK
        || sqlite3_bind_int64(insert.get(), 9, now) != SQLITE_OK
        || !step_done(insert, error)) return result;
    const bool inserted = sqlite3_changes(impl_->database) == 1;
    result.record = find_autonomy_request(request.request_id, error);
    if (!result.record) return result;
    if (inserted) {
        result.status = ProjectAutonomyReservationStatus::reserved;
        return result;
    }
    const ProjectAutonomyRequestRecord& existing = *result.record;
    const bool same = existing.request_digest == request.request_digest
        && existing.source_digest == request.source_digest
        && existing.policy_digest == request.policy_digest
        && existing.conversion_policy_digest
               == request.conversion_policy_digest;
    if (!same) {
        result.status = ProjectAutonomyReservationStatus::conflict;
        return result;
    }
    if (existing.state == ProjectAutonomyRequestState::running
        && !existing.queue_artifact && existing.lease_expires_at <= now) {
        Statement reclaim(impl_->database,
            "UPDATE autonomy_requests SET lease_owner=?,lease_expires_at=?,"
            "updated_at=? WHERE request_id=? AND request_digest=? "
            "AND state='running' AND queue_artifact_digest IS NULL "
            "AND lease_expires_at<=?;");
        if (!reclaim.valid()
            || !bind_text(reclaim.get(), 1, request.lease_owner)
            || sqlite3_bind_int64(reclaim.get(), 2,
                                  request.lease_expires_at) != SQLITE_OK
            || sqlite3_bind_int64(reclaim.get(), 3, now) != SQLITE_OK
            || !bind_text(reclaim.get(), 4, request.request_id)
            || !bind_text(reclaim.get(), 5, request.request_digest)
            || sqlite3_bind_int64(reclaim.get(), 6, now) != SQLITE_OK
            || !step_done(reclaim, error)) return result;
        if (sqlite3_changes(impl_->database) == 1) {
            result.record = find_autonomy_request(request.request_id, error);
            result.status = result.record
                ? ProjectAutonomyReservationStatus::reserved
                : ProjectAutonomyReservationStatus::error;
            return result;
        }
    }
    result.status = ProjectAutonomyReservationStatus::existing;
    return result;
}

bool ProjectStore::claim_autonomy_request(
    const std::string& request_id,
    const std::string& request_digest,
    const std::string& lease_owner,
    std::int64_t lease_expires_at,
    std::string* error) {
    if (!is_open() && !open(error)) return false;
    if (request_id.empty() || request_digest.empty() || lease_owner.empty()
        || lease_expires_at <= 0) {
        set_error(error, "invalid autonomy request lease claim");
        return false;
    }
    Statement update(impl_->database,
        "UPDATE autonomy_requests SET state='running',lease_owner=?,"
        "lease_expires_at=?,updated_at=? WHERE request_id=? "
        "AND request_digest=? AND state IN ('awaiting_decision','failed');");
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    if (!update.valid() || !bind_text(update.get(), 1, lease_owner)
        || sqlite3_bind_int64(update.get(), 2, lease_expires_at) != SQLITE_OK
        || sqlite3_bind_int64(update.get(), 3, now) != SQLITE_OK
        || !bind_text(update.get(), 4, request_id)
        || !bind_text(update.get(), 5, request_digest)
        || !step_done(update, error)) return false;
    if (sqlite3_changes(impl_->database) == 1) return true;
    set_error(error, "autonomy request is not resumable or identity changed");
    return false;
}

bool ProjectStore::renew_autonomy_request_lease(
    const std::string& request_id,
    const std::string& request_digest,
    const std::string& lease_owner,
    std::int64_t lease_expires_at,
    std::string* error) {
    if (!is_open() && !open(error)) return false;
    if (request_id.empty() || request_digest.empty() || lease_owner.empty()
        || lease_expires_at <= 0) {
        set_error(error, "invalid autonomy request lease renewal");
        return false;
    }
    Statement update(impl_->database,
        "UPDATE autonomy_requests SET lease_expires_at=?,updated_at=? "
        "WHERE request_id=? AND request_digest=? AND lease_owner=? "
        "AND state='running';");
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    if (!update.valid()
        || sqlite3_bind_int64(update.get(), 1, lease_expires_at) != SQLITE_OK
        || sqlite3_bind_int64(update.get(), 2, now) != SQLITE_OK
        || !bind_text(update.get(), 3, request_id)
        || !bind_text(update.get(), 4, request_digest)
        || !bind_text(update.get(), 5, lease_owner)
        || !step_done(update, error)) return false;
    if (sqlite3_changes(impl_->database) == 1) return true;
    set_error(error, "autonomy request lease renewal rejected by ownership or state");
    return false;
}

bool ProjectStore::rollback_autonomy_request_claim(
    const std::string& request_id,
    const std::string& request_digest,
    const std::string& lease_owner,
    ProjectAutonomyRequestState previous_state,
    std::string* error) {
    if (!is_open() && !open(error)) return false;
    if (request_id.empty() || request_digest.empty() || lease_owner.empty()
        || previous_state == ProjectAutonomyRequestState::completed) {
        set_error(error, "invalid autonomy request claim rollback");
        return false;
    }
    if (previous_state == ProjectAutonomyRequestState::running) {
        Statement remove(impl_->database,
            "DELETE FROM autonomy_requests WHERE request_id=? "
            "AND request_digest=? AND lease_owner=? AND state='running' "
            "AND queue_artifact_digest IS NULL;");
        if (!remove.valid() || !bind_text(remove.get(), 1, request_id)
            || !bind_text(remove.get(), 2, request_digest)
            || !bind_text(remove.get(), 3, lease_owner)
            || !step_done(remove, error)) return false;
        if (sqlite3_changes(impl_->database) == 1) return true;
    } else {
        const std::string restored =
            project_autonomy_request_state_name(previous_state);
        Statement update(impl_->database,
            "UPDATE autonomy_requests SET state=?,lease_owner='',"
            "lease_expires_at=0,updated_at=? WHERE request_id=? "
            "AND request_digest=? AND lease_owner=? AND state='running' "
            "AND queue_artifact_digest IS NOT NULL;");
        const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
        if (!update.valid() || !bind_text(update.get(), 1, restored)
            || sqlite3_bind_int64(update.get(), 2, now) != SQLITE_OK
            || !bind_text(update.get(), 3, request_id)
            || !bind_text(update.get(), 4, request_digest)
            || !bind_text(update.get(), 5, lease_owner)
            || !step_done(update, error)) return false;
        if (sqlite3_changes(impl_->database) == 1) return true;
    }
    set_error(error,
              "autonomy request claim rollback rejected by ownership or state");
    return false;
}

bool ProjectStore::finish_autonomy_request(
    const std::string& request_id,
    const std::string& request_digest,
    const std::string& lease_owner,
    ProjectAutonomyRequestState state,
    const ArtifactRecord& queue_artifact,
    std::int64_t now_epoch_seconds,
    std::string* error) {
    if (!is_open() && !open(error)) return false;
    if (request_id.empty() || request_digest.empty() || lease_owner.empty()
        || state == ProjectAutonomyRequestState::running
        || queue_artifact.kind != "autonomy-queue"
        || !artifacts_.validate(queue_artifact, error)) {
        if (error && error->empty())
            *error = "invalid autonomy request completion";
        return false;
    }
    if (!execute_sql(impl_->database, "BEGIN IMMEDIATE;", error)) return false;
    const bool artifact_ok = record_artifact(queue_artifact, error);
    Statement update(impl_->database,
        "UPDATE autonomy_requests SET state=?,queue_artifact_algorithm=?,"
        "queue_artifact_digest=?,queue_artifact_kind=?,updated_at=? "
        "WHERE request_id=? AND request_digest=? AND lease_owner=? AND "
        "(state IN ('running','awaiting_decision','failed') OR "
        "(state='completed' AND ?='completed' AND "
        "queue_artifact_algorithm=? AND queue_artifact_digest=? AND "
        "queue_artifact_kind=?));");
    const std::string state_name = project_autonomy_request_state_name(state);
    const bool update_ok = artifact_ok && update.valid()
        && bind_text(update.get(), 1, state_name)
        && bind_text(update.get(), 2, queue_artifact.digest_algorithm)
        && bind_text(update.get(), 3, queue_artifact.digest)
        && bind_text(update.get(), 4, queue_artifact.kind)
        && sqlite3_bind_int64(update.get(), 5, now_epoch_seconds) == SQLITE_OK
        && bind_text(update.get(), 6, request_id)
        && bind_text(update.get(), 7, request_digest)
        && bind_text(update.get(), 8, lease_owner)
        && bind_text(update.get(), 9, state_name)
        && bind_text(update.get(), 10, queue_artifact.digest_algorithm)
        && bind_text(update.get(), 11, queue_artifact.digest)
        && bind_text(update.get(), 12, queue_artifact.kind)
        && step_done(update, error)
        && sqlite3_changes(impl_->database) == 1;
    if (update_ok && execute_sql(impl_->database, "COMMIT;", error)) return true;
    if (update_ok && error && error->empty())
        *error = "autonomy request completion could not commit";
    if (!update_ok && error && error->empty())
        *error = "autonomy request completion rejected by identity or state";
    execute_sql(impl_->database, "ROLLBACK;", nullptr);
    return false;
}

bool ProjectStore::sync_definition_recognition_cache(
    const ConversionReport& report,
    const ConversionOptions& options,
    DefinitionCacheSyncResult* result,
    std::string* error) {
    if (!is_open() && !open(error)) return false;
    DefinitionCacheSyncResult local;
    std::unordered_map<std::string, const AnalyzedSolid*> solids;
    for (const AnalyzedSolid& solid : report.solids) {
        solids.emplace(solid.id, &solid);
    }
    std::unordered_map<std::string, const ComponentInstance*> representatives;
    for (const ComponentInstance& instance : report.component_instances) {
        if (!instance.active || instance.source_shape_id.empty()
            || solids.find(instance.source_shape_id) == solids.end()) {
            continue;
        }
        representatives.emplace(instance.definition_id, &instance);
    }
    const std::string policy_digest = recognition_policy_digest(options);
    for (const ComponentDefinition& definition : report.component_definitions) {
        if (!definition.geometry_digest_verified
            || definition.geometry_digest.empty()) {
            ++local.skipped_unverified;
            continue;
        }
        const auto representative = representatives.find(definition.id);
        if (representative == representatives.end()) {
            ++local.skipped_unverified;
            continue;
        }
        const AnalyzedSolid& solid = *solids.at(
            representative->second->source_shape_id);
        const std::string contents = recognition_certificate_json(
            definition, solid, policy_digest);
        const std::string certificate_digest = digest_text(contents);
        const std::string cache_key = "definition-recognition-v1:"
            + digest_text(definition.geometry_digest + "|" + policy_digest);

        std::string lookup_error;
        const auto cached = find_conversion_cache(cache_key, &lookup_error);
        if (!lookup_error.empty()) {
            set_error(error, lookup_error);
            return false;
        }
        if (cached && cached->geometry_digest == definition.geometry_digest
            && cached->policy_digest == policy_digest
            && cached->certificate_digest == certificate_digest
            && cached->artifact.kind == "definition-recognition-v1") {
            ++local.hits;
            continue;
        }

        ++local.misses;
        ArtifactRecord artifact;
        if (!artifacts_.publish_bytes("definition-recognition-v1", "json",
                                      contents, &artifact, error)
            || !record_artifact(artifact, error)) {
            return false;
        }
        ConversionCacheEntry entry;
        entry.cache_key = cache_key;
        entry.geometry_digest = definition.geometry_digest;
        entry.policy_digest = policy_digest;
        entry.artifact = artifact;
        entry.certificate_digest = certificate_digest;
        if (!record_conversion_cache(entry, error)) return false;
        ++local.published;
    }
    if (result) *result = local;
    return true;
}

bool ProjectStore::enqueue_job(
    const ProjectJobSpec& job,
    const std::vector<std::string>& dependency_job_ids,
    std::string* error) {
    if (!is_open() && !open(error)) return false;
    if (job.job_id.empty() || job.target_id.empty() || job.operation.empty()
        || job.input_digest.empty() || job.policy_digest.empty()
        || job.max_attempts == 0) {
        set_error(error, "job specification has an empty required field or zero attempts");
        return false;
    }
    for (const std::string& dependency : dependency_job_ids) {
        if (dependency.empty() || dependency == job.job_id) {
            set_error(error, "job dependency is empty or self-referential");
            return false;
        }
    }
    sqlite3* database = impl_->database;
    if (!execute_sql(database, "BEGIN IMMEDIATE;", error)) return false;
    Statement insert(database,
        "INSERT INTO conversion_jobs VALUES(?,?,?,?,?,?,"
        "'queued',0,?,?,?,?,?, '',?,0,0,0,0,'','','','','','');");
    const sqlite3_int64 created_at = static_cast<sqlite3_int64>(std::time(nullptr));
    bool success = insert.valid()
        && bind_text(insert.get(), 1, job.job_id)
        && bind_text(insert.get(), 2, job.target_id)
        && bind_text(insert.get(), 3, job.operation)
        && bind_text(insert.get(), 4, job.input_digest)
        && bind_text(insert.get(), 5, job.policy_digest)
        && sqlite3_bind_int64(insert.get(), 6, job.stable_order) == SQLITE_OK
        && sqlite3_bind_int64(insert.get(), 7, job.max_attempts) == SQLITE_OK
        && sqlite3_bind_int64(insert.get(), 8,
                              static_cast<sqlite3_int64>(job.limits.memory_bytes)) == SQLITE_OK
        && sqlite3_bind_int64(insert.get(), 9,
                              static_cast<sqlite3_int64>(job.limits.wall_time_ms)) == SQLITE_OK
        && sqlite3_bind_int64(insert.get(), 10,
                              static_cast<sqlite3_int64>(job.limits.cpu_time_seconds)) == SQLITE_OK
        && sqlite3_bind_int64(insert.get(), 11,
                              static_cast<sqlite3_int64>(job.limits.file_size_bytes)) == SQLITE_OK
        && sqlite3_bind_int64(insert.get(), 12, created_at) == SQLITE_OK
        && step_done(insert, error);
    if (success) {
        for (const std::string& dependency : dependency_job_ids) {
            Statement edge(database,
                "INSERT INTO job_dependencies(job_id,depends_on_job_id) VALUES(?,?);");
            if (!edge.valid() || !bind_text(edge.get(), 1, job.job_id)
                || !bind_text(edge.get(), 2, dependency)
                || !step_done(edge, error)) {
                success = false;
                break;
            }
        }
    }
    if (success && execute_sql(database, "COMMIT;", error)) return true;
    execute_sql(database, "ROLLBACK;", nullptr);
    return false;
}

std::optional<ClaimedProjectJob> ProjectStore::claim_next_job(
    const std::string& worker_id,
    std::int64_t now_epoch_seconds,
    std::string* error) {
    if (!is_open() && !open(error)) return std::nullopt;
    if (worker_id.empty()) {
        set_error(error, "worker ID is empty");
        return std::nullopt;
    }
    sqlite3* database = impl_->database;
    if (!execute_sql(database, "BEGIN IMMEDIATE;", error)) return std::nullopt;
    Statement select(database,
        "SELECT j.job_id,j.target_id,j.operation,j.input_digest,j.policy_digest,"
        "j.stable_order,j.attempts,j.max_attempts,j.memory_bytes,j.wall_time_ms,"
        "j.cpu_time_seconds,j.file_size_bytes FROM conversion_jobs j "
        "WHERE j.state='queued' AND j.cancel_requested=0 AND NOT EXISTS("
        "SELECT 1 FROM job_dependencies e JOIN conversion_jobs d "
        "ON d.job_id=e.depends_on_job_id WHERE e.job_id=j.job_id "
        "AND d.state<>'succeeded') "
        "ORDER BY j.stable_order,j.target_id,j.job_id LIMIT 1;");
    if (!select.valid()) {
        set_error(error, select.error());
        execute_sql(database, "ROLLBACK;", nullptr);
        return std::nullopt;
    }
    const int selected = sqlite3_step(select.get());
    if (selected == SQLITE_DONE) {
        execute_sql(database, "COMMIT;", error);
        return std::nullopt;
    }
    if (selected != SQLITE_ROW) {
        set_error(error, select.error());
        execute_sql(database, "ROLLBACK;", nullptr);
        return std::nullopt;
    }
    ClaimedProjectJob result;
    result.spec.job_id = column_text(select.get(), 0);
    result.spec.target_id = column_text(select.get(), 1);
    result.spec.operation = column_text(select.get(), 2);
    result.spec.input_digest = column_text(select.get(), 3);
    result.spec.policy_digest = column_text(select.get(), 4);
    result.spec.stable_order = sqlite3_column_int64(select.get(), 5);
    result.attempt = static_cast<std::uint32_t>(sqlite3_column_int64(select.get(), 6) + 1);
    result.spec.max_attempts = static_cast<std::uint32_t>(sqlite3_column_int64(select.get(), 7));
    result.spec.limits.memory_bytes = static_cast<std::uint64_t>(sqlite3_column_int64(select.get(), 8));
    result.spec.limits.wall_time_ms = static_cast<std::uint64_t>(sqlite3_column_int64(select.get(), 9));
    result.spec.limits.cpu_time_seconds = static_cast<std::uint64_t>(sqlite3_column_int64(select.get(), 10));
    result.spec.limits.file_size_bytes = static_cast<std::uint64_t>(sqlite3_column_int64(select.get(), 11));
    result.state = ProjectJobState::running;
    result.worker_id = worker_id;

    Statement update(database,
        "UPDATE conversion_jobs SET state='running',attempts=attempts+1,"
        "worker_id=?,started_at=?,heartbeat_at=?,error_code='',error_message='' "
        "WHERE job_id=? AND state='queued';");
    const bool updated = update.valid()
        && bind_text(update.get(), 1, worker_id)
        && sqlite3_bind_int64(update.get(), 2, now_epoch_seconds) == SQLITE_OK
        && sqlite3_bind_int64(update.get(), 3, now_epoch_seconds) == SQLITE_OK
        && bind_text(update.get(), 4, result.spec.job_id)
        && step_done(update, error)
        && sqlite3_changes(database) == 1;
    if (!updated || !execute_sql(database, "COMMIT;", error)) {
        execute_sql(database, "ROLLBACK;", nullptr);
        return std::nullopt;
    }
    return result;
}

std::optional<ClaimedProjectJob> ProjectStore::claim_job(
    const std::string& job_id,
    const std::string& worker_id,
    std::int64_t now_epoch_seconds,
    std::string* error) {
    if (!is_open() && !open(error)) return std::nullopt;
    if (job_id.empty() || worker_id.empty()) {
        set_error(error, "job ID or worker ID is empty");
        return std::nullopt;
    }
    sqlite3* database = impl_->database;
    if (!execute_sql(database, "BEGIN IMMEDIATE;", error)) return std::nullopt;
    Statement select(database,
        "SELECT j.job_id,j.target_id,j.operation,j.input_digest,j.policy_digest,"
        "j.stable_order,j.attempts,j.max_attempts,j.memory_bytes,j.wall_time_ms,"
        "j.cpu_time_seconds,j.file_size_bytes FROM conversion_jobs j "
        "WHERE j.job_id=? AND j.state='queued' AND j.cancel_requested=0 "
        "AND NOT EXISTS(SELECT 1 FROM job_dependencies e "
        "JOIN conversion_jobs d ON d.job_id=e.depends_on_job_id "
        "WHERE e.job_id=j.job_id AND d.state<>'succeeded');");
    if (!select.valid() || !bind_text(select.get(), 1, job_id)) {
        set_error(error, select.error());
        execute_sql(database, "ROLLBACK;", nullptr);
        return std::nullopt;
    }
    const int selected = sqlite3_step(select.get());
    if (selected == SQLITE_DONE) {
        set_error(error, "requested job is not queued and dependency-ready");
        execute_sql(database, "ROLLBACK;", nullptr);
        return std::nullopt;
    }
    if (selected != SQLITE_ROW) {
        set_error(error, select.error());
        execute_sql(database, "ROLLBACK;", nullptr);
        return std::nullopt;
    }
    ClaimedProjectJob result;
    result.spec.job_id = column_text(select.get(), 0);
    result.spec.target_id = column_text(select.get(), 1);
    result.spec.operation = column_text(select.get(), 2);
    result.spec.input_digest = column_text(select.get(), 3);
    result.spec.policy_digest = column_text(select.get(), 4);
    result.spec.stable_order = sqlite3_column_int64(select.get(), 5);
    result.attempt = static_cast<std::uint32_t>(
        sqlite3_column_int64(select.get(), 6) + 1);
    result.spec.max_attempts = static_cast<std::uint32_t>(
        sqlite3_column_int64(select.get(), 7));
    result.spec.limits.memory_bytes = static_cast<std::uint64_t>(
        sqlite3_column_int64(select.get(), 8));
    result.spec.limits.wall_time_ms = static_cast<std::uint64_t>(
        sqlite3_column_int64(select.get(), 9));
    result.spec.limits.cpu_time_seconds = static_cast<std::uint64_t>(
        sqlite3_column_int64(select.get(), 10));
    result.spec.limits.file_size_bytes = static_cast<std::uint64_t>(
        sqlite3_column_int64(select.get(), 11));
    result.state = ProjectJobState::running;
    result.worker_id = worker_id;

    Statement update(database,
        "UPDATE conversion_jobs SET state='running',attempts=attempts+1,"
        "worker_id=?,started_at=?,heartbeat_at=?,error_code='',error_message='' "
        "WHERE job_id=? AND state='queued';");
    const bool updated = update.valid()
        && bind_text(update.get(), 1, worker_id)
        && sqlite3_bind_int64(update.get(), 2, now_epoch_seconds) == SQLITE_OK
        && sqlite3_bind_int64(update.get(), 3, now_epoch_seconds) == SQLITE_OK
        && bind_text(update.get(), 4, job_id)
        && step_done(update, error)
        && sqlite3_changes(database) == 1;
    if (!updated || !execute_sql(database, "COMMIT;", error)) {
        execute_sql(database, "ROLLBACK;", nullptr);
        return std::nullopt;
    }
    return result;
}

bool ProjectStore::heartbeat_job(const std::string& job_id,
                                 const std::string& worker_id,
                                 std::int64_t now_epoch_seconds,
                                 std::string* error) {
    if (!is_open() && !open(error)) return false;
    Statement update(impl_->database,
        "UPDATE conversion_jobs SET heartbeat_at=? WHERE job_id=? "
        "AND worker_id=? AND state='running';");
    if (!update.valid()
        || sqlite3_bind_int64(update.get(), 1, now_epoch_seconds) != SQLITE_OK
        || !bind_text(update.get(), 2, job_id)
        || !bind_text(update.get(), 3, worker_id)
        || !step_done(update, error)) return false;
    if (sqlite3_changes(impl_->database) == 1) return true;
    set_error(error, "job heartbeat rejected: job is not owned by this worker");
    return false;
}

bool ProjectStore::append_job_event(const JobProgressEvent& event,
                                    std::int64_t emitted_at,
                                    const std::string& artifact_digest,
                                    std::string* error) {
    if (!is_open() && !open(error)) return false;
    if (event.job_id.empty() || event.sequence == 0 || emitted_at < 0
        || event.sequence > static_cast<std::uint64_t>(
            std::numeric_limits<sqlite3_int64>::max())) {
        set_error(error, "job event has an empty ID, invalid sequence, or invalid timestamp");
        return false;
    }
    Statement insert(impl_->database,
        "INSERT INTO job_events(job_id,sequence,emitted_at,phase,completed,total,"
        "total_known,target_id,message_code,payload_json,artifact_digest) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?);");
    if (!insert.valid()
        || !bind_text(insert.get(), 1, event.job_id)
        || sqlite3_bind_int64(insert.get(), 2,
            static_cast<sqlite3_int64>(event.sequence)) != SQLITE_OK
        || sqlite3_bind_int64(insert.get(), 3, emitted_at) != SQLITE_OK
        || !bind_text(insert.get(), 4, job_phase_name(event.phase))
        || sqlite3_bind_int64(insert.get(), 5,
            static_cast<sqlite3_int64>(event.completed)) != SQLITE_OK
        || sqlite3_bind_int64(insert.get(), 6,
            static_cast<sqlite3_int64>(event.total)) != SQLITE_OK
        || sqlite3_bind_int(insert.get(), 7, event.total_known ? 1 : 0) != SQLITE_OK
        || !bind_text(insert.get(), 8, event.target_id)
        || !bind_text(insert.get(), 9, event.message_code)
        || !bind_text(insert.get(), 10, job_progress_event_json(event))
        || !bind_text(insert.get(), 11, artifact_digest)
        || !step_done(insert, error)) {
        if (error && error->empty())
            *error = "job event append failed (job missing or sequence already exists)";
        return false;
    }
    return true;
}

std::vector<StoredJobProgressEvent> ProjectStore::list_job_events(
    const std::string& job_id,
    std::uint64_t after_sequence,
    std::size_t limit,
    std::string* error) const {
    std::vector<StoredJobProgressEvent> result;
    if (!is_open()) {
        set_error(error, "project store is not open");
        return result;
    }
    if (job_id.empty() || limit == 0
        || after_sequence > static_cast<std::uint64_t>(
            std::numeric_limits<sqlite3_int64>::max())) {
        set_error(error, "invalid job event query");
        return result;
    }
    Statement query(impl_->database,
        "SELECT sequence,emitted_at,phase,completed,total,total_known,target_id,"
        "message_code,payload_json,artifact_digest FROM job_events "
        "WHERE job_id=? AND sequence>? ORDER BY sequence LIMIT ?;");
    if (!query.valid() || !bind_text(query.get(), 1, job_id)
        || sqlite3_bind_int64(query.get(), 2,
            static_cast<sqlite3_int64>(after_sequence)) != SQLITE_OK
        || sqlite3_bind_int64(query.get(), 3,
            static_cast<sqlite3_int64>(limit)) != SQLITE_OK) {
        set_error(error, query.error());
        return result;
    }
    while (true) {
        const int status = sqlite3_step(query.get());
        if (status == SQLITE_DONE) break;
        if (status != SQLITE_ROW) {
            set_error(error, query.error());
            return {};
        }
        StoredJobProgressEvent stored;
        std::string parse_error;
        if (!parse_job_progress_event_json(column_text(query.get(), 8),
                                           &stored.event, &parse_error)) {
            set_error(error, "stored job event is invalid: " + parse_error);
            return {};
        }
        const std::uint64_t sequence = static_cast<std::uint64_t>(
            sqlite3_column_int64(query.get(), 0));
        const std::size_t completed = static_cast<std::size_t>(
            sqlite3_column_int64(query.get(), 3));
        const std::size_t total = static_cast<std::size_t>(
            sqlite3_column_int64(query.get(), 4));
        if (stored.event.job_id != job_id || stored.event.sequence != sequence
            || std::string(job_phase_name(stored.event.phase)) != column_text(query.get(), 2)
            || stored.event.completed != completed || stored.event.total != total
            || stored.event.total_known != (sqlite3_column_int(query.get(), 5) != 0)
            || stored.event.target_id != column_text(query.get(), 6)
            || stored.event.message_code != column_text(query.get(), 7)) {
            set_error(error, "stored job event index does not match its payload");
            return {};
        }
        stored.emitted_at = sqlite3_column_int64(query.get(), 1);
        stored.artifact_digest = column_text(query.get(), 9);
        result.push_back(std::move(stored));
    }
    if (error) error->clear();
    return result;
}

std::uint64_t ProjectStore::latest_job_event_sequence(
    const std::string& job_id,
    std::string* error) const {
    if (!is_open()) {
        set_error(error, "project store is not open");
        return 0;
    }
    if (job_id.empty()) {
        set_error(error, "job ID is empty");
        return 0;
    }
    Statement query(impl_->database,
        "SELECT COALESCE(MAX(sequence),0) FROM job_events WHERE job_id=?;");
    if (!query.valid() || !bind_text(query.get(), 1, job_id)) {
        set_error(error, query.error());
        return 0;
    }
    if (sqlite3_step(query.get()) != SQLITE_ROW) {
        set_error(error, query.error());
        return 0;
    }
    const auto sequence = static_cast<std::uint64_t>(
        sqlite3_column_int64(query.get(), 0));
    if (error) error->clear();
    return sequence;
}

bool ProjectStore::complete_job(const std::string& job_id,
                                const std::string& worker_id,
                                const ArtifactRecord& artifact,
                                const std::string& certificate_digest,
                                std::int64_t now_epoch_seconds,
                                std::string* error) {
    if (!is_open() && !open(error)) return false;
    if (certificate_digest.empty() || !artifacts_.validate(artifact, error)) return false;
    const auto indexed = find_artifact(artifact.digest, artifact.kind, error);
    if (!indexed || indexed->digest_algorithm != artifact.digest_algorithm) {
        set_error(error, "job output artifact is not indexed in this project");
        return false;
    }
    Statement update(impl_->database,
        "UPDATE conversion_jobs SET state='succeeded',worker_id='',finished_at=?,"
        "heartbeat_at=?,cancel_requested=0,output_algorithm=?,output_digest=?,"
        "output_kind=?,certificate_digest=? WHERE job_id=? AND worker_id=? "
        "AND state='running' AND cancel_requested=0;");
    if (!update.valid()
        || sqlite3_bind_int64(update.get(), 1, now_epoch_seconds) != SQLITE_OK
        || sqlite3_bind_int64(update.get(), 2, now_epoch_seconds) != SQLITE_OK
        || !bind_text(update.get(), 3, artifact.digest_algorithm)
        || !bind_text(update.get(), 4, artifact.digest)
        || !bind_text(update.get(), 5, artifact.kind)
        || !bind_text(update.get(), 6, certificate_digest)
        || !bind_text(update.get(), 7, job_id)
        || !bind_text(update.get(), 8, worker_id)
        || !step_done(update, error)) return false;
    if (sqlite3_changes(impl_->database) == 1) return true;
    set_error(error, "job completion rejected: ownership changed or cancellation requested");
    return false;
}

bool ProjectStore::complete_job_with_event(
    const std::string& job_id,
    const std::string& worker_id,
    const ArtifactRecord& artifact,
    const std::string& certificate_digest,
    const JobProgressEvent& terminal_event,
    std::int64_t now_epoch_seconds,
    std::string* error) {
    if (!is_open() && !open(error)) return false;
    if (terminal_event.job_id != job_id
        || terminal_event.phase != JobPhase::finished
        || terminal_event.outcome != JobOutcome::committed) {
        set_error(error, "job completion requires a matching committed terminal event");
        return false;
    }
    if (!execute_sql(impl_->database, "BEGIN IMMEDIATE;", error)) return false;
    const bool success = append_job_event(
        terminal_event, now_epoch_seconds, artifact.digest, error)
        && complete_job(job_id, worker_id, artifact, certificate_digest,
                        now_epoch_seconds, error);
    if (success && execute_sql(impl_->database, "COMMIT;", error)) return true;
    execute_sql(impl_->database, "ROLLBACK;", nullptr);
    return false;
}

bool ProjectStore::fail_job(const std::string& job_id,
                            const std::string& worker_id,
                            const std::string& error_code,
                            const std::string& message,
                            bool retryable,
                            std::int64_t now_epoch_seconds,
                            std::string* error) {
    if (!is_open() && !open(error)) return false;
    Statement update(impl_->database,
        "UPDATE conversion_jobs SET state=CASE "
        "WHEN ?<>0 AND attempts<max_attempts AND cancel_requested=0 "
        "THEN 'queued' WHEN cancel_requested<>0 THEN 'cancelled' ELSE 'failed' END,"
        "worker_id='',heartbeat_at=?,finished_at=CASE "
        "WHEN ?<>0 AND attempts<max_attempts AND cancel_requested=0 THEN 0 ELSE ? END,"
        "error_code=?,error_message=? WHERE job_id=? AND worker_id=? AND state='running';");
    if (!update.valid()
        || sqlite3_bind_int(update.get(), 1, retryable ? 1 : 0) != SQLITE_OK
        || sqlite3_bind_int64(update.get(), 2, now_epoch_seconds) != SQLITE_OK
        || sqlite3_bind_int(update.get(), 3, retryable ? 1 : 0) != SQLITE_OK
        || sqlite3_bind_int64(update.get(), 4, now_epoch_seconds) != SQLITE_OK
        || !bind_text(update.get(), 5, error_code)
        || !bind_text(update.get(), 6, message)
        || !bind_text(update.get(), 7, job_id)
        || !bind_text(update.get(), 8, worker_id)
        || !step_done(update, error)) return false;
    if (sqlite3_changes(impl_->database) == 1) return true;
    set_error(error, "job failure rejected: job is not owned by this worker");
    return false;
}

bool ProjectStore::request_job_cancellation(const std::string& job_id,
                                            std::int64_t now_epoch_seconds,
                                            std::string* error) {
    if (!is_open() && !open(error)) return false;
    Statement update(impl_->database,
        "UPDATE conversion_jobs SET cancel_requested=1,"
        "state=CASE WHEN state='queued' THEN 'cancelled' ELSE state END,"
        "finished_at=CASE WHEN state='queued' THEN ? ELSE finished_at END "
        "WHERE job_id=? AND state IN('queued','running');");
    if (!update.valid()
        || sqlite3_bind_int64(update.get(), 1, now_epoch_seconds) != SQLITE_OK
        || !bind_text(update.get(), 2, job_id)
        || !step_done(update, error)) return false;
    if (sqlite3_changes(impl_->database) == 1) return true;
    set_error(error, "job cancellation rejected: job is not active");
    return false;
}

bool ProjectStore::job_cancellation_requested(const std::string& job_id,
                                              const std::string& worker_id,
                                              bool* requested,
                                              std::string* error) const {
    if (requested) *requested = false;
    if (!is_open()) {
        set_error(error, "project store is not open");
        return false;
    }
    Statement query(impl_->database,
        "SELECT cancel_requested FROM conversion_jobs WHERE job_id=? "
        "AND worker_id=? AND state='running';");
    if (!query.valid() || !bind_text(query.get(), 1, job_id)
        || !bind_text(query.get(), 2, worker_id)) {
        set_error(error, query.error());
        return false;
    }
    if (sqlite3_step(query.get()) != SQLITE_ROW) {
        set_error(error, "job cancellation query rejected: job is not owned by worker");
        return false;
    }
    if (requested) *requested = sqlite3_column_int(query.get(), 0) != 0;
    return true;
}

std::uint64_t ProjectStore::recover_abandoned_jobs(
    std::int64_t stale_before_epoch_seconds,
    std::int64_t now_epoch_seconds,
    std::string* error) {
    if (!is_open() && !open(error)) return 0;
    Statement update(impl_->database,
        "UPDATE conversion_jobs SET state=CASE "
        "WHEN cancel_requested<>0 THEN 'cancelled' "
        "WHEN attempts<max_attempts THEN 'queued' ELSE 'failed' END,"
        "worker_id='',finished_at=CASE WHEN attempts<max_attempts "
        "AND cancel_requested=0 THEN 0 ELSE ? END,error_code='worker_abandoned',"
        "error_message='Worker heartbeat expired before commit' "
        "WHERE state='running' AND heartbeat_at<?;");
    if (!update.valid()
        || sqlite3_bind_int64(update.get(), 1, now_epoch_seconds) != SQLITE_OK
        || sqlite3_bind_int64(update.get(), 2, stale_before_epoch_seconds) != SQLITE_OK
        || !step_done(update, error)) return 0;
    return static_cast<std::uint64_t>(sqlite3_changes(impl_->database));
}

bool ProjectStore::retry_failed_job(const std::string& job_id,
                                    std::int64_t now_epoch_seconds,
                                    std::string* error) {
    if (!is_open() && !open(error)) return false;
    if (job_id.empty()) {
        set_error(error, "job ID is empty");
        return false;
    }
    Statement update(impl_->database,
        "UPDATE conversion_jobs SET state='queued',max_attempts=CASE "
        "WHEN max_attempts<=attempts THEN attempts+1 ELSE max_attempts END,"
        "worker_id='',started_at=0,heartbeat_at=0,finished_at=0,"
        "cancel_requested=0,error_code='',error_message='',"
        "output_algorithm='',output_digest='',output_kind='',"
        "certificate_digest='' WHERE job_id=? AND state='failed';");
    if (!update.valid() || !bind_text(update.get(), 1, job_id)
        || !step_done(update, error)) return false;
    if (sqlite3_changes(impl_->database) == 1) return true;
    (void)now_epoch_seconds;
    set_error(error, "job is missing or is not failed: " + job_id);
    return false;
}

ProjectJobCounts ProjectStore::job_counts(std::string* error) const {
    ProjectJobCounts result;
    if (!is_open()) {
        set_error(error, "project store is not open");
        return result;
    }
    Statement query(impl_->database,
        "SELECT state,COUNT(*) FROM conversion_jobs GROUP BY state;");
    if (!query.valid()) {
        set_error(error, query.error());
        return result;
    }
    while (true) {
        const int status = sqlite3_step(query.get());
        if (status == SQLITE_DONE) break;
        if (status != SQLITE_ROW) {
            set_error(error, query.error());
            return {};
        }
        const std::string state = column_text(query.get(), 0);
        const std::uint64_t count = static_cast<std::uint64_t>(
            sqlite3_column_int64(query.get(), 1));
        if (state == "queued") result.queued = count;
        else if (state == "running") result.running = count;
        else if (state == "succeeded") result.succeeded = count;
        else if (state == "failed") result.failed = count;
        else if (state == "cancelled") result.cancelled = count;
    }
    return result;
}

std::vector<ProjectSourceSummary> ProjectStore::list_sources(
    std::size_t limit, std::size_t offset, std::string* error) const {
    std::vector<ProjectSourceSummary> result;
    if (!is_open()) {
        set_error(error, "project store is not open");
        return result;
    }
    Statement query(impl_->database,
        "SELECT source_path,source_digest FROM source_documents "
        "ORDER BY source_path,source_digest LIMIT ? OFFSET ?;");
    if (!query.valid()
        || sqlite3_bind_int64(query.get(), 1, static_cast<sqlite3_int64>(limit)) != SQLITE_OK
        || sqlite3_bind_int64(query.get(), 2, static_cast<sqlite3_int64>(offset)) != SQLITE_OK) {
        set_error(error, query.error());
        return result;
    }
    while (sqlite3_step(query.get()) == SQLITE_ROW) {
        result.push_back({column_text(query.get(), 0), column_text(query.get(), 1)});
    }
    return result;
}

std::vector<ProjectDefinitionSummary> ProjectStore::list_component_definitions(
    std::size_t limit, std::size_t offset, std::string* error) const {
    std::vector<ProjectDefinitionSummary> result;
    if (!is_open()) {
        set_error(error, "project store is not open");
        return result;
    }
    Statement query(impl_->database,
        "SELECT s.source_digest,d.definition_id,COALESCE(n.value,''),"
        "d.geometry_digest,d.geometry_digest_verified,COUNT(i.instance_id) "
        "FROM component_definitions d JOIN source_documents s ON s.source_id=d.source_id "
        "LEFT JOIN metadata_entities m ON m.source_id=d.source_id AND m.metadata_id=d.metadata_id "
        "LEFT JOIN interned_strings n ON n.source_id=d.source_id AND n.string_id=m.display_name "
        "LEFT JOIN component_instances i ON i.source_id=d.source_id AND i.definition_id=d.definition_id "
        "GROUP BY d.source_id,d.definition_id ORDER BY s.source_digest,d.definition_id LIMIT ? OFFSET ?;");
    if (!query.valid()
        || sqlite3_bind_int64(query.get(), 1, static_cast<sqlite3_int64>(limit)) != SQLITE_OK
        || sqlite3_bind_int64(query.get(), 2, static_cast<sqlite3_int64>(offset)) != SQLITE_OK) {
        set_error(error, query.error());
        return result;
    }
    while (sqlite3_step(query.get()) == SQLITE_ROW) {
        ProjectDefinitionSummary row;
        row.source_digest = column_text(query.get(), 0);
        row.definition_id = column_text(query.get(), 1);
        row.display_name = column_text(query.get(), 2);
        row.geometry_digest = column_text(query.get(), 3);
        row.geometry_digest_verified = sqlite3_column_int(query.get(), 4) != 0;
        row.instance_count = static_cast<std::uint64_t>(sqlite3_column_int64(query.get(), 5));
        result.push_back(std::move(row));
    }
    return result;
}

std::vector<ProjectInstanceSummary> ProjectStore::list_component_instances(
    std::size_t limit, std::size_t offset, std::string* error) const {
    std::vector<ProjectInstanceSummary> result;
    if (!is_open()) {
        set_error(error, "project store is not open");
        return result;
    }
    Statement query(impl_->database,
        "SELECT s.source_digest,i.instance_id,i.definition_id,i.parent_assembly_id,"
        "i.source_shape_id,COALESCE(n.value,''),i.material_override,i.active "
        "FROM component_instances i JOIN source_documents s ON s.source_id=i.source_id "
        "LEFT JOIN metadata_entities m ON m.source_id=i.source_id AND m.metadata_id=i.metadata_id "
        "LEFT JOIN interned_strings n ON n.source_id=i.source_id AND n.string_id=m.display_name "
        "ORDER BY s.source_digest,i.instance_id LIMIT ? OFFSET ?;");
    if (!query.valid()
        || sqlite3_bind_int64(query.get(), 1, static_cast<sqlite3_int64>(limit)) != SQLITE_OK
        || sqlite3_bind_int64(query.get(), 2, static_cast<sqlite3_int64>(offset)) != SQLITE_OK) {
        set_error(error, query.error());
        return result;
    }
    while (sqlite3_step(query.get()) == SQLITE_ROW) {
        ProjectInstanceSummary row;
        row.source_digest = column_text(query.get(), 0);
        row.instance_id = column_text(query.get(), 1);
        row.definition_id = column_text(query.get(), 2);
        row.parent_assembly_id = column_text(query.get(), 3);
        row.source_shape_id = column_text(query.get(), 4);
        row.display_name = column_text(query.get(), 5);
        row.material_override = column_text(query.get(), 6);
        row.active = sqlite3_column_int(query.get(), 7) != 0;
        result.push_back(std::move(row));
    }
    return result;
}

std::vector<ProjectDefinitionSummary> ProjectStore::search_component_definitions(
    const std::string& search, std::size_t limit, std::size_t offset,
    std::string* error) const {
    if (search.empty()) return list_component_definitions(limit, offset, error);
    std::vector<ProjectDefinitionSummary> result;
    if (!is_open()) {
        set_error(error, "project store is not open");
        return result;
    }
    Statement query(impl_->database,
        "SELECT s.source_digest,d.definition_id,COALESCE(n.value,''),"
        "d.geometry_digest,d.geometry_digest_verified,COUNT(i.instance_id) "
        "FROM component_definitions d JOIN source_documents s ON s.source_id=d.source_id "
        "LEFT JOIN metadata_entities m ON m.source_id=d.source_id AND m.metadata_id=d.metadata_id "
        "LEFT JOIN interned_strings n ON n.source_id=d.source_id AND n.string_id=m.display_name "
        "LEFT JOIN component_instances i ON i.source_id=d.source_id AND i.definition_id=d.definition_id "
        "WHERE lower(d.definition_id) LIKE lower(?) OR lower(COALESCE(n.value,'')) LIKE lower(?) "
        "OR lower(d.geometry_digest) LIKE lower(?) "
        "GROUP BY d.source_id,d.definition_id ORDER BY s.source_digest,d.definition_id LIMIT ? OFFSET ?;");
    const std::string pattern = "%" + search + "%";
    if (!query.valid() || !bind_text(query.get(), 1, pattern)
        || !bind_text(query.get(), 2, pattern) || !bind_text(query.get(), 3, pattern)
        || sqlite3_bind_int64(query.get(), 4, static_cast<sqlite3_int64>(limit)) != SQLITE_OK
        || sqlite3_bind_int64(query.get(), 5, static_cast<sqlite3_int64>(offset)) != SQLITE_OK) {
        set_error(error, query.error());
        return result;
    }
    while (true) {
        const int status = sqlite3_step(query.get());
        if (status == SQLITE_DONE) break;
        if (status != SQLITE_ROW) { set_error(error, query.error()); return {}; }
        ProjectDefinitionSummary row;
        row.source_digest = column_text(query.get(), 0);
        row.definition_id = column_text(query.get(), 1);
        row.display_name = column_text(query.get(), 2);
        row.geometry_digest = column_text(query.get(), 3);
        row.geometry_digest_verified = sqlite3_column_int(query.get(), 4) != 0;
        row.instance_count = static_cast<std::uint64_t>(sqlite3_column_int64(query.get(), 5));
        result.push_back(std::move(row));
    }
    return result;
}

std::vector<ProjectInstanceSummary> ProjectStore::search_component_instances(
    const std::string& search, std::size_t limit, std::size_t offset,
    std::string* error) const {
    if (search.empty()) return list_component_instances(limit, offset, error);
    std::vector<ProjectInstanceSummary> result;
    if (!is_open()) {
        set_error(error, "project store is not open");
        return result;
    }
    Statement query(impl_->database,
        "SELECT s.source_digest,i.instance_id,i.definition_id,i.parent_assembly_id,"
        "i.source_shape_id,COALESCE(n.value,''),i.material_override,i.active "
        "FROM component_instances i JOIN source_documents s ON s.source_id=i.source_id "
        "LEFT JOIN metadata_entities m ON m.source_id=i.source_id AND m.metadata_id=i.metadata_id "
        "LEFT JOIN interned_strings n ON n.source_id=i.source_id AND n.string_id=m.display_name "
        "WHERE lower(i.instance_id) LIKE lower(?) OR lower(i.definition_id) LIKE lower(?) "
        "OR lower(i.parent_assembly_id) LIKE lower(?) OR lower(i.source_shape_id) LIKE lower(?) "
        "OR lower(COALESCE(n.value,'')) LIKE lower(?) OR lower(i.material_override) LIKE lower(?) "
        "ORDER BY s.source_digest,i.instance_id LIMIT ? OFFSET ?;");
    const std::string pattern = "%" + search + "%";
    if (!query.valid() || !bind_text(query.get(), 1, pattern)
        || !bind_text(query.get(), 2, pattern) || !bind_text(query.get(), 3, pattern)
        || !bind_text(query.get(), 4, pattern) || !bind_text(query.get(), 5, pattern)
        || !bind_text(query.get(), 6, pattern)
        || sqlite3_bind_int64(query.get(), 7, static_cast<sqlite3_int64>(limit)) != SQLITE_OK
        || sqlite3_bind_int64(query.get(), 8, static_cast<sqlite3_int64>(offset)) != SQLITE_OK) {
        set_error(error, query.error());
        return result;
    }
    while (true) {
        const int status = sqlite3_step(query.get());
        if (status == SQLITE_DONE) break;
        if (status != SQLITE_ROW) { set_error(error, query.error()); return {}; }
        ProjectInstanceSummary row;
        row.source_digest = column_text(query.get(), 0);
        row.instance_id = column_text(query.get(), 1);
        row.definition_id = column_text(query.get(), 2);
        row.parent_assembly_id = column_text(query.get(), 3);
        row.source_shape_id = column_text(query.get(), 4);
        row.display_name = column_text(query.get(), 5);
        row.material_override = column_text(query.get(), 6);
        row.active = sqlite3_column_int(query.get(), 7) != 0;
        result.push_back(std::move(row));
    }
    return result;
}

std::vector<ProjectJobSummary> ProjectStore::list_jobs(
    std::size_t limit, std::size_t offset, const std::string& state,
    std::string* error) const {
    std::vector<ProjectJobSummary> result;
    if (!is_open()) {
        set_error(error, "project store is not open");
        return result;
    }
    const std::string sql =
        "SELECT j.job_id,j.target_id,j.operation,j.state,j.attempts,j.max_attempts,"
        "j.worker_id,j.error_code,j.error_message,j.started_at,j.heartbeat_at,"
        "j.finished_at,COALESCE(e.sequence,0),COALESCE(e.phase,'queued'),"
        "COALESCE(e.completed,0),COALESCE(e.total,0),COALESCE(e.total_known,0) "
        "FROM conversion_jobs j LEFT JOIN job_events e ON e.job_id=j.job_id "
        "AND e.sequence=(SELECT MAX(latest.sequence) FROM job_events latest "
        "WHERE latest.job_id=j.job_id) "
        + std::string(state.empty() ? "" : "WHERE j.state=? ")
        + "ORDER BY j.stable_order,j.target_id,j.job_id LIMIT ? OFFSET ?;";
    Statement query(impl_->database, sql.c_str());
    if (!query.valid()) {
        set_error(error, query.error());
        return result;
    }
    int parameter = 1;
    if ((!state.empty() && !bind_text(query.get(), parameter++, state))
        || sqlite3_bind_int64(query.get(), parameter++, static_cast<sqlite3_int64>(limit)) != SQLITE_OK
        || sqlite3_bind_int64(query.get(), parameter, static_cast<sqlite3_int64>(offset)) != SQLITE_OK) {
        set_error(error, query.error());
        return result;
    }
    while (sqlite3_step(query.get()) == SQLITE_ROW) {
        ProjectJobSummary row;
        row.job_id = column_text(query.get(), 0);
        row.target_id = column_text(query.get(), 1);
        row.operation = column_text(query.get(), 2);
        const std::string job_state = column_text(query.get(), 3);
        if (job_state == "running") row.state = ProjectJobState::running;
        else if (job_state == "succeeded") row.state = ProjectJobState::succeeded;
        else if (job_state == "failed") row.state = ProjectJobState::failed;
        else if (job_state == "cancelled") row.state = ProjectJobState::cancelled;
        row.attempt = static_cast<std::uint32_t>(sqlite3_column_int(query.get(), 4));
        row.max_attempts = static_cast<std::uint32_t>(sqlite3_column_int(query.get(), 5));
        row.worker_id = column_text(query.get(), 6);
        row.error_code = column_text(query.get(), 7);
        row.error_message = column_text(query.get(), 8);
        row.started_at = sqlite3_column_int64(query.get(), 9);
        row.heartbeat_at = sqlite3_column_int64(query.get(), 10);
        row.finished_at = sqlite3_column_int64(query.get(), 11);
        row.latest_sequence = static_cast<std::uint64_t>(
            sqlite3_column_int64(query.get(), 12));
        const std::string phase = column_text(query.get(), 13);
        for (int raw = static_cast<int>(JobPhase::queued);
             raw <= static_cast<int>(JobPhase::finished); ++raw) {
            const JobPhase candidate = static_cast<JobPhase>(raw);
            if (phase == job_phase_name(candidate)) {
                row.latest_phase = candidate;
                break;
            }
        }
        row.completed = static_cast<std::size_t>(
            sqlite3_column_int64(query.get(), 14));
        row.total = static_cast<std::size_t>(
            sqlite3_column_int64(query.get(), 15));
        row.total_known = sqlite3_column_int(query.get(), 16) != 0;
        result.push_back(std::move(row));
    }
    return result;
}

std::vector<ArtifactRecord> ProjectStore::list_artifacts(
    std::size_t limit, std::size_t offset, const std::string& kind,
    std::string* error) const {
    std::vector<ArtifactRecord> result;
    if (!is_open()) {
        set_error(error, "project store is not open");
        return result;
    }
    const std::string sql =
        "SELECT digest_algorithm,digest,kind,relative_path,size FROM artifacts "
        + std::string(kind.empty() ? "" : "WHERE kind=? ")
        + "ORDER BY kind,digest LIMIT ? OFFSET ?;";
    Statement query(impl_->database, sql.c_str());
    if (!query.valid()) {
        set_error(error, query.error());
        return result;
    }
    int parameter = 1;
    if ((!kind.empty() && !bind_text(query.get(), parameter++, kind))
        || sqlite3_bind_int64(query.get(), parameter++, static_cast<sqlite3_int64>(limit)) != SQLITE_OK
        || sqlite3_bind_int64(query.get(), parameter, static_cast<sqlite3_int64>(offset)) != SQLITE_OK) {
        set_error(error, query.error());
        return result;
    }
    while (sqlite3_step(query.get()) == SQLITE_ROW) {
        ArtifactRecord row;
        row.digest_algorithm = column_text(query.get(), 0);
        row.digest = column_text(query.get(), 1);
        row.kind = column_text(query.get(), 2);
        row.relative_path = column_text(query.get(), 3);
        row.size = static_cast<std::uint64_t>(sqlite3_column_int64(query.get(), 4));
        row.deduplicated = true;
        result.push_back(std::move(row));
    }
    return result;
}

std::vector<std::string> ProjectStore::list_spatial_namespaces(
    std::string* error) const {
    std::vector<std::string> result;
    if (!is_open()) {
        set_error(error, "project store is not open");
        return result;
    }
    Statement query(impl_->database,
        "SELECT DISTINCT model_namespace FROM spatial_entities ORDER BY model_namespace;");
    if (!query.valid()) { set_error(error, query.error()); return result; }
    while (true) {
        const int status = sqlite3_step(query.get());
        if (status == SQLITE_DONE) break;
        if (status != SQLITE_ROW) { set_error(error, query.error()); return {}; }
        result.push_back(column_text(query.get(), 0));
    }
    return result;
}

std::vector<SpatialEntry> ProjectStore::list_spatial_entries(
    const std::string& model_namespace, std::size_t limit, std::size_t offset,
    const std::string& kind, const std::string& search,
    std::string* error) const {
    std::vector<SpatialEntry> result;
    if (!is_open()) {
        set_error(error, "project store is not open");
        return result;
    }
    std::string sql =
        "SELECT e.entity_id,e.owner_id,e.kind,r.xmin,r.ymin,r.zmin,r.xmax,r.ymax,r.zmax "
        "FROM spatial_entities e JOIN spatial_rtree r ON r.spatial_id=e.spatial_id "
        "WHERE e.model_namespace=? ";
    if (!kind.empty()) sql += "AND e.kind=? ";
    if (!search.empty())
        sql += "AND (lower(e.entity_id) LIKE lower(?) OR lower(e.owner_id) LIKE lower(?)) ";
    sql += "ORDER BY e.kind,e.entity_id LIMIT ? OFFSET ?;";
    Statement query(impl_->database, sql.c_str());
    if (!query.valid()) { set_error(error, query.error()); return result; }
    int parameter = 1;
    const std::string pattern = "%" + search + "%";
    if (!bind_text(query.get(), parameter++, model_namespace)
        || (!kind.empty() && !bind_text(query.get(), parameter++, kind))
        || (!search.empty() && (!bind_text(query.get(), parameter++, pattern)
                                || !bind_text(query.get(), parameter++, pattern)))
        || sqlite3_bind_int64(query.get(), parameter++, static_cast<sqlite3_int64>(limit)) != SQLITE_OK
        || sqlite3_bind_int64(query.get(), parameter, static_cast<sqlite3_int64>(offset)) != SQLITE_OK) {
        set_error(error, query.error());
        return result;
    }
    while (true) {
        const int status = sqlite3_step(query.get());
        if (status == SQLITE_DONE) break;
        if (status != SQLITE_ROW) { set_error(error, query.error()); return {}; }
        SpatialEntry row;
        row.entity_id = column_text(query.get(), 0);
        row.owner_id = column_text(query.get(), 1);
        row.kind = column_text(query.get(), 2);
        row.bounds = {sqlite3_column_double(query.get(), 3),
                      sqlite3_column_double(query.get(), 4),
                      sqlite3_column_double(query.get(), 5),
                      sqlite3_column_double(query.get(), 6),
                      sqlite3_column_double(query.get(), 7),
                      sqlite3_column_double(query.get(), 8), true};
        result.push_back(std::move(row));
    }
    return result;
}

bool ProjectStore::replace_spatial_entries(
    const std::string& model_namespace,
    const std::vector<SpatialEntry>& entries,
    std::string* error) {
    if (!is_open() && !open(error)) return false;
    if (model_namespace.empty()) {
        set_error(error, "spatial model namespace is empty");
        return false;
    }
    for (const SpatialEntry& entry : entries) {
        if (entry.entity_id.empty() || entry.kind.empty()
            || !valid_spatial_bounds(entry.bounds)) {
            set_error(error, "spatial entry has an invalid ID, kind, or bounding box");
            return false;
        }
    }
    sqlite3* database = impl_->database;
    if (!execute_sql(database, "BEGIN IMMEDIATE;", error)) return false;
    bool success = false;
    do {
        Statement remove_index(database,
            "DELETE FROM spatial_rtree WHERE spatial_id IN ("
            "SELECT spatial_id FROM spatial_entities WHERE model_namespace=?);");
        if (!remove_index.valid()
            || !bind_text(remove_index.get(), 1, model_namespace)
            || !step_done(remove_index, error)) break;
        Statement remove_entities(database,
            "DELETE FROM spatial_entities WHERE model_namespace=?;");
        if (!remove_entities.valid()
            || !bind_text(remove_entities.get(), 1, model_namespace)
            || !step_done(remove_entities, error)) break;

        bool inserted = true;
        for (const SpatialEntry& entry : entries) {
            Statement entity(database,
                "INSERT INTO spatial_entities(model_namespace,entity_id,owner_id,kind) "
                "VALUES(?,?,?,?);");
            if (!entity.valid()
                || !bind_text(entity.get(), 1, model_namespace)
                || !bind_text(entity.get(), 2, entry.entity_id)
                || !bind_text(entity.get(), 3, entry.owner_id)
                || !bind_text(entity.get(), 4, entry.kind)
                || !step_done(entity, error)) {
                inserted = false;
                break;
            }
            const sqlite3_int64 spatial_id = sqlite3_last_insert_rowid(database);
            Statement bounds(database,
                "INSERT INTO spatial_rtree VALUES(?,?,?,?,?,?,?);");
            if (!bounds.valid()
                || sqlite3_bind_int64(bounds.get(), 1, spatial_id) != SQLITE_OK
                || sqlite3_bind_double(bounds.get(), 2, entry.bounds.xmin) != SQLITE_OK
                || sqlite3_bind_double(bounds.get(), 3, entry.bounds.xmax) != SQLITE_OK
                || sqlite3_bind_double(bounds.get(), 4, entry.bounds.ymin) != SQLITE_OK
                || sqlite3_bind_double(bounds.get(), 5, entry.bounds.ymax) != SQLITE_OK
                || sqlite3_bind_double(bounds.get(), 6, entry.bounds.zmin) != SQLITE_OK
                || sqlite3_bind_double(bounds.get(), 7, entry.bounds.zmax) != SQLITE_OK
                || !step_done(bounds, error)) {
                inserted = false;
                break;
            }
        }
        if (!inserted) break;
        success = true;
    } while (false);
    if (success && execute_sql(database, "COMMIT;", error)) return true;
    execute_sql(database, "ROLLBACK;", nullptr);
    return false;
}

std::vector<SpatialEntry> ProjectStore::query_spatial_entries(
    const std::string& model_namespace,
    const SpatialBounds& bounds,
    const std::string& kind,
    double expansion,
    std::string* error) const {
    std::vector<SpatialEntry> result;
    if (!is_open()) {
        set_error(error, "project store is not open");
        return result;
    }
    if (model_namespace.empty() || !valid_spatial_bounds(bounds)
        || !std::isfinite(expansion) || expansion < 0.0) {
        set_error(error, "invalid spatial query namespace, bounds, or expansion");
        return result;
    }
    const std::string sql =
        "SELECT e.entity_id,e.owner_id,e.kind,r.xmin,r.ymin,r.zmin,"
        "r.xmax,r.ymax,r.zmax FROM spatial_entities e JOIN spatial_rtree r "
        "ON r.spatial_id=e.spatial_id WHERE e.model_namespace=? "
        + std::string(kind.empty() ? "" : "AND e.kind=? ")
        + "AND r.xmax>=? AND r.xmin<=? AND r.ymax>=? AND r.ymin<=? "
          "AND r.zmax>=? AND r.zmin<=? ORDER BY e.kind,e.entity_id;";
    Statement query(impl_->database, sql.c_str());
    if (!query.valid()) {
        set_error(error, query.error());
        return result;
    }
    int parameter = 1;
    if (!bind_text(query.get(), parameter++, model_namespace)
        || (!kind.empty() && !bind_text(query.get(), parameter++, kind))
        || sqlite3_bind_double(query.get(), parameter++, bounds.xmin - expansion) != SQLITE_OK
        || sqlite3_bind_double(query.get(), parameter++, bounds.xmax + expansion) != SQLITE_OK
        || sqlite3_bind_double(query.get(), parameter++, bounds.ymin - expansion) != SQLITE_OK
        || sqlite3_bind_double(query.get(), parameter++, bounds.ymax + expansion) != SQLITE_OK
        || sqlite3_bind_double(query.get(), parameter++, bounds.zmin - expansion) != SQLITE_OK
        || sqlite3_bind_double(query.get(), parameter++, bounds.zmax + expansion) != SQLITE_OK) {
        set_error(error, query.error());
        return result;
    }
    while (true) {
        const int status = sqlite3_step(query.get());
        if (status == SQLITE_DONE) break;
        if (status != SQLITE_ROW) {
            set_error(error, query.error());
            return {};
        }
        SpatialEntry entry;
        entry.entity_id = column_text(query.get(), 0);
        entry.owner_id = column_text(query.get(), 1);
        entry.kind = column_text(query.get(), 2);
        entry.bounds = {
            sqlite3_column_double(query.get(), 3),
            sqlite3_column_double(query.get(), 4),
            sqlite3_column_double(query.get(), 5),
            sqlite3_column_double(query.get(), 6),
            sqlite3_column_double(query.get(), 7),
            sqlite3_column_double(query.get(), 8), true};
        result.push_back(std::move(entry));
    }
    return result;
}

std::vector<SpatialCandidatePair> ProjectStore::spatial_overlap_candidates(
    const std::string& model_namespace,
    const std::string& kind,
    double expansion,
    std::size_t maximum_pairs,
    std::string* error) const {
    std::vector<SpatialCandidatePair> result;
    if (!is_open()) {
        set_error(error, "project store is not open");
        return result;
    }
    if (model_namespace.empty() || kind.empty()
        || !std::isfinite(expansion) || expansion < 0.0) {
        set_error(error, "invalid spatial candidate namespace, kind, or expansion");
        return result;
    }
    Statement query(impl_->database,
        "SELECT CASE WHEN ea.entity_id<eb.entity_id THEN ea.entity_id ELSE eb.entity_id END,"
        "CASE WHEN ea.entity_id<eb.entity_id THEN eb.entity_id ELSE ea.entity_id END "
        "FROM spatial_entities ea JOIN spatial_rtree ra ON ra.spatial_id=ea.spatial_id "
        "JOIN spatial_entities eb ON eb.model_namespace=ea.model_namespace "
        "AND eb.kind=ea.kind AND eb.spatial_id>ea.spatial_id "
        "JOIN spatial_rtree rb ON rb.spatial_id=eb.spatial_id "
        "WHERE ea.model_namespace=? AND ea.kind=? "
        "AND ra.xmax+?>=rb.xmin AND rb.xmax+?>=ra.xmin "
        "AND ra.ymax+?>=rb.ymin AND rb.ymax+?>=ra.ymin "
        "AND ra.zmax+?>=rb.zmin AND rb.zmax+?>=ra.zmin "
        "ORDER BY 1,2;");
    if (!query.valid() || !bind_text(query.get(), 1, model_namespace)
        || !bind_text(query.get(), 2, kind)) {
        set_error(error, query.error());
        return result;
    }
    for (int parameter = 3; parameter <= 8; ++parameter) {
        if (sqlite3_bind_double(query.get(), parameter, expansion) != SQLITE_OK) {
            set_error(error, query.error());
            return {};
        }
    }
    while (maximum_pairs == 0 || result.size() < maximum_pairs) {
        const int status = sqlite3_step(query.get());
        if (status == SQLITE_DONE) break;
        if (status != SQLITE_ROW) {
            set_error(error, query.error());
            return {};
        }
        result.emplace_back(column_text(query.get(), 0), column_text(query.get(), 1));
    }
    return result;
}

ProjectCounts ProjectStore::counts(std::string* error) const {
    ProjectCounts result;
    if (!is_open()) {
        set_error(error, "project store is not open");
        return result;
    }
    struct Query { const char* table; std::uint64_t ProjectCounts::*field; };
    const Query queries[] = {
        {"source_documents", &ProjectCounts::sources},
        {"interned_strings", &ProjectCounts::strings},
        {"metadata_entities", &ProjectCounts::metadata_entities},
        {"component_definitions", &ProjectCounts::component_definitions},
        {"component_instances", &ProjectCounts::component_instances},
        {"materials", &ProjectCounts::materials},
        {"artifacts", &ProjectCounts::artifacts},
        {"conversion_cache", &ProjectCounts::conversion_cache_entries},
        {"spatial_entities", &ProjectCounts::spatial_entities},
        {"autonomy_requests", &ProjectCounts::autonomy_requests},
        {"agent_conversations", &ProjectCounts::agent_conversations},
        {"agent_messages", &ProjectCounts::agent_messages},
    };
    for (const Query& query_spec : queries) {
        const std::string sql = std::string("SELECT COUNT(*) FROM ")
            + query_spec.table + ';';
        Statement query(impl_->database, sql.c_str());
        if (!query.valid() || sqlite3_step(query.get()) != SQLITE_ROW) {
            set_error(error, query.error());
            return {};
        }
        result.*(query_spec.field) = static_cast<std::uint64_t>(
            sqlite3_column_int64(query.get(), 0));
    }
    return result;
}

} // namespace csgify

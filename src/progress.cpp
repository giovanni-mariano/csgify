#include "csgify/progress.hpp"

#include "json_parser.hpp"

#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace csgify {
namespace {

std::string escape_json(const std::string& value) {
    std::ostringstream out;
    for (const unsigned char ch : value) {
        switch (ch) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (ch < 0x20) {
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<unsigned>(ch) << std::dec;
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
    return "info";
}

bool parse_severity(const std::string& name, DiagnosticSeverity* value) {
    if (name == "info") *value = DiagnosticSeverity::info;
    else if (name == "warning") *value = DiagnosticSeverity::warning;
    else if (name == "error") *value = DiagnosticSeverity::error;
    else return false;
    return true;
}

template <typename Enum>
bool parse_named(const std::string& name, const char* (*to_name)(Enum),
                 Enum first, Enum last, Enum* result) {
    for (int raw = static_cast<int>(first); raw <= static_cast<int>(last); ++raw) {
        const Enum value = static_cast<Enum>(raw);
        if (name == to_name(value)) {
            *result = value;
            return true;
        }
    }
    return false;
}

std::size_t size_value(const JsonValue* value, const char* field) {
    if (!value || value->kind != JsonKind::number || value->num < 0.0
        || !std::isfinite(value->num)
        || value->num > static_cast<double>(std::numeric_limits<std::size_t>::max())) {
        throw std::runtime_error(std::string("invalid ") + field);
    }
    return static_cast<std::size_t>(value->num);
}

std::uint64_t sequence_value(const JsonValue* value) {
    if (!value || value->kind != JsonKind::number || value->num < 0.0
        || !std::isfinite(value->num)) {
        throw std::runtime_error("invalid sequence");
    }
    return static_cast<std::uint64_t>(value->num);
}

std::string string_value(const JsonValue* value, const char* field) {
    if (!value || value->kind != JsonKind::string)
        throw std::runtime_error(std::string("invalid ") + field);
    return value->str;
}

} // namespace

const char* job_operation_name(JobOperation operation) {
    switch (operation) {
    case JobOperation::analyze: return "analyze";
    case JobOperation::batch_remove: return "batch_remove";
    case JobOperation::fixed_point_decomposition: return "fixed_point_decomposition";
    case JobOperation::decomposition_preview: return "decomposition_preview";
    case JobOperation::export_step: return "export_step";
    case JobOperation::emit_csg: return "emit_csg";
    case JobOperation::alea_void_generation: return "alea_void_generation";
    }
    return "analyze";
}

const char* job_phase_name(JobPhase phase) {
    switch (phase) {
    case JobPhase::queued: return "queued";
    case JobPhase::resolving_targets: return "resolving_targets";
    case JobPhase::loading_geometry: return "loading_geometry";
    case JobPhase::analyzing_solids: return "analyzing_solids";
    case JobPhase::staging_mutation: return "staging_mutation";
    case JobPhase::certifying_solids: return "certifying_solids";
    case JobPhase::checking_overlaps: return "checking_overlaps";
    case JobPhase::terminal_verification: return "terminal_verification";
    case JobPhase::committing: return "committing";
    case JobPhase::awaiting_review: return "awaiting_review";
    case JobPhase::finished: return "finished";
    }
    return "queued";
}

const char* job_outcome_name(JobOutcome outcome) {
    switch (outcome) {
    case JobOutcome::none: return "none";
    case JobOutcome::committed: return "committed";
    case JobOutcome::certified_partial: return "certified_partial";
    case JobOutcome::cancelled: return "cancelled";
    case JobOutcome::rolled_back: return "rolled_back";
    case JobOutcome::failed: return "failed";
    }
    return "none";
}

std::string job_progress_event_json(const JobProgressEvent& event) {
    std::ostringstream out;
    out << std::setprecision(17)
        << "{\"schema\":\"csgify.job-progress\",\"schema_version\":"
        << event.schema_version << ",\"job_id\":\"" << escape_json(event.job_id)
        << "\",\"sequence\":" << event.sequence << ",\"operation\":\""
        << job_operation_name(event.operation) << "\",\"phase\":\""
        << job_phase_name(event.phase) << "\",\"outcome\":\""
        << job_outcome_name(event.outcome) << "\",\"message_code\":\""
        << escape_json(event.message_code) << "\",\"target_id\":\""
        << escape_json(event.target_id) << "\",\"candidate_id\":\""
        << escape_json(event.candidate_id) << "\",\"strategy\":\""
        << escape_json(event.strategy) << "\",\"completed\":" << event.completed
        << ",\"total\":" << event.total << ",\"total_known\":"
        << (event.total_known ? "true" : "false") << ",\"elapsed_ms\":"
        << event.elapsed_ms << ",\"base_revision\":" << event.base_revision
        << ",\"committed_revision\":" << event.committed_revision
        << ",\"actions_applied\":" << event.actions_applied
        << ",\"candidates_evaluated\":" << event.candidates_evaluated
        << ",\"splitter_calls\":" << event.splitter_calls
        << ",\"residual_count\":" << event.residual_count
        << ",\"active_piece_count\":" << event.active_piece_count
        << ",\"residual_volume\":" << event.residual_volume
        << ",\"artifact_id\":\"" << escape_json(event.artifact_id)
        << "\",\"changed_shape_ids\":[";
    for (std::size_t i = 0; i < event.changed_shape_ids.size(); ++i) {
        if (i) out << ',';
        out << '"' << escape_json(event.changed_shape_ids[i]) << '"';
    }
    out << "],\"diagnostics\":[";
    for (std::size_t i = 0; i < event.diagnostics.size(); ++i) {
        if (i) out << ',';
        const Diagnostic& d = event.diagnostics[i];
        out << "{\"severity\":\"" << severity_name(d.severity)
            << "\",\"code\":\"" << escape_json(d.code)
            << "\",\"message\":\"" << escape_json(d.message)
            << "\",\"shape_label\":\"" << escape_json(d.shape_label)
            << "\",\"shape_id\":\"" << escape_json(d.shape_id)
            << "\",\"document_id\":\"" << escape_json(d.document_id)
            << "\",\"local_shape_id\":\"" << escape_json(d.local_shape_id)
            << "\"}";
    }
    out << "]}";
    return out.str();
}

bool parse_job_progress_event_json(const std::string& json,
                                   JobProgressEvent* event,
                                   std::string* error) {
    if (!event) {
        if (error) *error = "output event is null";
        return false;
    }
    try {
        const std::shared_ptr<JsonValue> root = JsonParser(json).parse();
        if (!root || root->kind != JsonKind::object)
            throw std::runtime_error("root is not an object");
        if (string_value(root->get("schema"), "schema") != "csgify.job-progress")
            throw std::runtime_error("unsupported schema");
        JobProgressEvent parsed;
        parsed.schema_version = static_cast<unsigned>(
            size_value(root->get("schema_version"), "schema_version"));
        if (parsed.schema_version != 1)
            throw std::runtime_error("unsupported schema version");
        parsed.job_id = string_value(root->get("job_id"), "job_id");
        parsed.sequence = sequence_value(root->get("sequence"));
        const std::string operation = string_value(root->get("operation"), "operation");
        const std::string phase = string_value(root->get("phase"), "phase");
        const std::string outcome = string_value(root->get("outcome"), "outcome");
        if (!parse_named(operation, job_operation_name, JobOperation::analyze,
                         JobOperation::alea_void_generation, &parsed.operation)
            || !parse_named(phase, job_phase_name, JobPhase::queued,
                            JobPhase::finished, &parsed.phase)
            || !parse_named(outcome, job_outcome_name, JobOutcome::none,
                            JobOutcome::failed, &parsed.outcome)) {
            throw std::runtime_error("unknown operation, phase, or outcome");
        }
        parsed.message_code = string_value(root->get("message_code"), "message_code");
        parsed.target_id = string_value(root->get("target_id"), "target_id");
        parsed.candidate_id = string_value(root->get("candidate_id"), "candidate_id");
        parsed.strategy = string_value(root->get("strategy"), "strategy");
        parsed.completed = size_value(root->get("completed"), "completed");
        parsed.total = size_value(root->get("total"), "total");
        const JsonValue* known = root->get("total_known");
        if (!known || known->kind != JsonKind::boolean)
            throw std::runtime_error("invalid total_known");
        parsed.total_known = known->b;
        const JsonValue* elapsed = root->get("elapsed_ms");
        if (!elapsed || elapsed->kind != JsonKind::number || elapsed->num < 0.0
            || !std::isfinite(elapsed->num))
            throw std::runtime_error("invalid elapsed_ms");
        parsed.elapsed_ms = elapsed->num;
        parsed.base_revision = size_value(root->get("base_revision"), "base_revision");
        parsed.committed_revision = size_value(root->get("committed_revision"), "committed_revision");
        parsed.actions_applied = size_value(root->get("actions_applied"), "actions_applied");
        parsed.candidates_evaluated = size_value(root->get("candidates_evaluated"), "candidates_evaluated");
        parsed.splitter_calls = size_value(root->get("splitter_calls"), "splitter_calls");
        parsed.residual_count = size_value(root->get("residual_count"), "residual_count");
        parsed.active_piece_count = size_value(root->get("active_piece_count"), "active_piece_count");
        const JsonValue* residual = root->get("residual_volume");
        if (!residual || residual->kind != JsonKind::number || !std::isfinite(residual->num))
            throw std::runtime_error("invalid residual_volume");
        parsed.residual_volume = residual->num;
        parsed.artifact_id = string_value(root->get("artifact_id"), "artifact_id");
        const JsonValue* changed = root->get("changed_shape_ids");
        if (!changed || changed->kind != JsonKind::array)
            throw std::runtime_error("invalid changed_shape_ids");
        for (const auto& value : changed->arr)
            parsed.changed_shape_ids.push_back(string_value(value.get(), "changed shape ID"));
        const JsonValue* diagnostics = root->get("diagnostics");
        if (!diagnostics || diagnostics->kind != JsonKind::array)
            throw std::runtime_error("invalid diagnostics");
        for (const auto& value : diagnostics->arr) {
            if (!value || value->kind != JsonKind::object)
                throw std::runtime_error("invalid diagnostic");
            Diagnostic d;
            if (!parse_severity(string_value(value->get("severity"), "severity"),
                                &d.severity))
                throw std::runtime_error("unknown diagnostic severity");
            d.code = string_value(value->get("code"), "diagnostic code");
            d.message = string_value(value->get("message"), "diagnostic message");
            d.shape_label = string_value(value->get("shape_label"), "shape_label");
            d.shape_id = string_value(value->get("shape_id"), "shape_id");
            d.document_id = string_value(value->get("document_id"), "document_id");
            d.local_shape_id = string_value(value->get("local_shape_id"), "local_shape_id");
            parsed.diagnostics.push_back(std::move(d));
        }
        *event = std::move(parsed);
        if (error) error->clear();
        return true;
    } catch (const std::exception& exception) {
        if (error) *error = exception.what();
        return false;
    }
}

} // namespace csgify

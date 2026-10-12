#include "decision_log.hpp"

#include "csgify/engine.hpp"  // occt_version()
#include "fnv.hpp"
#include "json_parser.hpp"
#include "operation_registry.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <ios>
#include <sstream>
#include <stdexcept>

namespace csgify {
namespace {

// Local JSON helpers — small enough that pulling report.cpp's namespace in
// would not be worth the coupling.

std::string json_escape(const std::string& value) {
    std::ostringstream out;
    for (char ch : value) {
        switch (ch) {
        case '\\': out << "\\\\"; break;
        case '"':  out << "\\\""; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<int>(static_cast<unsigned char>(ch));
            } else {
                out << ch;
            }
        }
    }
    return out.str();
}

std::string quote(const std::string& value) {
    return std::string("\"") + json_escape(value) + "\"";
}

std::string number(double value) {
    std::ostringstream out;
    out << std::setprecision(17) << value;
    return out.str();
}

std::string iso8601_utc_now() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto sec = system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &sec);
#else
    gmtime_r(&sec, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

std::size_t latest_seq_in_log(const std::string& path) {
    std::ifstream in(path);
    std::size_t latest = 0;
    std::string line;
    while (std::getline(in, line)) {
        const std::string marker = "\"seq\":";
        const std::size_t pos = line.find(marker);
        if (pos == std::string::npos) continue;
        std::size_t i = pos + marker.size();
        while (i < line.size() && line[i] == ' ') ++i;
        std::size_t value = 0;
        bool have = false;
        while (i < line.size() && line[i] >= '0' && line[i] <= '9') {
            have = true;
            value = value * 10 + static_cast<std::size_t>(line[i] - '0');
            ++i;
        }
        if (have) latest = std::max(latest, value);
    }
    return latest;
}

bool file_has_content(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    return in && in.tellg() > 0;
}

void validate_existing_manifest(const std::string& path,
                                const std::string& step_path) {
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line) && line.find_first_not_of(" \t\r\n")
            == std::string::npos) {}
    if (line.empty())
        throw std::runtime_error("decision log has no manifest: " + path);
    std::shared_ptr<JsonValue> manifest;
    try {
        manifest = JsonParser(line).parse();
    } catch (const std::exception& error) {
        throw std::runtime_error("decision log manifest is invalid: "
                                 + std::string(error.what()));
    }
    if (!manifest || manifest->kind != JsonKind::object
        || !manifest->get("kind")
        || manifest->get("kind")->as_string() != "manifest")
        throw std::runtime_error("decision log does not start with a manifest");
    const JsonValue* schema = manifest->get("schema");
    const JsonValue* schema_version = manifest->get("schema_version");
    if (!schema || schema->as_string() != "csgify.decision-log"
        || !schema_version || schema_version->as_number() != 1.0)
        throw std::runtime_error("decision log schema is incompatible");
    const JsonValue* recorded_csgify = manifest->get("csgify_version");
    const JsonValue* recorded_occt = manifest->get("occt_version");
    if (!recorded_csgify
        || recorded_csgify->as_string() != csgify_version())
        throw std::runtime_error(
            "decision log was created by a different csgify version");
    if (!recorded_occt || recorded_occt->as_string() != occt_version())
        throw std::runtime_error(
            "decision log was created by a different OCCT version");
    if (!step_path.empty()) {
        const std::string raw = fnv1a128_file_hex(step_path);
        const std::string expected = raw.empty() ? std::string()
                                                  : "fnv1a128:" + raw;
        const JsonValue* recorded_digest = manifest->get("step_digest");
        if (expected.empty() || !recorded_digest
            || recorded_digest->as_string() != expected)
            throw std::runtime_error(
                "decision log belongs to different source contents");
    }
}

} // namespace

DecisionParam param_string(const std::string& name, const std::string& value) {
    return {name, quote(value)};
}

DecisionParam param_double(const std::string& name, double value) {
    return {name, number(value)};
}

DecisionParam param_bool(const std::string& name, bool value) {
    return {name, value ? "true" : "false"};
}

DecisionParam param_int(const std::string& name, long long value) {
    std::ostringstream out;
    out << value;
    return {name, out.str()};
}

DecisionLog::DecisionLog(const std::string& path, const std::string& step_path)
    : path_(path) {
    std::error_code filesystem_error;
    const bool exists = std::filesystem::exists(path, filesystem_error);
    if (filesystem_error) {
        throw std::runtime_error("could not inspect decision log: " + path
                                 + ": " + filesystem_error.message());
    }
    if (exists && !std::filesystem::is_regular_file(path, filesystem_error)) {
        throw std::runtime_error(
            "decision log path is not a regular file: " + path);
    }
    if (filesystem_error) {
        throw std::runtime_error("could not inspect decision log: " + path
                                 + ": " + filesystem_error.message());
    }
    const bool append_existing = file_has_content(path);
    if (append_existing) validate_existing_manifest(path, step_path);
    seq_ = latest_seq_in_log(path);
    out_.open(path, std::ios::out | std::ios::app);
    if (!out_.is_open()) {
        throw std::runtime_error("could not open decision log: " + path);
    }
    if (!append_existing) {
        write_manifest(step_path);
    }
}

DecisionLog::~DecisionLog() {
    if (out_.is_open()) {
        out_.close();
    }
}

void DecisionLog::append_line(const std::string& line,
                              const char* failure_message) {
    if (!out_.is_open())
        throw std::runtime_error(std::string(failure_message) + ": " + path_);
    out_.clear();
    out_.seekp(0, std::ios::end);
    const std::streampos boundary = out_.tellp();
    if (boundary < 0)
        throw std::runtime_error(std::string(failure_message) + ": " + path_);
    out_ << line;
    out_.flush();
    if (out_) return;

    // A short append is not a record. Restore the preceding complete-line
    // boundary before reporting failure so a later retry cannot inherit
    // malformed NDJSON or a sequence number that was never durable.
    out_.close();
    std::error_code rollback_error;
    std::filesystem::resize_file(
        path_, static_cast<std::uintmax_t>(boundary), rollback_error);
    out_.clear();
    out_.open(path_, std::ios::out | std::ios::app);
    std::string detail = std::string(failure_message) + ": " + path_;
    if (rollback_error)
        detail += "; partial-record rollback failed: "
            + rollback_error.message();
    if (!out_.is_open()) detail += "; decision log could not be reopened";
    throw std::runtime_error(detail);
}

void DecisionLog::write_manifest(const std::string& step_path) {
    std::string digest;
    if (!step_path.empty()) {
        const std::string raw = fnv1a128_file_hex(step_path);
        if (!raw.empty()) {
            digest = "fnv1a128:" + raw;
        }
    }
    std::ostringstream line;
    line << "{"
         << "\"kind\":\"manifest\","
         << "\"schema\":\"csgify.decision-log\","
         << "\"schema_version\":1,"
         << "\"seq\":0,"
         << "\"csgify_version\":" << quote(csgify_version()) << ","
         << "\"occt_version\":" << quote(occt_version()) << ","
         << "\"step_path\":" << quote(step_path) << ","
         << "\"step_digest\":" << quote(digest) << ","
         << "\"created_at\":" << quote(iso8601_utc_now())
         << "}\n";
    append_line(line.str(), "could not write decision log");
}

void DecisionLog::record(const DecisionTarget& target,
                         const std::string& op,
                         const std::vector<DecisionParam>& params,
                         const std::string& result,
                         const std::vector<std::string>& produced) {
    const std::size_t next_sequence = seq_ + 1;
    const OperationDescriptor* descriptor = find_operation(op);

    std::ostringstream line;
    line << "{"
         << "\"kind\":\"decision\","
         << "\"seq\":" << next_sequence << ","
         << "\"target\":{"
         <<   "\"kind\":" << quote(target.kind) << ","
         <<   "\"shape_id\":" << quote(target.shape_id) << ","
         <<   "\"component_entry\":" << quote(target.component_entry);
    if (target.fingerprint.valid) {
        line << ","
             << "\"fingerprint\":{"
             <<   "\"volume\":"        << number(target.fingerprint.volume) << ","
             <<   "\"surface_area\":"  << number(target.fingerprint.surface_area) << ","
             <<   "\"bbox_diagonal\":" << number(target.fingerprint.bbox_diagonal) << ","
             <<   "\"face_count\":"    << target.fingerprint.face_count << ","
             <<   "\"centroid\":["     << number(target.fingerprint.centroid_x) << ","
             <<                           number(target.fingerprint.centroid_y) << ","
             <<                           number(target.fingerprint.centroid_z) << "]"
             << "}";
    }
    line << "},"
         << "\"op\":" << quote(op) << ","
         << "\"operation_version\":"
         << (descriptor ? descriptor->version : 0) << ",";
    if (!audit_context_.actor.empty() || !audit_context_.proposal_actor.empty()
        || !audit_context_.proposal_id.empty()
        || !audit_context_.rationale.empty() || !audit_context_.policy_rule.empty()
        || !audit_context_.approval_mode.empty()) {
        line << "\"audit\":{"
             << "\"actor\":" << quote(audit_context_.actor) << ","
             << "\"proposal_actor\":" << quote(audit_context_.proposal_actor) << ","
             << "\"proposal_id\":" << quote(audit_context_.proposal_id) << ","
             << "\"rationale\":" << quote(audit_context_.rationale) << ","
             << "\"policy_rule\":" << quote(audit_context_.policy_rule) << ","
             << "\"approval_mode\":" << quote(audit_context_.approval_mode)
             << "},";
    }
    line << "\"params\":{";
    for (std::size_t i = 0; i < params.size(); ++i) {
        if (i) line << ",";
        line << quote(params[i].name) << ":" << params[i].value_json;
    }
    line << "},"
         << "\"result\":" << quote(result) << ","
         << "\"produced\":[";
    for (std::size_t i = 0; i < produced.size(); ++i) {
        if (i) line << ",";
        line << quote(produced[i]);
    }
    line << "],"
         << "\"issued_at\":" << quote(iso8601_utc_now())
         << "}\n";
    append_line(line.str(), "could not append decision log");
    seq_ = next_sequence;
}

void DecisionLog::record_undo(std::size_t undoes_seq) {
    const std::size_t next_sequence = seq_ + 1;
    std::ostringstream line;
    line << "{"
         << "\"kind\":\"undo\","
         << "\"seq\":" << next_sequence << ","
         << "\"undoes_seq\":" << undoes_seq << ","
         << "\"issued_at\":" << quote(iso8601_utc_now())
         << "}\n";
    append_line(line.str(), "could not append decision log");
    seq_ = next_sequence;
}

void DecisionLog::record_review(const std::string& target_id,
                                const std::string& proposal_id,
                                const std::string& decision,
                                const std::string& review_kind,
                                const std::string& action,
                                const std::string& candidate_id,
                                std::size_t base_revision) {
    const std::size_t next_sequence = seq_ + 1;
    std::ostringstream line;
    line << "{\"kind\":\"review\",\"seq\":" << next_sequence
         << ",\"target_id\":" << quote(target_id)
         << ",\"proposal_id\":" << quote(proposal_id)
         << ",\"decision\":" << quote(decision)
         << ",\"review_kind\":" << quote(review_kind)
         << ",\"action\":" << quote(action)
         << ",\"candidate_id\":" << quote(candidate_id)
         << ",\"base_revision\":" << base_revision
         << ",\"audit\":{\"actor\":" << quote(audit_context_.actor)
         << ",\"proposal_actor\":" << quote(audit_context_.proposal_actor)
         << ",\"proposal_id\":" << quote(audit_context_.proposal_id)
         << ",\"rationale\":" << quote(audit_context_.rationale)
         << ",\"policy_rule\":" << quote(audit_context_.policy_rule)
         << ",\"approval_mode\":" << quote(audit_context_.approval_mode)
         << "},\"issued_at\":" << quote(iso8601_utc_now()) << "}\n";
    append_line(line.str(), "could not append decision log");
    seq_ = next_sequence;
}

} // namespace csgify

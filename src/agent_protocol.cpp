#include "csgify/agent.hpp"

#include "fnv.hpp"
#include "json_parser.hpp"
#include "operation_registry.hpp"

#include <cmath>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace csgify {
namespace {

std::string escape_json(const std::string& value) {
    std::ostringstream out;
    for (const unsigned char ch : value) {
        switch (ch) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        default:
            if (ch < 0x20) {
                out << "\\u" << std::hex << std::setw(4)
                    << std::setfill('0') << static_cast<int>(ch) << std::dec;
            } else {
                out << static_cast<char>(ch);
            }
        }
    }
    return out.str();
}

void write_json_value(std::ostream& out, const JsonValue& value) {
    switch (value.kind) {
    case JsonKind::null: out << "null"; return;
    case JsonKind::boolean: out << (value.b ? "true" : "false"); return;
    case JsonKind::number:
        out << std::setprecision(17) << value.num;
        return;
    case JsonKind::string:
        out << '"' << escape_json(value.str) << '"';
        return;
    case JsonKind::array:
        out << '[';
        for (std::size_t i = 0; i < value.arr.size(); ++i) {
            if (i) out << ',';
            write_json_value(out, *value.arr[i]);
        }
        out << ']';
        return;
    case JsonKind::object:
        out << '{';
        {
            std::size_t index = 0;
            for (const auto& field : value.obj) {
                if (index++) out << ',';
                out << '"' << escape_json(field.first) << "\":";
                write_json_value(out, *field.second);
            }
        }
        out << '}';
        return;
    }
}

std::string serialize_json_value(const JsonValue& value) {
    std::ostringstream out;
    write_json_value(out, value);
    if (!out) throw std::runtime_error("failed to serialize agent response");
    return out.str();
}

void reject_unknown_fields(const JsonValue& object,
                           const std::set<std::string>& allowed,
                           const std::string& context) {
    for (const auto& field : object.obj) {
        if (allowed.find(field.first) == allowed.end())
            throw std::runtime_error(
                context + " has unknown field: " + field.first);
    }
}

const JsonValue& required_object_field(const JsonValue& object,
                                       const char* name,
                                       JsonKind kind,
                                       const char* context) {
    const JsonValue* value = object.get(name);
    if (!value || value->kind != kind)
        throw std::runtime_error(
            std::string(context) + "." + name + " has the wrong type");
    return *value;
}

std::string required_bounded_string(const JsonValue& object,
                                    const char* name,
                                    std::size_t max_size,
                                    const char* context) {
    const JsonValue& value = required_object_field(
        object, name, JsonKind::string, context);
    if (value.str.empty() || value.str.size() > max_size)
        throw std::runtime_error(
            std::string(context) + "." + name
            + " must be a non-empty bounded string");
    return value.str;
}

AgentResponseKind response_kind_from_text(const std::string& kind) {
    if (kind == "answer") return AgentResponseKind::answer;
    if (kind == "proposal") return AgentResponseKind::proposal;
    if (kind == "question") return AgentResponseKind::question;
    if (kind == "stop") return AgentResponseKind::stop;
    throw std::runtime_error(
        "agent response.kind must be answer, proposal, question, or stop");
}

bool reference_kind_supported(const std::string& kind) {
    return kind == "document" || kind == "target"
        || kind == "diagnostic" || kind == "candidate"
        || kind == "operation" || kind == "material"
        || kind == "interface" || kind == "occurrence"
        || kind == "history";
}

bool contains_string_value(const JsonValue& value,
                           const std::string& expected) {
    if (value.kind == JsonKind::string) return value.str == expected;
    if (value.kind == JsonKind::array) {
        for (const auto& item : value.arr)
            if (item && contains_string_value(*item, expected)) return true;
    } else if (value.kind == JsonKind::object) {
        for (const auto& field : value.obj)
            if (field.second
                && contains_string_value(*field.second, expected)) return true;
    }
    return false;
}

AgentValidationResult valid() {
    return {true, "ok", {}};
}

AgentValidationResult invalid(std::string code, std::string message) {
    return {false, std::move(code), std::move(message)};
}

std::size_t non_negative_size(const JsonValue& object,
                              const char* name,
                              const char* context) {
    const JsonValue& value = required_object_field(
        object, name, JsonKind::number, context);
    if (!std::isfinite(value.num) || value.num < 0.0
        || std::floor(value.num) != value.num
        || value.num >= static_cast<double>(
            std::numeric_limits<std::size_t>::max()))
        throw std::runtime_error(
            std::string(context) + "." + name
            + " must be a non-negative integer");
    return static_cast<std::size_t>(value.num);
}

} // namespace

const char* agent_response_kind_name(AgentResponseKind kind) {
    switch (kind) {
    case AgentResponseKind::answer: return "answer";
    case AgentResponseKind::proposal: return "proposal";
    case AgentResponseKind::question: return "question";
    case AgentResponseKind::stop: return "stop";
    }
    return "answer";
}

AgentContextSnapshot make_agent_context_snapshot(
    Session& session,
    const DocumentId& document_id,
    const AutonomyPolicy& policy,
    const AutonomyContextOptions& options,
    std::string conversion_policy_digest) {
    AgentContextSnapshot snapshot;
    snapshot.context_json = build_autonomy_context_json(
        session, document_id, policy, options);
    const std::shared_ptr<JsonValue> root =
        JsonParser(snapshot.context_json).parse();
    if (!root || root->kind != JsonKind::object)
        throw std::runtime_error("agent context is not a JSON object");
    const JsonValue& document = required_object_field(
        *root, "document", JsonKind::object, "agent context");
    snapshot.document_id = required_bounded_string(
        document, "id", 256, "agent context.document");
    snapshot.source_digest = required_bounded_string(
        document, "source_digest", 128, "agent context.document");
    snapshot.document_revision = non_negative_size(
        document, "revision", "agent context.document");
    const JsonValue& selection = required_object_field(
        *root, "selection", JsonKind::string, "agent context");
    snapshot.target_id = selection.str;
    snapshot.policy_digest = autonomy_policy_digest(policy);
    snapshot.conversion_policy_digest =
        std::move(conversion_policy_digest);
    snapshot.mode = policy.mode;
    snapshot.context_digest = fnv1a128_hex(fnv1a128(
        snapshot.context_json.data(), snapshot.context_json.size()));
    snapshot.context_id = "context:" + snapshot.context_digest;
    return snapshot;
}

AgentResponse parse_agent_response_json(const std::string& json,
                                        std::size_t max_serialized_bytes) {
    if (json.empty() || json.size() > max_serialized_bytes)
        throw std::runtime_error("agent response exceeds its size boundary");
    const std::shared_ptr<JsonValue> root = JsonParser(json).parse();
    if (!root || root->kind != JsonKind::object)
        throw std::runtime_error("agent response is not a JSON object");
    reject_unknown_fields(
        *root,
        {"schema", "schema_version", "context_id", "kind", "message",
         "references", "actions"},
        "agent response");
    if (required_bounded_string(
            *root, "schema", 64, "agent response")
        != "csgify.agent-response")
        throw std::runtime_error("unsupported agent response schema");
    const JsonValue& version = required_object_field(
        *root, "schema_version", JsonKind::number, "agent response");
    if (version.num != 1.0)
        throw std::runtime_error("unsupported agent response schema_version");

    AgentResponse response;
    response.context_id = required_bounded_string(
        *root, "context_id", 256, "agent response");
    response.kind = response_kind_from_text(required_bounded_string(
        *root, "kind", 32, "agent response"));
    response.message = required_bounded_string(
        *root, "message", 16 * 1024, "agent response");

    if (const JsonValue* references = root->get("references")) {
        if (references->kind != JsonKind::array)
            throw std::runtime_error("agent response.references must be an array");
        if (references->arr.size() > 128)
            throw std::runtime_error("agent response exceeds 128 references");
        for (const auto& raw : references->arr) {
            if (!raw || raw->kind != JsonKind::object)
                throw std::runtime_error("agent reference must be an object");
            reject_unknown_fields(*raw, {"kind", "id", "label"},
                                  "agent reference");
            AgentEvidenceReference reference;
            reference.kind = required_bounded_string(
                *raw, "kind", 32, "agent reference");
            reference.id = required_bounded_string(
                *raw, "id", 512, "agent reference");
            if (!reference_kind_supported(reference.kind))
                throw std::runtime_error("unsupported agent reference kind");
            if (const JsonValue* label = raw->get("label")) {
                if (label->kind != JsonKind::string
                    || label->str.size() > 512)
                    throw std::runtime_error(
                        "agent reference.label must be a bounded string");
                reference.label = label->str;
            }
            response.references.push_back(std::move(reference));
        }
    }

    const JsonValue* actions = root->get("actions");
    if (response.kind == AgentResponseKind::proposal) {
        if (!actions || actions->kind != JsonKind::array
            || actions->arr.size() != 1)
            throw std::runtime_error(
                "agent proposal must contain exactly one action");
        std::ostringstream request_json;
        request_json
            << "{\"schema\":\"csgify.autonomy-actions\","
            << "\"schema_version\":2,\"request_id\":\"agent-parse\","
            << "\"source_digest\":\"unbound\",\"expected_revision\":0,"
            << "\"actor\":\"llm:embedded\",\"actions\":"
            << serialize_json_value(*actions) << '}';
        response.actions = parse_autonomy_request_json(
            request_json.str()).actions;
        const OperationDescriptor* operation =
            find_operation(response.actions.front().op);
        if (!operation || !operation->autonomy_supported)
            throw std::runtime_error(
                "agent proposal uses an unsupported operation");
    } else if (actions && (actions->kind != JsonKind::array
                           || !actions->arr.empty())) {
        throw std::runtime_error(
            "non-proposal agent response cannot contain actions");
    }
    return response;
}

AgentValidationResult validate_agent_context_snapshot(
    const Session& session,
    const DocumentId& document_id,
    const AutonomyPolicy& policy,
    const AgentContextSnapshot& snapshot,
    const std::string& target_id) {
    if (snapshot.document_id != document_id)
        return invalid("document_changed",
                       "The active document no longer matches this turn.");
    if (document_revision(session, document_id) != snapshot.document_revision)
        return invalid("document_revision_changed",
                       "The geometry changed after this context was captured.");
    if (autonomy_policy_digest(policy) != snapshot.policy_digest)
        return invalid("policy_changed",
                       "The authority policy changed after this context was captured.");
    if (target_id != snapshot.target_id)
        return invalid("selection_changed",
                       "The active selection changed after this context was captured.");
    return valid();
}

AgentValidationResult validate_agent_response(
    const AgentResponse& response,
    const AgentContextSnapshot& snapshot) {
    if (response.context_id != snapshot.context_id)
        return invalid("context_mismatch",
                       "The response was produced for a different context.");
    if (response.kind == AgentResponseKind::proposal
        && snapshot.mode == AutonomyMode::explain)
        return invalid("proposal_not_allowed",
                       "The current authority mode allows answers but not proposals.");
    if (response.kind == AgentResponseKind::proposal
        && response.actions.size() != 1)
        return invalid("invalid_action_count",
                       "A proposal must contain exactly one action.");
    if (response.kind != AgentResponseKind::proposal
        && !response.actions.empty())
        return invalid("unexpected_actions",
                       "Only proposal responses may contain actions.");
    if (response.kind == AgentResponseKind::proposal
        && !snapshot.target_id.empty()) {
        const AutonomyProposal& action = response.actions.front();
        const OperationDescriptor* operation = find_operation(action.op);
        if (!operation || operation->target == OperationTargetKind::document
            || action.target_id != snapshot.target_id)
            return invalid(
                "proposal_out_of_scope",
                "The proposal is outside the target pinned to this turn.");
    }

    std::shared_ptr<JsonValue> context;
    try {
        context = JsonParser(snapshot.context_json).parse();
    } catch (const std::exception& error) {
        return invalid("invalid_context", error.what());
    }
    for (const AgentEvidenceReference& reference : response.references) {
        if (!context || !contains_string_value(*context, reference.id))
            return invalid(
                "unknown_reference",
                "The response cites evidence that is absent from its context: "
                    + reference.id);
    }
    return valid();
}

AutonomyRequest bind_agent_proposal(
    const AgentResponse& response,
    const AgentContextSnapshot& snapshot,
    const std::string& request_id,
    const std::string& actor) {
    const AgentValidationResult validation =
        validate_agent_response(response, snapshot);
    if (!validation.ok)
        throw std::runtime_error(validation.code + ": " + validation.message);
    if (response.kind != AgentResponseKind::proposal)
        throw std::runtime_error("agent response is not a proposal");
    if (request_id.empty() || request_id.size() > 256)
        throw std::runtime_error(
            "agent request_id must contain at most 256 bytes");
    AutonomyRequest request;
    request.request_id = request_id;
    request.source_digest = snapshot.source_digest;
    request.expected_revision = snapshot.document_revision;
    request.actor = actor == "llm" || actor.rfind("llm:", 0) == 0
        ? actor : "llm:embedded";
    request.actions = response.actions;
    for (AutonomyProposal& action : request.actions)
        action.actor = request.actor;
    return request;
}

ScriptedAgentProvider::ScriptedAgentProvider(
    std::vector<std::string> responses)
    : responses_(std::move(responses)) {}

AgentResponse ScriptedAgentProvider::respond(
    const AgentProviderRequest& request) {
    if (request.context.context_id.empty())
        throw std::runtime_error("scripted provider requires a context");
    if (request.messages.empty()
        || request.messages.back().role != AgentMessageRole::user
        || request.messages.back().text.empty())
        throw std::runtime_error(
            "scripted provider requires a non-empty user message");
    if (next_ >= responses_.size())
        throw std::runtime_error("scripted provider response queue is exhausted");
    AgentResponse response = parse_agent_response_json(responses_[next_++]);
    const AgentValidationResult validation =
        validate_agent_response(response, request.context);
    if (!validation.ok)
        throw std::runtime_error(validation.code + ": " + validation.message);
    return response;
}

std::size_t ScriptedAgentProvider::remaining() const {
    return responses_.size() - next_;
}

} // namespace csgify

#pragma once

#include "csgify/autonomy.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace csgify {

enum class AgentResponseKind { answer, proposal, question, stop };
enum class AgentMessageRole { user, assistant };

struct AgentEvidenceReference {
    std::string kind;
    std::string id;
    std::string label;
};

struct AgentMessage {
    AgentMessageRole role = AgentMessageRole::user;
    std::string text;
};

// Immutable evidence envelope captured for one conversational turn. The
// context JSON is kept verbatim so providers never need direct Session access.
struct AgentContextSnapshot {
    std::string context_id;
    std::string context_digest;
    std::string context_json;
    std::string document_id;
    std::string source_digest;
    std::size_t document_revision = 0;
    std::string policy_digest;
    std::string conversion_policy_digest;
    std::string target_id;
    AutonomyMode mode = AutonomyMode::propose;
};

struct AgentResponse {
    AgentResponseKind kind = AgentResponseKind::answer;
    std::string context_id;
    std::string message;
    std::vector<AgentEvidenceReference> references;
    std::vector<AutonomyProposal> actions;
};

struct AgentValidationResult {
    bool ok = false;
    std::string code;
    std::string message;
};

struct AgentProviderRequest {
    AgentContextSnapshot context;
    std::vector<AgentMessage> messages;
};

class AgentProvider {
public:
    virtual ~AgentProvider() = default;
    virtual AgentResponse respond(const AgentProviderRequest& request) = 0;
};

// Deterministic provider used by protocol/controller tests. Each call parses
// the next response through the same strict boundary as a live provider.
class ScriptedAgentProvider final : public AgentProvider {
public:
    explicit ScriptedAgentProvider(std::vector<std::string> responses);
    AgentResponse respond(const AgentProviderRequest& request) override;
    std::size_t remaining() const;

private:
    std::vector<std::string> responses_;
    std::size_t next_ = 0;
};

AgentContextSnapshot make_agent_context_snapshot(
    Session& session,
    const DocumentId& document_id,
    const AutonomyPolicy& policy,
    const AutonomyContextOptions& options,
    std::string conversion_policy_digest = {});

AgentResponse parse_agent_response_json(
    const std::string& json,
    std::size_t max_serialized_bytes = 256 * 1024);

AgentValidationResult validate_agent_context_snapshot(
    const Session& session,
    const DocumentId& document_id,
    const AutonomyPolicy& policy,
    const AgentContextSnapshot& snapshot,
    const std::string& target_id);

AgentValidationResult validate_agent_response(
    const AgentResponse& response,
    const AgentContextSnapshot& snapshot);

// Converts a validated single-action proposal into the existing autonomy
// request contract. This does not execute or approve the proposal.
AutonomyRequest bind_agent_proposal(
    const AgentResponse& response,
    const AgentContextSnapshot& snapshot,
    const std::string& request_id,
    const std::string& actor = "llm:embedded");

const char* agent_response_kind_name(AgentResponseKind kind);

} // namespace csgify

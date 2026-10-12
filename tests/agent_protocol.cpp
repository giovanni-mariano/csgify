#include "csgify/agent.hpp"
#include "csgify/session.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int fail(const std::string& message) {
    std::cerr << "agent_protocol: " << message << '\n';
    return EXIT_FAILURE;
}

std::string response_json(const std::string& context_id,
                          const std::string& kind,
                          const std::string& message,
                          const std::string& tail = {}) {
    return "{\"schema\":\"csgify.agent-response\","
        "\"schema_version\":1,\"context_id\":\"" + context_id
        + "\",\"kind\":\"" + kind + "\",\"message\":\""
        + message + "\"" + tail + "}";
}

bool rejects(const std::string& json) {
    try {
        (void)csgify::parse_agent_response_json(json);
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return fail("expected one STEP fixture path");

    csgify::SessionPtr session = csgify::create_session();
    const csgify::DocumentId document = csgify::load_step(*session, argv[1]);
    const std::string target = document + "/solid:1";
    csgify::AutonomyPolicy policy;
    policy.mode = csgify::AutonomyMode::propose;
    csgify::AutonomyContextOptions options;
    options.project_id = "agent-protocol-test";
    options.intended_analysis = "geometry review";
    options.target_id = target;

    const std::size_t revision_before =
        csgify::document_revision(*session, document);
    const csgify::AgentContextSnapshot snapshot =
        csgify::make_agent_context_snapshot(
            *session, document, policy, options, "conversion:test");
    if (snapshot.context_json.empty() || snapshot.context_id.empty()
        || snapshot.context_digest.empty()
        || snapshot.document_id != document
        || snapshot.document_revision != revision_before
        || snapshot.target_id != target
        || snapshot.conversion_policy_digest != "conversion:test")
        return fail("context snapshot did not preserve its evidence envelope");
    if (csgify::document_revision(*session, document) != revision_before)
        return fail("capturing context mutated the document");

    const std::string question = response_json(
        snapshot.context_id, "question",
        "Should I optimize for exact geometry or a bounded approximation?",
        ",\"references\":[{\"kind\":\"target\",\"id\":\""
            + target + "\",\"label\":\"Selected solid\"}]");
    const std::string answer = response_json(
        snapshot.context_id, "answer",
        "The selected solid requires decomposition.",
        ",\"references\":[{\"kind\":\"document\",\"id\":\""
            + document + "\"}]");
    csgify::ScriptedAgentProvider provider({question, answer});
    csgify::AgentProviderRequest request;
    request.context = snapshot;
    request.messages.push_back(
        {csgify::AgentMessageRole::user, "What should I do with this solid?"});
    const csgify::AgentResponse clarification = provider.respond(request);
    if (clarification.kind != csgify::AgentResponseKind::question
        || clarification.references.size() != 1 || provider.remaining() != 1)
        return fail("scripted provider did not return the follow-up question");
    request.messages.push_back(
        {csgify::AgentMessageRole::assistant, clarification.message});
    request.messages.push_back(
        {csgify::AgentMessageRole::user, "Keep the result exact."});
    const csgify::AgentResponse answered = provider.respond(request);
    if (answered.kind != csgify::AgentResponseKind::answer
        || answered.message.empty() || provider.remaining() != 0)
        return fail("scripted provider did not return the answer");

    const std::string proposal_json = response_json(
        snapshot.context_id, "proposal", "Remove the selected solid.",
        ",\"references\":[{\"kind\":\"target\",\"id\":\""
            + target
            + "\"}],\"actions\":[{\"id\":\"proposal:remove-1\","
              "\"op\":\"remove_solid\",\"target\":\""
            + target + "\",\"rationale\":\"Requested cleanup\"}]");
    const csgify::AgentResponse proposal =
        csgify::parse_agent_response_json(proposal_json);
    const csgify::AgentValidationResult proposal_validation =
        csgify::validate_agent_response(proposal, snapshot);
    if (!proposal_validation.ok)
        return fail("valid assisted proposal was rejected: "
                    + proposal_validation.message);
    const csgify::AutonomyRequest bound = csgify::bind_agent_proposal(
        proposal, snapshot, "agent-request:1", "llm:test-provider");
    if (bound.actions.size() != 1
        || bound.source_digest != snapshot.source_digest
        || bound.expected_revision != snapshot.document_revision
        || bound.actor != "llm:test-provider"
        || bound.actions[0].actor != bound.actor)
        return fail("proposal was not bound to authoritative app identity");

    csgify::AgentContextSnapshot advisory = snapshot;
    advisory.mode = csgify::AutonomyMode::explain;
    const csgify::AgentValidationResult advisory_result =
        csgify::validate_agent_response(proposal, advisory);
    if (advisory_result.ok || advisory_result.code != "proposal_not_allowed")
        return fail("advisory mode accepted a mutating proposal");

    csgify::AgentResponse wrong_context = answered;
    wrong_context.context_id = "context:stale";
    if (csgify::validate_agent_response(wrong_context, snapshot).ok)
        return fail("mismatched response context was accepted");

    const csgify::AgentResponse out_of_scope =
        csgify::parse_agent_response_json(response_json(
            snapshot.context_id, "proposal", "Remove a different solid.",
            ",\"actions\":[{\"id\":\"wrong-target\","
            "\"op\":\"remove_solid\",\"target\":\"solid:999\"}]"));
    if (csgify::validate_agent_response(
            out_of_scope, snapshot).code != "proposal_out_of_scope")
        return fail("out-of-scope proposal was accepted");

    const csgify::AgentResponse unknown_reference =
        csgify::parse_agent_response_json(response_json(
            snapshot.context_id, "answer", "Unsupported citation.",
            ",\"references\":[{\"kind\":\"target\","
            "\"id\":\"solid:does-not-exist\"}]"));
    if (csgify::validate_agent_response(
            unknown_reference, snapshot).code != "unknown_reference")
        return fail("invented evidence reference was accepted");

    if (!rejects(response_json(
            snapshot.context_id, "proposal", "Invented operation.",
            ",\"actions\":[{\"id\":\"bad\",\"op\":\"teleport_solid\","
            "\"target\":\"solid:1\"}]")))
        return fail("unsupported operation was accepted");
    if (!rejects(response_json(
            snapshot.context_id, "answer", "Unknown field.",
            ",\"unexpected\":true")))
        return fail("unknown response field was accepted");

    const csgify::CommandResult removed =
        csgify::remove_solid(*session, target);
    if (!removed.ok) return fail("fixture could not be mutated for stale test");
    const csgify::AgentValidationResult stale =
        csgify::validate_agent_context_snapshot(
            *session, document, policy, snapshot, target);
    if (stale.ok || stale.code != "document_revision_changed")
        return fail("stale geometry context was accepted");

    return EXIT_SUCCESS;
}

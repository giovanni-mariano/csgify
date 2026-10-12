#include "qt/openai_agent_provider.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cassert>
#include <stdexcept>

int main() {
    csgify::AgentProviderRequest request;
    request.context.context_id = "context:test";
    request.context.context_digest = "digest:test";
    request.context.context_json = "{\"document\":\"untrusted\"}";
    request.context.document_id = "doc:1";
    request.messages.push_back(
        {csgify::AgentMessageRole::user, "Why is export blocked?"});
    const QByteArray body = csgify::OpenAiAgentProvider::buildRequestBody(
        request, QStringLiteral("test-model"));
    const QJsonDocument request_document = QJsonDocument::fromJson(body);
    assert(request_document.isObject());
    const QJsonObject root = request_document.object();
    assert(root.value(QStringLiteral("model")).toString()
           == QStringLiteral("test-model"));
    assert(!root.value(QStringLiteral("store")).toBool(true));
    assert(root.value(QStringLiteral("input")).toArray().size() == 2);
    const QJsonObject format = root.value(QStringLiteral("text")).toObject()
        .value(QStringLiteral("format")).toObject();
    assert(format.value(QStringLiteral("type")).toString()
           == QStringLiteral("json_schema"));
    assert(format.value(QStringLiteral("strict")).toBool());

    const QString structured = QStringLiteral(
        "{\"schema\":\"csgify.agent-response\",\"schema_version\":1,"
        "\"context_id\":\"context:test\",\"kind\":\"answer\","
        "\"message\":\"A diagnostic blocks export.\","
        "\"references\":[],\"actions\":[]}");
    const QJsonObject envelope{
        {QStringLiteral("status"), QStringLiteral("completed")},
        {QStringLiteral("output"), QJsonArray{QJsonObject{
            {QStringLiteral("type"), QStringLiteral("message")},
            {QStringLiteral("content"), QJsonArray{QJsonObject{
                {QStringLiteral("type"), QStringLiteral("output_text")},
                {QStringLiteral("text"), structured}}}}}}}};
    const csgify::AgentResponse decoded =
        csgify::OpenAiAgentProvider::decodeResponse(
            QJsonDocument(envelope).toJson(QJsonDocument::Compact),
            request.context);
    assert(decoded.kind == csgify::AgentResponseKind::answer);
    assert(decoded.message == "A diagnostic blocks export.");

    bool rejected = false;
    try {
        QJsonObject wrong = envelope;
        wrong.insert(QStringLiteral("status"), QStringLiteral("incomplete"));
        (void)csgify::OpenAiAgentProvider::decodeResponse(
            QJsonDocument(wrong).toJson(QJsonDocument::Compact),
            request.context);
    } catch (const std::exception&) {
        rejected = true;
    }
    assert(rejected);
    return 0;
}

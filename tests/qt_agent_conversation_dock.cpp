#include "qt/agent_conversation_dock.hpp"

#include <QApplication>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextBrowser>

#include <cassert>

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    csgify::AgentConversationDock dock;
    dock.setAvailable(true);
    dock.setContext(QStringLiteral("doc:1/solid:2"), 7);

    auto* composer = dock.findChild<QPlainTextEdit*>(
        QStringLiteral("agentQuestionComposer"));
    auto* ask = dock.findChild<QPushButton*>(
        QStringLiteral("askAgentButton"));
    auto* stop = dock.findChild<QPushButton*>(
        QStringLiteral("stopAgentButton"));
    auto* transcript = dock.findChild<QTextBrowser*>(
        QStringLiteral("agentTranscript"));
    assert(composer && ask && stop && transcript);
    assert(ask->isEnabled());
    assert(!stop->isEnabled());

    dock.setProvider(QStringLiteral("Evidence assistant · OpenAI · advisory"),
                     false);
    auto* provider = dock.findChild<QLabel*>(
        QStringLiteral("agentProviderLabel"));
    assert(provider);
    assert(provider->text().contains(QStringLiteral("OpenAI")));
    assert(transcript->toPlainText().contains(QStringLiteral("advisory mode")));

    QString submitted;
    QObject::connect(&dock, &csgify::AgentConversationDock::askRequested,
                     [&submitted](const QString& value) {
                         submitted = value;
                     });
    composer->setPlainText(QStringLiteral("Why is export blocked?"));
    ask->click();
    assert(submitted == QStringLiteral("Why is export blocked?"));
    assert(!ask->isEnabled());
    assert(stop->isEnabled());
    assert(transcript->toPlainText().contains(
        QStringLiteral("Why is export blocked?")));

    csgify::AgentResponse response;
    response.kind = csgify::AgentResponseKind::answer;
    response.message = "A blocking diagnostic remains.";
    response.references.push_back(
        {"document", "doc:1", "Engine context"});
    dock.appendResponse(response);
    assert(ask->isEnabled());
    assert(!stop->isEnabled());
    assert(transcript->toPlainText().contains(
        QStringLiteral("A blocking diagnostic remains.")));
    assert(transcript->toPlainText().contains(QStringLiteral("doc:1")));
    return 0;
}

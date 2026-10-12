#include "agent_conversation_dock.hpp"

#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShortcut>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <QWidget>

namespace csgify {

AgentConversationDock::AgentConversationDock(QWidget* parent)
    : QDockWidget(QStringLiteral("Project conversation"), parent) {
    setObjectName(QStringLiteral("agentConversationDock"));
    setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea
                    | Qt::BottomDockWidgetArea);
    setMinimumWidth(330);

    auto* body = new QWidget(this);
    body->setObjectName(QStringLiteral("agentConversationBody"));
    body->setStyleSheet(QStringLiteral(
        "QWidget#agentConversationBody { background:#ffffff; color:#203442; }"
        "QLabel { color:#203442; }"
        "QLabel[role='secondary'] { color:#526775; }"
        "QLabel[role='heading'] { font-weight:600; font-size:13px; }"
        "QTextBrowser,QPlainTextEdit { background:#ffffff; color:#203442;"
        " border:1px solid #d6dfe5; border-radius:4px; padding:6px; }"
        "QPlainTextEdit:focus { border:2px solid #126b80; }"
        "QPushButton { background:#f5f8fa; color:#203442;"
        " border:1px solid #bccbd4; border-radius:4px; padding:6px 9px; }"
        "QPushButton[role='primary'] { background:#126b80; color:#ffffff;"
        " border-color:#126b80; }"
        "QPushButton:disabled { color:#73838d; background:#edf2f5;"
        " border-color:#d6dfe5; }"));
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    provider_ = new QLabel(
        QStringLiteral("Evidence assistant · simulated provider"), body);
    provider_->setObjectName(QStringLiteral("agentProviderLabel"));
    provider_->setProperty("role", "heading");
    layout->addWidget(provider_);

    context_ = new QLabel(
        QStringLiteral("Load a model to attach project evidence."), body);
    context_->setObjectName(QStringLiteral("agentContextLabel"));
    context_->setProperty("role", "secondary");
    context_->setWordWrap(true);
    layout->addWidget(context_);

    transcript_ = new QTextBrowser(body);
    transcript_->setObjectName(QStringLiteral("agentTranscript"));
    transcript_->setOpenExternalLinks(false);
    transcript_->setAccessibleName(QStringLiteral("Project conversation"));
    resetTranscript();
    layout->addWidget(transcript_, 1);

    composer_ = new QPlainTextEdit(body);
    composer_->setObjectName(QStringLiteral("agentQuestionComposer"));
    composer_->setPlaceholderText(
        QStringLiteral("Ask about this project…"));
    composer_->setMaximumHeight(92);
    composer_->setAccessibleName(QStringLiteral("Question for project assistant"));
    layout->addWidget(composer_);

    status_ = new QLabel(QStringLiteral("Model not loaded"), body);
    status_->setObjectName(QStringLiteral("agentStatusLabel"));
    status_->setProperty("role", "secondary");
    status_->setWordWrap(true);
    layout->addWidget(status_);

    auto* actions = new QHBoxLayout;
    ask_ = new QPushButton(QStringLiteral("Ask"), body);
    ask_->setObjectName(QStringLiteral("askAgentButton"));
    ask_->setProperty("role", "primary");
    stop_ = new QPushButton(QStringLiteral("Stop response"), body);
    stop_->setObjectName(QStringLiteral("stopAgentButton"));
    actions->addStretch();
    actions->addWidget(stop_);
    actions->addWidget(ask_);
    layout->addLayout(actions);

    connect(ask_, &QPushButton::clicked,
            this, &AgentConversationDock::submitQuestion);
    connect(stop_, &QPushButton::clicked,
            this, &AgentConversationDock::stopRequested);
    auto* submit = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Return")),
                                 composer_);
    connect(submit, &QShortcut::activated,
            this, &AgentConversationDock::submitQuestion);

    setWidget(body);
    setAvailable(false);
}

void AgentConversationDock::setContext(const QString& target,
                                       std::size_t revision) {
    context_->setText(QStringLiteral("Attached evidence · %1 · revision %2")
                          .arg(target.isEmpty()
                              ? QStringLiteral("whole document") : target)
                          .arg(revision));
}

void AgentConversationDock::setProvider(const QString& label,
                                        bool simulated) {
    simulated_provider_ = simulated;
    provider_->setText(label);
    resetTranscript();
}

void AgentConversationDock::setAvailable(bool available) {
    available_ = available;
    composer_->setEnabled(available && !busy_);
    ask_->setEnabled(available && !busy_);
    stop_->setEnabled(busy_);
    if (!busy_) status_->setText(available
        ? QStringLiteral("Ready · Ctrl+Enter to ask")
        : QStringLiteral("Model not loaded"));
}

void AgentConversationDock::setBusy(bool busy, const QString& status) {
    busy_ = busy;
    composer_->setEnabled(available_ && !busy);
    ask_->setEnabled(available_ && !busy);
    stop_->setEnabled(busy);
    status_->setText(status.isEmpty()
        ? busy ? QStringLiteral("Gathering project evidence…")
               : available_ ? QStringLiteral("Ready · Ctrl+Enter to ask")
                            : QStringLiteral("Model not loaded")
        : status);
}

void AgentConversationDock::resetTranscript() {
    transcript_->setHtml(simulated_provider_
        ? QStringLiteral(
            "<p><b>Assistant</b><br>Ask about the loaded project. "
            "Answers use the built-in simulated provider and are marked "
            "accordingly.</p>")
        : QStringLiteral(
            "<p><b>Assistant</b><br>Ask about the loaded project. "
            "Answers use attached engine evidence in advisory mode; the "
            "assistant cannot change geometry.</p>"));
}

void AgentConversationDock::appendUserMessage(const QString& message) {
    transcript_->append(QStringLiteral("<p><b>You</b><br>%1</p>")
                            .arg(message.toHtmlEscaped()));
}

void AgentConversationDock::appendResponse(const AgentResponse& response,
                                           bool simulated) {
    const QString kind = response.kind == AgentResponseKind::question
        ? QStringLiteral("Follow-up question")
        : response.kind == AgentResponseKind::proposal
            ? QStringLiteral("Proposal") : QStringLiteral("Assistant");
    QString references;
    for (const AgentEvidenceReference& reference : response.references) {
        references += QStringLiteral("<li><code>%1</code></li>")
            .arg(QString::fromStdString(reference.id).toHtmlEscaped());
    }
    transcript_->append(QStringLiteral(
        "<p><b>%1</b>%2<br>%3%4</p>")
        .arg(kind,
             simulated
                ? QStringLiteral(" <span style='color:#526775'>(simulated)</span>")
                : QString(),
             QString::fromStdString(response.message).toHtmlEscaped(),
             references.isEmpty()
                ? QString()
                : QStringLiteral("<br><span style='color:#526775'>Evidence</span>"
                                 "<ul>%1</ul>").arg(references)));
    setBusy(false);
}

void AgentConversationDock::appendError(const QString& message) {
    transcript_->append(QStringLiteral(
        "<p><b>Assistant unavailable</b><br>%1</p>")
        .arg(message.toHtmlEscaped()));
    setBusy(false, QStringLiteral("Could not answer · project unchanged"));
}

void AgentConversationDock::appendStopped() {
    transcript_->append(QStringLiteral(
        "<p><b>Response stopped</b><br>The project was not changed.</p>"));
    setBusy(false, QStringLiteral("Stopped · ready for another question"));
}

void AgentConversationDock::submitQuestion() {
    const QString question = composer_->toPlainText().trimmed();
    if (!available_ || busy_ || question.isEmpty()) return;
    appendUserMessage(question);
    composer_->clear();
    setBusy(true);
    emit askRequested(question);
}

} // namespace csgify

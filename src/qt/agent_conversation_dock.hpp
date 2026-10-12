#pragma once

#include "csgify/agent.hpp"

#include <QDockWidget>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTextBrowser;

namespace csgify {

class AgentConversationDock final : public QDockWidget {
    Q_OBJECT
public:
    explicit AgentConversationDock(QWidget* parent = nullptr);

    void setContext(const QString& target, std::size_t revision);
    void setProvider(const QString& label, bool simulated);
    void setAvailable(bool available);
    void setBusy(bool busy, const QString& status = {});
    void resetTranscript();
    void appendUserMessage(const QString& message);
    void appendResponse(const AgentResponse& response, bool simulated = true);
    void appendError(const QString& message);
    void appendStopped();

signals:
    void askRequested(QString question);
    void stopRequested();

private:
    void submitQuestion();

    QLabel* provider_ = nullptr;
    QLabel* context_ = nullptr;
    QLabel* status_ = nullptr;
    QTextBrowser* transcript_ = nullptr;
    QPlainTextEdit* composer_ = nullptr;
    QPushButton* ask_ = nullptr;
    QPushButton* stop_ = nullptr;
    bool available_ = false;
    bool busy_ = false;
    bool simulated_provider_ = true;
};

} // namespace csgify

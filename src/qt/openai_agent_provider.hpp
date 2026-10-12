#pragma once

#include "csgify/agent.hpp"

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>

#include <memory>

class QNetworkAccessManager;
class QNetworkReply;

namespace csgify {

using AgentResponsePtr = std::shared_ptr<AgentResponse>;

class OpenAiAgentProvider final : public QObject {
    Q_OBJECT
public:
    explicit OpenAiAgentProvider(QString api_key,
                                 QString model,
                                 QObject* parent = nullptr);

    bool active() const { return active_; }
    const QString& model() const { return model_; }
    bool configured() const;
    bool start(const AgentProviderRequest& request);

    static QByteArray buildRequestBody(const AgentProviderRequest& request,
                                       const QString& model);
    static AgentResponse decodeResponse(
        const QByteArray& response_body,
        const AgentContextSnapshot& expected_context);

public slots:
    void cancel();

signals:
    void completed(csgify::AgentResponsePtr response);
    void failed(QString code, QString message);
    void activeChanged(bool active);

private:
    void finish();
    void setActive(bool active);

    QString api_key_;
    QString model_;
    QNetworkAccessManager* network_ = nullptr;
    QPointer<QNetworkReply> reply_;
    QByteArray response_body_;
    AgentContextSnapshot expected_context_;
    bool active_ = false;
    bool cancelled_ = false;
    bool oversized_ = false;
};

} // namespace csgify

Q_DECLARE_METATYPE(csgify::AgentResponsePtr)

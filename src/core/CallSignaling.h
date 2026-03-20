#pragma once

#include <QObject>
#include <QTimer>
#include <QJsonObject>
#include <QJsonArray>
#include "core/ApiClient.h"

/**
 * Internal signaling for WebRTC calls (HTTP polling).
 * Used when no standalone signaling (HPB) MCU is available.
 * Messages are exchanged via:
 *   POST /ocs/v2.php/apps/spreed/api/v3/signaling/{token}  (send)
 *   GET  /ocs/v2.php/apps/spreed/api/v3/signaling/{token}  (poll)
 */
class CallSignaling : public QObject
{
    Q_OBJECT

public:
    explicit CallSignaling(ApiClient *api, QObject *parent = nullptr);

    void start(const QString &token, const QString &sessionId);
    void stop();

    void sendOffer(const QString &toSessionId, const QString &sdp, const QString &sid);
    void sendAnswer(const QString &toSessionId, const QString &sdp, const QString &sid);
    void sendCandidate(const QString &toSessionId, const QJsonObject &candidate, const QString &sid);

    bool isRunning() const { return m_running; }

signals:
    void offerReceived(const QString &fromSessionId, const QString &sdp, const QString &sid);
    void answerReceived(const QString &fromSessionId, const QString &sdp, const QString &sid);
    void candidateReceived(const QString &fromSessionId, const QJsonObject &candidate);

private:
    void poll();
    void sendMessage(const QJsonObject &data);

    ApiClient *m_api;
    QString m_token;
    QString m_sessionId;
    bool m_running = false;
    QTimer m_pollTimer;
    QList<QJsonObject> m_pendingMessages;
    bool m_sending = false;
};

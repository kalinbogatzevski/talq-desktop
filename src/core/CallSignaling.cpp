#include "core/CallSignaling.h"
#include <QJsonDocument>
#include <QNetworkReply>
#include <QUrlQuery>
#include <QDebug>

CallSignaling::CallSignaling(ApiClient *api, QObject *parent)
    : QObject(parent)
    , m_api(api)
{
}

void CallSignaling::start(const QString &token, const QString &sessionId)
{
    m_token = token;
    m_sessionId = sessionId;
    m_running = true;
    m_pendingMessages.clear();
    qDebug() << "CallSignaling: started for" << token << "session=" << sessionId.left(20);
    poll();
}

void CallSignaling::stop()
{
    m_running = false;
    m_pollTimer.stop();
    qDebug() << "CallSignaling: stopped";
}

void CallSignaling::sendOffer(const QString &toSessionId, const QString &sdp, const QString &sid)
{
    QJsonObject payload;
    payload["type"] = QString("offer");
    payload["sdp"] = sdp;

    QJsonObject data;
    data["type"] = QString("offer");
    data["to"] = toSessionId;
    data["sid"] = sid;
    data["roomType"] = QString("video");
    data["payload"] = payload;

    sendMessage(data);
    qDebug() << "CallSignaling: sent offer to" << toSessionId.left(20);
}

void CallSignaling::sendAnswer(const QString &toSessionId, const QString &sdp, const QString &sid)
{
    QJsonObject payload;
    payload["type"] = QString("answer");
    payload["sdp"] = sdp;

    QJsonObject data;
    data["type"] = QString("answer");
    data["to"] = toSessionId;
    data["sid"] = sid;
    data["roomType"] = QString("video");
    data["payload"] = payload;

    sendMessage(data);
    qDebug() << "CallSignaling: sent answer to" << toSessionId.left(20);
}

void CallSignaling::sendCandidate(const QString &toSessionId, const QJsonObject &candidate, const QString &sid)
{
    QJsonObject payload;
    payload["candidate"] = candidate;

    QJsonObject data;
    data["type"] = QString("candidate");
    data["to"] = toSessionId;
    data["sid"] = sid;
    data["roomType"] = QString("video");
    data["payload"] = payload;

    sendMessage(data);
}

void CallSignaling::sendMessage(const QJsonObject &data)
{
    // Queue message for batch sending
    QJsonObject msg;
    msg["ev"] = QString("message");
    msg["fn"] = QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact));
    msg["sessionId"] = m_sessionId;
    m_pendingMessages.append(msg);

    // Send immediately if not already sending
    if (!m_sending && !m_pendingMessages.isEmpty()) {
        m_sending = true;
        QJsonArray messages;
        for (const auto &m : m_pendingMessages)
            messages.append(m);

        QJsonObject body;
        body["messages"] = QString::fromUtf8(QJsonDocument(messages).toJson(QJsonDocument::Compact));

        m_api->post("apps/spreed/api/v3/signaling/" + m_token, body,
            [this](bool ok, const QJsonObject &, int status) {
                if (!ok)
                    qWarning() << "CallSignaling: send failed, status=" << status;
                m_sending = false;
                // Flush any messages queued while this send was in flight
                if (!m_pendingMessages.isEmpty())
                    flushPending();
            });
        m_pendingMessages.clear();
    }
}

void CallSignaling::flushPending()
{
    if (m_sending || m_pendingMessages.isEmpty()) return;
    m_sending = true;
    QJsonArray messages;
    for (const auto &m : m_pendingMessages)
        messages.append(m);

    QJsonObject body;
    body["messages"] = QString::fromUtf8(QJsonDocument(messages).toJson(QJsonDocument::Compact));

    m_api->post("apps/spreed/api/v3/signaling/" + m_token, body,
        [this](bool ok, const QJsonObject &, int status) {
            if (!ok)
                qWarning() << "CallSignaling: send failed, status=" << status;
            m_sending = false;
            if (!m_pendingMessages.isEmpty())
                flushPending();
        });
    m_pendingMessages.clear();
}

void CallSignaling::poll()
{
    if (!m_running || m_token.isEmpty()) return;

    auto *reply = m_api->getRaw("apps/spreed/api/v3/signaling/" + m_token);

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (!m_running) return;

        int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "CallSignaling: poll error:" << status << reply->errorString();
            // Retry after 2s
            QTimer::singleShot(2000, this, &CallSignaling::poll);
            return;
        }

        QByteArray body = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(body);
        QJsonObject ocs = doc.object()["ocs"].toObject();
        QJsonArray data = ocs["data"].toArray();

        for (const auto &val : data) {
            QJsonObject msg = val.toObject();
            QString type = msg["type"].toString();

            if (type == "message") {
                QJsonObject msgData = msg["data"].toObject();
                // If data is a string, parse it
                if (msg["data"].isString())
                    msgData = QJsonDocument::fromJson(msg["data"].toString().toUtf8()).object();

                QString msgType = msgData["type"].toString();
                QString from = msgData["from"].toString();

                if (msgType == "offer") {
                    QJsonObject payload = msgData["payload"].toObject();
                    QString sdp = payload["sdp"].toString();
                    QString sid = msgData["sid"].toString();
                    qDebug() << "CallSignaling: received offer from" << from.left(20);
                    emit offerReceived(from, sdp, sid);
                }
                else if (msgType == "answer") {
                    QJsonObject payload = msgData["payload"].toObject();
                    QString sdp = payload["sdp"].toString();
                    QString sid = msgData["sid"].toString();
                    qDebug() << "CallSignaling: received answer from" << from.left(20);
                    emit answerReceived(from, sdp, sid);
                }
                else if (msgType == "candidate") {
                    QJsonObject payload = msgData["payload"].toObject();
                    QJsonObject candidate = payload["candidate"].toObject();
                    qDebug() << "CallSignaling: received candidate from" << from.left(20);
                    emit candidateReceived(from, candidate);
                }
            }
            else if (type == "usersInRoom") {
                // Participant list update — could use for call state
                QJsonArray users = msg["data"].toArray();
                for (const auto &u : users) {
                    QJsonObject user = u.toObject();
                    int inCall = user["inCall"].toInt();
                    QString sessionId = user["sessionId"].toString();
                    if (sessionId != m_sessionId && inCall > 0) {
                        qDebug() << "CallSignaling: user in call:" << sessionId.left(20) << "flags=" << inCall;
                    }
                }
            }
        }

        // Poll again immediately
        if (m_running)
            poll();
    });
}

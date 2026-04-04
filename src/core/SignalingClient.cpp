#include "core/SignalingClient.h"
#include "core/TalqLog.h"
#include <QJsonDocument>

SignalingClient::SignalingClient(ApiClient *api, QObject *parent)
    : QObject(parent)
    , m_api(api)
{
    connect(&m_ws, &QWebSocket::connected, this, &SignalingClient::onConnected);
    connect(&m_ws, &QWebSocket::disconnected, this, &SignalingClient::onDisconnected);
    connect(&m_ws, &QWebSocket::textMessageReceived, this, &SignalingClient::onTextMessage);
    connect(&m_ws, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError err) {
        qWarning() << "Signaling: WebSocket error:" << err << m_ws.errorString();
    });

    m_reconnectTimer.setSingleShot(true);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &SignalingClient::start);

    // Clear typing indicator after 15s (safety net)
    m_typingClearTimer.setSingleShot(true);
    m_typingClearTimer.setInterval(15000);
    connect(&m_typingClearTimer, &QTimer::timeout, this, [this]() {
        if (!m_typingUser.isEmpty()) {
            m_typingUser.clear();
            m_typingRoom.clear();
            emit typingUserChanged();
        }
    });
}

void SignalingClient::start()
{
    fetchSettings();
}

void SignalingClient::stop()
{
    m_reconnectTimer.stop();
    m_ws.close();
    m_sessionId.clear();
    m_reconnectDelay = 2000;
    if (m_authenticated) {
        m_authenticated = false;
        emit connectedChanged();
    }
}

void SignalingClient::fetchSettings()
{
    m_api->get("apps/spreed/api/v3/signaling/settings",
        [this](bool ok, const QJsonObject &data, int) {
            if (!ok) {
                qWarning() << "Signaling: failed to get settings";
                reconnect();
                return;
            }

            m_signalingUrl = data["server"].toString().trimmed();
            if (m_signalingUrl.isEmpty()) {
                qDebug() << "Signaling: no standalone server configured";
                return;
            }

            // Get v1.0 auth params
            QJsonObject authParams = data["helloAuthParams"].toObject();
            QJsonObject v1 = authParams["1.0"].toObject();
            m_userId = v1["userid"].toString();
            m_ticket = v1["ticket"].toString();

            if (m_ticket.isEmpty()) {
                qWarning() << "Signaling: no ticket in settings";
                return;
            }

            qDebug() << "Signaling: server at" << m_signalingUrl;
            connectWebSocket();
        });
}

void SignalingClient::connectWebSocket()
{
    QString wsUrl = m_signalingUrl;
    wsUrl.replace("https://", "wss://").replace("http://", "ws://");
    if (!wsUrl.endsWith("/"))
        wsUrl += "/";
    wsUrl += "spreed";

    qDebug() << "Signaling: connecting to" << wsUrl;
    m_ws.open(QUrl(wsUrl));
}

void SignalingClient::onConnected()
{
    qDebug() << "Signaling: WebSocket connected, waiting for welcome";
    m_reconnectDelay = 2000;
}

void SignalingClient::onDisconnected()
{
    qDebug() << "Signaling: disconnected";
    bool wasAuth = m_authenticated;
    m_authenticated = false;
    m_sessionId.clear();
    if (wasAuth) emit connectedChanged();
    reconnect();
}

void SignalingClient::onTextMessage(const QString &msg)
{
    QJsonDocument doc = QJsonDocument::fromJson(msg.toUtf8());
    if (doc.isNull()) {
        qWarning() << "Signaling: received malformed JSON:" << msg.left(200);
        return;
    }
    QJsonObject obj = doc.object();
    QString type = obj["type"].toString();

    // Debug: log all incoming WebSocket messages
    if (type != "hello" && type != "welcome")
        qDebug() << "Signaling: WS <<" << type << msg.left(200);

    if (type == "welcome") {
        qDebug() << "Signaling: received welcome, sending hello";
        sendHello();
    }
    else if (type == "hello") {
        QJsonObject helloObj = obj["hello"].toObject();
        m_sessionId = helloObj["sessionid"].toString();
        m_authenticated = true;

        // Check server features (MCU, etc.)
        QJsonArray features = helloObj["server"].toObject()["features"].toArray();
        QStringList featureList;
        for (const auto &f : features)
            featureList << f.toString();
        m_hasMcu = featureList.contains("mcu");
        qDebug() << "Signaling: features:" << featureList.join(", ") << "MCU:" << m_hasMcu;

        emit connectedChanged();
        qDebug() << "Signaling: authenticated, session:" << m_sessionId.left(20) + "...";

        // Re-join room if we had one
        if (!m_currentRoom.isEmpty()) {
            joinRoom(m_currentRoom);
        }
    }
    else if (type == "error") {
        qWarning() << "Signaling: error:" << obj["error"].toObject()["message"].toString();
    }
    else if (type == "room") {
        qDebug() << "Signaling: joined room";
        emit roomJoined();
    }
    else if (type == "message") {
        QJsonObject messageObj = obj["message"].toObject();
        QJsonObject msgData = messageObj["data"].toObject();
        QString msgType = msgData["type"].toString();
        QString senderSessionId = messageObj["sender"].toObject()["sessionid"].toString();

        // WebRTC signaling messages (session-targeted, dispatch before room filter)
        if (msgType == "offer") {
            QString sdp = msgData["payload"].toObject()["sdp"].toString();
            QString sid = msgData["sid"].toString();
            QString roomType = msgData["roomType"].toString("video");
            qDebug() << "Signaling: received offer from" << senderSessionId.left(20) << "sid=" << sid;
            emit offerReceived(senderSessionId, sdp, sid, roomType);
            return;
        }
        if (msgType == "answer") {
            QString sdp = msgData["payload"].toObject()["sdp"].toString();
            QString roomType = msgData["roomType"].toString("video");
            qDebug() << "Signaling: received answer from" << senderSessionId.left(20);
            emit answerReceived(senderSessionId, sdp, roomType);
            return;
        }
        if (msgType == "candidate") {
            QJsonObject payload = msgData["payload"].toObject();
            // MCU (Janus) sends: payload = {candidate: {candidate: "...", sdpMLineIndex: N}}
            // P2P client sends: payload = {candidate: {candidate: "...", sdpMLineIndex: N}}
            // Extract the inner candidate object if double-nested
            QJsonObject candidate;
            if (payload.contains("candidate") && payload["candidate"].isObject()) {
                candidate = payload["candidate"].toObject();
            } else {
                candidate = payload;
            }
            QString candRoomType = msgData["roomType"].toString("video");
            qDebug() << "Signaling: received candidate from" << senderSessionId.left(20)
                     << "roomType=" << candRoomType;
            emit candidateReceived(senderSessionId, candidate, candRoomType);
            return;
        }
        if (msgType == "endOfCandidates") {
            emit endOfCandidatesReceived(senderSessionId);
            return;
        }
        if (msgType == "mute" || msgType == "unmute") {
            QString media = msgData["payload"].toObject()["name"].toString();
            bool muted = (msgType == "mute");
            emit remoteMuteChanged(senderSessionId, media, muted);
            return;
        }

        if (msgType == "unshareScreen") {
            qDebug() << "Signaling: received unshareScreen from" << senderSessionId.left(20);
            emit screenShareStopped(senderSessionId);
            return;
        }

        // Room-scoped messages (typing indicators)
        QString senderRoom = messageObj["sender"].toObject()["roomid"].toString();
        if (!senderRoom.isEmpty() && senderRoom != m_currentRoom) {
            return;
        }

        if (msgType == "startedTyping") {
            QJsonObject senderObj = messageObj["sender"].toObject();
            QString sender = senderObj["displayname"].toString();
            if (sender.isEmpty()) {
                // HPB may not provide displayname — use userid as fallback key
                // and look up the display name from participant data if available
                sender = m_participantNames.value(
                    senderObj["userid"].toString(),
                    senderObj["userid"].toString());
            }

            if (sender != m_userId && !sender.isEmpty()) {
                m_typingUser = sender;
                m_typingRoom = m_currentRoom;
                emit typingUserChanged();
                m_typingClearTimer.start();
            }
        }
        else if (msgType == "stoppedTyping") {
            if (!m_typingUser.isEmpty()) {
                m_typingUser.clear();
                m_typingRoom.clear();
                emit typingUserChanged();
                m_typingClearTimer.stop();
            }
        }
    }
    else if (type == "event") {
        QJsonObject event = obj["event"].toObject();
        QString target = event["target"].toString();
        QString eventType = event["type"].toString();
        qDebug() << "Signaling: event target=" << target << "type=" << eventType;

        if (target == "room" && eventType == "join") {
            QJsonArray joins = event["join"].toArray();
            for (const auto &j : joins) {
                QString sid = j.toObject()["sessionid"].toString();
                if (!sid.isEmpty() && sid != m_sessionId) {
                    qDebug() << "Signaling: room peer joined:" << sid.left(20);
                    emit roomPeerJoined(sid);
                }
            }
        }

        if (target == "participants") {
            QJsonObject update = event["update"].toObject();
            QJsonArray users = update["users"].toArray();
            TLOG_SIG("participants update:" << users.size() << "users in room" << update["roomid"].toString());
            for (const QJsonValue &val : users) {
                QJsonObject user = val.toObject();
                int inCall = user["inCall"].toInt();
                QString sid = user["sessionId"].toString();
                if (sid.isEmpty()) continue;
                if (sid == m_sessionId) {
                    TLOG_SIG("  skip self sid=" << sid.left(20) << "inCall=" << inCall);
                    continue;
                }

                // Cache userid → displayName for typing indicators
                QString userId = user["actorId"].toString();
                QString displayName = user["displayName"].toString();
                if (!userId.isEmpty() && !displayName.isEmpty())
                    m_participantNames[userId] = displayName;

                int prevFlags = m_participantCallFlags.value(sid, 0);
                m_participantCallFlags[sid] = inCall;

                TLOG_SIG("  participant sid=" << sid.left(20) << "inCall=" << inCall << "prev=" << prevFlags << "name=" << displayName);

                if (prevFlags == 0 && inCall > 0) {
                    TLOG_CALL("participant JOINED call sid=" << sid.left(20) << "flags=" << inCall);
                    emit participantJoinedCall(sid, inCall, displayName);
                } else if (prevFlags > 0 && inCall == 0) {
                    TLOG_CALL("participant LEFT call sid=" << sid.left(20));
                    // Prune maps to prevent unbounded growth
                    m_participantCallFlags.remove(sid);
                    if (!userId.isEmpty())
                        m_participantNames.remove(userId);
                    emit participantLeftCall(sid);
                } else if (prevFlags != inCall && inCall > 0) {
                    TLOG_CALL("participant flags CHANGED sid=" << sid.left(20) << prevFlags << "->" << inCall);
                    emit participantFlagsChanged(sid, prevFlags, inCall);
                }
            }
        }
    }
}

void SignalingClient::sendHello()
{
    QJsonObject hello;
    hello["type"] = QString("hello");

    QJsonObject helloData;
    // Use v1.0 auth (userid/ticket). v2.0 requires JWT which we don't have.
    helloData["version"] = QString("1.0");

    QJsonObject auth;
    auth["url"] = m_api->serverUrl() + "/ocs/v2.php/apps/spreed/api/v3/signaling/backend";

    QJsonObject params;
    params["userid"] = m_userId;
    params["ticket"] = m_ticket;
    auth["params"] = params;

    helloData["auth"] = auth;
    hello["hello"] = helloData;

    QString json = QJsonDocument(hello).toJson(QJsonDocument::Compact);
    qDebug() << "Signaling: WS >> hello (ticket redacted)";
    m_ws.sendTextMessage(json);
}

void SignalingClient::joinRoom(const QString &token)
{
    m_currentRoom = token;

    // Clear state from previous room
    if (!m_typingUser.isEmpty()) {
        m_typingUser.clear();
        emit typingUserChanged();
        m_typingClearTimer.stop();
    }
    m_participantCallFlags.clear();
    m_participantNames.clear();

    // Join as active participant — the response contains the sessionId
    // which the signaling server needs to verify room access
    QJsonObject empty;
    m_api->post("apps/spreed/api/v4/room/" + token + "/participants/active", empty,
        [this, token](bool ok, const QJsonObject &data, int) {
            qDebug() << "Signaling: participants/active response ok=" << ok
                     << "room match=" << (m_currentRoom == token)
                     << "auth=" << m_authenticated
                     << "sessionId=" << data["sessionId"].toString().left(20);
            if (!ok || m_currentRoom != token || !m_authenticated) return;

            // Extract the Nextcloud session ID from the response
            QString ncSessionId = data["sessionId"].toString();

            QJsonObject msg;
            msg["type"] = QString("room");

            QJsonObject room;
            room["roomid"] = token;
            if (!ncSessionId.isEmpty())
                room["sessionid"] = ncSessionId;
            msg["room"] = room;

            m_ws.sendTextMessage(QJsonDocument(msg).toJson(QJsonDocument::Compact));
            qDebug() << "Signaling: joining room" << token << "with session" << ncSessionId.left(20) + "...";
        });
}

void SignalingClient::sendStartedTyping()
{
    sendRoomMessage("startedTyping");
}

void SignalingClient::sendStoppedTyping()
{
    sendRoomMessage("stoppedTyping");
}

void SignalingClient::sendRoomMessage(const QString &msgType)
{
    if (!m_authenticated || m_currentRoom.isEmpty()) return;

    QJsonObject msg;
    msg["type"] = QString("message");

    QJsonObject message;
    QJsonObject recipient;
    recipient["type"] = QString("room");
    message["recipient"] = recipient;

    QJsonObject data;
    data["type"] = msgType;
    message["data"] = data;

    msg["message"] = message;

    m_ws.sendTextMessage(QJsonDocument(msg).toJson(QJsonDocument::Compact));
}

// --- WebRTC call signaling ---

void SignalingClient::sendSessionMessage(const QString &toSessionId, const QString &type,
                                          const QJsonObject &payload, const QString &sid,
                                          const QJsonObject &extraData, const QString &roomType)
{
    if (!m_authenticated) return;

    QJsonObject msg;
    msg["type"] = QString("message");

    QJsonObject message;
    QJsonObject recipient;
    recipient["type"] = QString("session");
    recipient["sessionid"] = toSessionId;
    message["recipient"] = recipient;

    QJsonObject data;
    data["type"] = type;
    data["to"] = toSessionId;
    data["sid"] = sid;
    data["roomType"] = roomType;
    data["payload"] = payload;
    // Merge extra fields (e.g. audiocodec, videocodec for Janus room creation)
    for (auto it = extraData.begin(); it != extraData.end(); ++it)
        data[it.key()] = it.value();
    message["data"] = data;

    msg["message"] = message;

    QString json = QJsonDocument(msg).toJson(QJsonDocument::Compact);
    qDebug() << "Signaling: WS >>" << type << json.left(500);
    m_ws.sendTextMessage(json);
}

void SignalingClient::sendOffer(const QString &toSessionId, const QString &sdp,
                                const QString &sid, const QString &nick, const QString &roomType)
{
    QJsonObject payload;
    payload["type"] = QString("offer");
    payload["sdp"] = sdp;
    if (!nick.isEmpty()) payload["nick"] = nick;
    // Include codec preferences so the signaling server passes them to Janus
    // when creating the videoroom. Without these, Janus sets codec to 'none'
    // and rejects all audio/video.
    QJsonObject extra;
    extra["audiocodec"] = QString("opus");
    extra["videocodec"] = QString("vp8");
    sendSessionMessage(toSessionId, "offer", payload, sid, extra, roomType);
    qDebug() << "Signaling: sent offer to" << toSessionId.left(20) << "sid=" << sid.left(10);
}

void SignalingClient::sendAnswer(const QString &toSessionId, const QString &sdp,
                                  const QString &sid, const QString &nick, const QString &roomType)
{
    QJsonObject payload;
    payload["type"] = QString("answer");
    payload["sdp"] = sdp;
    if (!nick.isEmpty()) payload["nick"] = nick;
    sendSessionMessage(toSessionId, "answer", payload, sid, {}, roomType);
    qDebug() << "Signaling: sent answer to" << toSessionId.left(20) << "sid=" << sid.left(10);
}

void SignalingClient::sendCandidate(const QString &toSessionId, const QJsonObject &candidate,
                                     const QString &sid, const QString &roomType)
{
    QJsonObject payload;
    payload["candidate"] = candidate;
    sendSessionMessage(toSessionId, "candidate", payload, sid, {}, roomType);
}

void SignalingClient::sendEndOfCandidates(const QString &toSessionId, const QString &sid)
{
    sendSessionMessage(toSessionId, "endOfCandidates", QJsonObject(), sid);
}

void SignalingClient::sendBroadcastMessage(const QJsonObject &data)
{
    if (!m_authenticated || m_currentRoom.isEmpty()) return;

    QJsonObject msg;
    msg["type"] = QString("message");

    QJsonObject message;
    QJsonObject recipient;
    recipient["type"] = QString("room");
    message["recipient"] = recipient;
    message["data"] = data;
    msg["message"] = message;

    m_ws.sendTextMessage(QJsonDocument(msg).toJson(QJsonDocument::Compact));
    qDebug() << "Signaling: sent broadcast message" << data["type"].toString();
}

void SignalingClient::sendMinimalMessage(const QString &toSessionId, const QJsonObject &data)
{
    if (!m_authenticated) return;

    QJsonObject msg;
    msg["type"] = QString("message");

    QJsonObject message;
    QJsonObject recipient;
    recipient["type"] = QString("session");
    recipient["sessionid"] = toSessionId;
    message["recipient"] = recipient;
    message["data"] = data;
    msg["message"] = message;

    m_ws.sendTextMessage(QJsonDocument(msg).toJson(QJsonDocument::Compact));
    qDebug() << "Signaling: sent minimal message" << data["type"].toString() << "to" << toSessionId.left(20);
}

void SignalingClient::requestOffer(const QString &sessionId, const QString &roomType)
{
    if (!m_authenticated) return;

    QJsonObject msg;
    msg["type"] = QString("message");

    QJsonObject message;
    QJsonObject recipient;
    recipient["type"] = QString("session");
    recipient["sessionid"] = sessionId;
    message["recipient"] = recipient;

    QJsonObject data;
    data["type"] = QString("requestoffer");
    data["roomType"] = roomType;
    message["data"] = data;

    msg["message"] = message;

    m_ws.sendTextMessage(QJsonDocument(msg).toJson(QJsonDocument::Compact));
    qDebug() << "Signaling: sent requestOffer to" << sessionId.left(20) << "type=" << roomType;
}

void SignalingClient::reconnect()
{
    if (m_api->serverUrl().isEmpty()) return;
    m_reconnectTimer.start(m_reconnectDelay);
    m_reconnectDelay = qMin(m_reconnectDelay * 2, 60000);
}

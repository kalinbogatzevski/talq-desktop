#include "core/SignalingClient.h"
#include "core/TalqLog.h"
#include "core/ChatSyncLogic.h"
#include <QJsonDocument>
#include <QSettings>
#include <QDateTime>

// bug 3 — a cached peer TalQ version is only trusted as "current" if observed
// within this window; older entries render as no chip rather than a wrong
// number. Generous because TalQ identity is sticky and re-announces are
// infrequent (room overlap / calls), so we would rather under-blank than show
// a confidently-wrong stale version.
static constexpr qint64 kPeerVersionFreshMs = 30LL * 24 * 60 * 60 * 1000; // 30 days

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

    loadPersistedPeerClients();

    // Periodic talq.client re-announce while in a room. Defense-in-depth
    // for the upgrade case: a peer that joined the room before us
    // upgraded silently keeps a stale-version cache for the whole session
    // unless something forces a re-announce. The one-shot room-join hello
    // covers a fresh peer-join, but co-resident sessions need this tick.
    // 5 minutes is small relative to a long call/chat session and well
    // under the spam threshold (the receive path de-duplicates by info
    // string equality, so an unchanged version triggers no state churn).
    m_talqClientReannounce.setInterval(5 * 60 * 1000);
    connect(&m_talqClientReannounce, &QTimer::timeout, this, [this]() {
        if (!m_currentRoom.isEmpty() && m_ws.state() == QAbstractSocket::ConnectedState)
            sendTalqClientHello();
    });
}

void SignalingClient::start()
{
    fetchSettings();
}

void SignalingClient::stop()
{
    m_reconnectTimer.stop();
    m_talqClientReannounce.stop();
    sendBye();                 // graceful HPB disconnect (matches upstream)
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

            // Auth params: prefer hello v2.0 (signed JWT) when the server
            // provides it, exactly like the official client; fall back to
            // v1.0 (userid/ticket) otherwise.
            QJsonObject authParams = data["helloAuthParams"].toObject();
            QJsonObject v1 = authParams["1.0"].toObject();
            m_userId = v1["userid"].toString();
            m_ticket = v1["ticket"].toString();
            // (v2.0 token is also offered by the server but our v2 hello
            // envelope was rejected at runtime — see sendHello(); we use
            // v1.0, which requires the ticket.)
            m_helloV2Token = authParams["2.0"].toObject()["token"].toString();

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
        qInfo() << "Signaling: joined room";
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
            qInfo() << "Signaling: received offer from" << senderSessionId.left(20) << "sid=" << sid;
            emit offerReceived(senderSessionId, sdp, sid, roomType);
            return;
        }
        if (msgType == "answer") {
            QString sdp = msgData["payload"].toObject()["sdp"].toString();
            QString roomType = msgData["roomType"].toString("video");
            qInfo() << "Signaling: received answer from" << senderSessionId.left(20);
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
            qInfo() << "Signaling: received candidate from" << senderSessionId.left(20)
                     << "roomType=" << candRoomType;
            emit candidateReceived(senderSessionId, candidate, candRoomType);
            return;
        }
        if (msgType == "endOfCandidates") {
            emit endOfCandidatesReceived(senderSessionId);
            return;
        }
        // 0.41.x-beta — TalQ-private P2P overlay. Custom session-targeted
        // type the HPB relays untouched (Janus never sees it), so 1:1 SDP
        // + ICE travels peer-to-peer and the media goes direct.
        if (msgType.startsWith(QStringLiteral("talq.p2p."))) {
            const QString subtype = msgType.mid(9);
            qInfo() << "Signaling: received talq.p2p." << subtype
                    << "from" << senderSessionId.left(20);
            emit p2pSignalReceived(senderSessionId, subtype,
                                   msgData["payload"].toObject());
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
        else if (msgType == "talq.client") {
            // Client-originated TalQ identification broadcast. Travels the
            // same path as typing indicators (recipient.type=room → echoed
            // back to all room participants as type="message").
            //
            // Prefer userId from the payload itself (we put it there in
            // sendTalqClientHello); fall back to the sender annotation
            // added by the spreedbackend.
            QJsonObject senderObj = messageObj["sender"].toObject();
            QString senderUid = msgData["userid"].toString();
            if (senderUid.isEmpty())
                senderUid = senderObj["userid"].toString();
            const QString senderSid = senderObj["sessionid"].toString();
            if (senderSid == m_sessionId || senderUid == m_userId) {
                // Echo of our own broadcast — ignore.
            } else if (!senderUid.isEmpty()) {
                const QString client = msgData["client"].toString();
                const QString version = msgData["version"].toString();
                const QString info = client + "/" + version;
                updatePeerClient(senderUid, info);  // bug 3 — stamps freshness + persists
                if (!senderSid.isEmpty())
                    m_sessionToUserId[senderSid] = senderUid;
            } else {
                qDebug() << "Signaling: received talq.client without userId — sender=" << senderObj;
            }
        }
    }
    else if (type == "event") {
        QJsonObject event = obj["event"].toObject();
        QString target = event["target"].toString();
        QString eventType = event["type"].toString();
        qDebug() << "Signaling: event target=" << target << "type=" << eventType;

        // HPB broadcasts chat events as room messages with data.type=="chat".
        // The payload looks like:
        //   { target: "room", type: "message",
        //     message: { roomid: "<token>",
        //                data: { type: "chat", chat: { refresh: true, comment: {...} } } } }
        // Emit a refresh signal so MessageListModel can pull the latest state
        // (which includes the X-Chat-Last-Common-Read header). Without this
        // we only learn about read-marker advances when a new message also
        // arrives via the chat long-poll.
        if (target == "room" && eventType == "message") {
            QJsonObject msgObj = event["message"].toObject();
            QJsonObject data = msgObj["data"].toObject();
            const QString dataType = data["type"].toString();
            if (dataType == "chat") {
                QString roomToken = msgObj["roomid"].toString();
                if (!roomToken.isEmpty()) {
                    TLOG_SIG("chat refresh hint for room" << roomToken);
                    emit chatRefreshNeeded(roomToken);
                }
            } else if (dataType == "talq.client") {
                // Defense-in-depth fallback path. The real delivery channel
                // for client-originated room broadcasts is the top-level
                // type="message" branch above (same path typing indicators
                // travel). This event/room/message branch is kept in case a
                // future server variant routes such messages here instead.
                QJsonObject sender = msgObj["sender"].toObject();
                const QString senderSid = sender["sessionid"].toString();
                QString senderUid = data["userid"].toString();
                if (senderUid.isEmpty()) senderUid = sender["userid"].toString();
                if (senderUid.isEmpty()) senderUid = m_sessionToUserId.value(senderSid);
                if (senderSid != m_sessionId && senderUid != m_userId && !senderUid.isEmpty()) {
                    const QString info = data["client"].toString() + "/" + data["version"].toString();
                    updatePeerClient(senderUid, info);  // bug 3 — stamps freshness + persists
                }
            }
        }

        if (target == "room" && eventType == "join") {
            QJsonArray joins = event["join"].toArray();
            bool anyNonSelf = false;
            for (const auto &j : joins) {
                QString sid = j.toObject()["sessionid"].toString();
                if (!sid.isEmpty() && sid != m_sessionId) {
                    qDebug() << "Signaling: room peer joined:" << sid.left(20);
                    emit roomPeerJoined(sid);
                    anyNonSelf = true;
                }
            }
            // Re-announce our TalQ version so the newcomer learns about us.
            // (HPB broadcasts are one-shot — peers that joined before our
            // initial hello already cached it.)
            if (anyNonSelf)
                sendTalqClientHello();
        }

        // #bug2 -- room peer LEFT (HPB room/leave). The payload is an array of
        // session-id strings on this HPB (tolerate the object form defensively).
        // Previously this event was logged and dropped: a vanished session emits
        // no inCall>0 -> 0 transition, so participantLeftCall never fired and a
        // peer whose publisher reconnected (new session/SSRCs) left the other
        // side decoding dead SSRCs forever.
        if (target == "room" && eventType == "leave") {
            const QJsonArray leaves = event["leave"].toArray();
            for (const QJsonValue &l : leaves) {
                QString sid = l.toString();
                if (sid.isEmpty())
                    sid = l.toObject()["sessionid"].toString();
                if (sid.isEmpty() || sid == m_sessionId)
                    continue;
                qDebug() << "Signaling: room peer left:" << sid.left(20);
                m_participantCallFlags.remove(sid);
                emit roomPeerLeft(sid);
            }
        }

        if (target == "participants") {
            QJsonObject update = event["update"].toObject();
            QJsonArray users = update["users"].toArray();
            TLOG_SIG("participants update:" << users.size() << "users in room" << update["roomid"].toString());
            bool sawNewPeer = false;
            for (const QJsonValue &val : users) {
                QJsonObject user = val.toObject();
                int inCall = user["inCall"].toInt();
                QString sid = user["sessionId"].toString();
                if (sid.isEmpty()) continue;
                if (sid == m_sessionId) {
                    TLOG_SIG("  skip self sid=" << sid.left(20) << "inCall=" << inCall);
                    continue;
                }

                // Cache userid → displayName for typing indicators, and
                // sessionId → userid for the TalQ peer-identification fallback
                // (so we can resolve a sender's NC userId even on servers that
                // omit userid from broadcast sender annotations).
                QString userId = user["actorId"].toString();
                QString displayName = user["displayName"].toString();
                if (!userId.isEmpty() && !displayName.isEmpty())
                    m_participantNames[userId] = displayName;
                if (!userId.isEmpty())
                    m_sessionToUserId[sid] = userId;
                // Some servers/participant events omit displayName (only
                // actorId). Emitting an empty name is the "incoming call
                // shows 'Call' instead of the caller" bug. Resolve from
                // the name cache (filled by the room participant list /
                // earlier events), else fall back to the userId itself —
                // anything identifiable beats a generic "Call".
                if (displayName.isEmpty() && !userId.isEmpty())
                    displayName = m_participantNames.value(userId, userId);

                if (!m_participantCallFlags.contains(sid))
                    sawNewPeer = true;
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
            // A peer we hadn't seen in this room appeared. HPB does not
            // reliably deliver a room/join event for them, so re-announce our
            // TalQ hello here too — this is the more dependable trigger for
            // the mutual peer-identification handshake.
            if (sawNewPeer)
                sendTalqClientHello();
        }
    }
}

void SignalingClient::sendHello()
{
    QJsonObject hello;
    hello["type"] = QString("hello");

    QJsonObject helloData;
    // hello v1.0 (userid + ticket). This is a server-accepted protocol
    // version and is the path that actually connects against the
    // standalone signaling backend. A v2.0 (signed-JWT) attempt was
    // rejected by the server at runtime with {"code":"invalid_format"}
    // (the exact v2 envelope this server expects is unverified), so we
    // stay on v1.0 rather than ship a broken handshake — the rest of the
    // flow (room/MCU/offer) is identical regardless of hello version.
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
    qDebug() << "Signaling: WS >> hello v1.0 (creds redacted)";
    m_ws.sendTextMessage(json);
}

void SignalingClient::sendBye()
{
    if (!m_authenticated || m_ws.state() != QAbstractSocket::ConnectedState)
        return;
    QJsonObject msg;
    msg["type"] = QString("bye");
    msg["bye"] = QJsonObject();
    m_ws.sendTextMessage(QJsonDocument(msg).toJson(QJsonDocument::Compact));
    qDebug() << "Signaling: WS >> bye";
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

            // Announce our TalQ version to other room participants. Other
            // TalQ clients cache it (by userId) and display it in conversation
            // info / call dialog. The official web client ignores it.
            sendTalqClientHello();
            // Arm the 5-minute periodic re-announce so peers with stale
            // caches (e.g. they joined the room while we were on the
            // pre-upgrade build) eventually heal.
            m_talqClientReannounce.start();
        });
}

void SignalingClient::sendTalqClientHello()
{
    if (m_userId.isEmpty()) {
        TLOG_SIG("sendTalqClientHello skipped — no userId yet");
        return;
    }
    QJsonObject data;
    data["type"] = QStringLiteral("talq.client");
    data["client"] = QStringLiteral("TalQ");
    data["version"] = QStringLiteral(TALQ_VERSION);
    // Self-identify in the payload itself — the spreedbackend's sender
    // annotation on room-broadcasts is unreliable (sessionid only, no userid
    // on some configs), so receivers should prefer this field.
    data["userid"] = m_userId;
    qDebug() << "Signaling: announcing TalQ/" TALQ_VERSION " in room" << m_currentRoom;
    sendBroadcastMessage(data);
}

// QSettings keys treat '/' as a group separator and choke on other characters
// that can appear in federated user IDs, so the userId is percent-encoded.
QString SignalingClient::peerClientInfo(const QString &userId) const
{
    // bug 3 — only report a cached version if it was observed recently. A
    // value loaded from disk that predates the freshness window (or a legacy
    // entry with no recorded timestamp, lastSeen==0) is treated as unknown so
    // the UI shows no chip instead of a confidently-wrong stale number.
    const qint64 lastSeen = m_peerClientSeen.value(userId, 0);
    if (!talq::isPeerVersionFresh(lastSeen,
                                  QDateTime::currentMSecsSinceEpoch(),
                                  kPeerVersionFreshMs))
        return QString();
    return m_peerClientInfo.value(userId);
}

void SignalingClient::updatePeerClient(const QString &userId, const QString &info)
{
    // bug 3 — single entry point that records a peer's TalQ version with a
    // fresh timestamp and persists it. Used by the HPB broadcast handlers and
    // by CallManager's in-call data-channel observation. The timestamp is
    // always refreshed (so a re-announce of the same version keeps it fresh);
    // peerClientInfoChanged is emitted whenever the displayed string changes
    // or a previously-stale value becomes current again, so the chrome
    // repaints.
    if (userId.isEmpty() || userId == m_userId || info.isEmpty())
        return;
    const QString prev = peerClientInfo(userId);   // freshness-gated current display
    m_peerClientInfo[userId] = info;
    m_peerClientSeen[userId] = QDateTime::currentMSecsSinceEpoch();
    persistPeerClient(userId, info);
    if (prev != info) {
        qDebug() << "Signaling: peer TalQ client" << userId << "=" << info;
        emit peerClientInfoChanged(userId, info);
    }
}

// QSettings keys treat '/' as a group separator and choke on other characters
// that can appear in federated user IDs, so the userId is percent-encoded.
void SignalingClient::loadPersistedPeerClients()
{
    QSettings s;
    s.beginGroup(QStringLiteral("peerClients"));
    const QStringList keys = s.childKeys();
    for (const QString &k : keys) {
        const QString uid = QString::fromUtf8(
            QByteArray::fromPercentEncoding(k.toLatin1()));
        const QString info = s.value(k).toString();
        if (!uid.isEmpty() && !info.isEmpty())
            m_peerClientInfo[uid] = info;
    }
    s.endGroup();
    // bug 3 — parallel last-seen timestamps. Entries written by an older build
    // have no timestamp here, so they default to 0 and are treated as stale by
    // peerClientInfo() — which is exactly what prevents the long-stale 0.28.3
    // entry from being shown as current after upgrade.
    s.beginGroup(QStringLiteral("peerClientsSeen"));
    const QStringList seenKeys = s.childKeys();
    for (const QString &k : seenKeys) {
        const QString uid = QString::fromUtf8(
            QByteArray::fromPercentEncoding(k.toLatin1()));
        if (!uid.isEmpty())
            m_peerClientSeen[uid] = s.value(k).toLongLong();
    }
    s.endGroup();
    if (!m_peerClientInfo.isEmpty())
        qDebug() << "Signaling: loaded" << m_peerClientInfo.size()
                 << "persisted peer client(s)";
}

void SignalingClient::persistPeerClient(const QString &userId, const QString &info)
{
    if (userId.isEmpty()) return;
    const QString key = QString::fromLatin1(userId.toUtf8().toPercentEncoding());
    QSettings s;
    s.beginGroup(QStringLiteral("peerClients"));
    s.setValue(key, info);
    s.endGroup();
    // Persist the freshness timestamp alongside (set by updatePeerClient).
    s.beginGroup(QStringLiteral("peerClientsSeen"));
    s.setValue(key, m_peerClientSeen.value(userId, QDateTime::currentMSecsSinceEpoch()));
    s.endGroup();
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
                                const QString &sid, const QString &nick,
                                const QString &roomType, const QString &broadcaster,
                                bool mcuCodecHints)
{
    QJsonObject payload;
    payload["type"] = QString("offer");
    payload["sdp"] = sdp;
    if (!nick.isEmpty()) payload["nick"] = nick;
    // Codec preferences passed to the signaling server (offer-codecs) so
    // Janus creates the room with the right codecs; without them Janus
    // sets codec 'none' and rejects media. The screen stream is
    // video-only (VP8) and the publisher must carry a `broadcaster`
    // field = its own session id (upstream Peer.send), or Janus can't
    // associate the screen publisher.
    // We publish hardware **H264** (highest quality at the raised HPB
    // bitrate ceiling, near-zero CPU). Janus is an SFU (no transcode) and
    // forces ONE codec per room from this preference list intersected
    // with what the publisher offers, so the first publisher's list here
    // decides the whole room. "h264,vp8" => H264 when offered (all
    // updated TalQ clients), VP8 only as a safety fallback. By project
    // decision, non-H264 clients simply won't see video — we never
    // transcode or downgrade the conference for one stale client.
    QJsonObject extra;
    // 0.41.9 — omit codec hints for true P2P so the HPB relays the offer
    // session-to-session instead of hijacking it as a Janus MCU publish.
    if (mcuCodecHints) {
        if (roomType == "screen") {
            extra["videocodec"] = QString("h264,vp8");
            if (!broadcaster.isEmpty())
                extra["broadcaster"] = broadcaster;
        } else {
            extra["audiocodec"] = QString("opus");
            extra["videocodec"] = QString("h264,vp8");
        }
    }
    sendSessionMessage(toSessionId, "offer", payload, sid, extra, roomType);
    qDebug() << "Signaling: sent offer to" << toSessionId.left(20)
             << "sid=" << sid.left(10) << "roomType=" << roomType;
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

void SignalingClient::sendEndOfCandidates(const QString &toSessionId, const QString &sid,
                                          const QString &roomType)
{
    sendSessionMessage(toSessionId, "endOfCandidates", QJsonObject(), sid, {}, roomType);
}

void SignalingClient::sendSelectStream(const QString &toSessionId, const QString &sid,
                                       int substream, int temporal, const QString &roomType)
{
    QJsonObject payload;
    payload["substream"] = substream;
    payload["temporal"]  = temporal;
    sendSessionMessage(toSessionId, "selectStream", payload, sid, {}, roomType);
    qDebug() << "Signaling: selectStream to" << toSessionId.left(20)
             << "substream=" << substream << "temporal=" << temporal;
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

void SignalingClient::sendP2pSignal(const QString &toSessionId, const QString &subtype,
                                    const QJsonObject &payload)
{
    // Custom type → the HPB relays it session-to-session verbatim (NOT
    // intercepted as an MCU op), so the SDP/ICE rides peer-to-peer and the
    // media goes direct WebRTC, bypassing Janus. See header comment.
    QJsonObject data;
    data["type"]    = QStringLiteral("talq.p2p.") + subtype;
    data["payload"] = payload;
    sendMinimalMessage(toSessionId, data);
    qDebug() << "Signaling: sent talq.p2p." << subtype << "to" << toSessionId.left(20);
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
    qInfo() << "Signaling: sent requestOffer to" << sessionId.left(20) << "type=" << roomType;
}

void SignalingClient::reconnect()
{
    if (m_api->serverUrl().isEmpty()) return;
    m_reconnectTimer.start(m_reconnectDelay);
    m_reconnectDelay = qMin(m_reconnectDelay * 2, 60000);
}

# Screen Sharing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Share primary monitor to Janus MCU and receive screen shares from remote participants, matching the Nextcloud Talk browser protocol.

**Architecture:** New ScreenSharePipeline class (send-only webrtcbin, d3d11screencapturesrc). SignalingClient extended with roomType parameter. CallManager routes screen share signaling separately from video. SubscribePipeline reused for receiving. Share button added to CallDialog.

**Tech Stack:** GStreamer d3d11screencapturesrc, webrtcbin, VP8, Qt6 signals

**Spec:** `docs/superpowers/specs/2026-04-03-screen-sharing-design.md`

---

## File Map

| File | Change | Responsibility |
|---|---|---|
| `src/core/SignalingClient.h` | Modify | Add roomType to signals/methods, add public sendRoomMessage |
| `src/core/SignalingClient.cpp` | Modify | Parse roomType, pass through, implement sendRoomMessage |
| `src/core/ScreenSharePipeline.h` | **New** | Send-only screen capture pipeline header |
| `src/core/ScreenSharePipeline.cpp` | **New** | Pipeline implementation |
| `src/core/CallManager.h` | Modify | Add screen share members, signals, toggleScreenShare |
| `src/core/CallManager.cpp` | Modify | Screen share orchestration, signaling routing |
| `src/ui/CallDialog.h` | Modify | Add screen share button member |
| `src/ui/CallDialog.cpp` | Modify | Button creation, signal connections |
| `CMakeLists.txt` | Modify | Add ScreenSharePipeline sources |

---

### Task 1: SignalingClient — add roomType to signals and methods

**Files:**
- Modify: `src/core/SignalingClient.h`
- Modify: `src/core/SignalingClient.cpp`

- [ ] **Step 1: Update signal signatures in SignalingClient.h**

In `src/core/SignalingClient.h`, replace the signals (lines 65-66):

Old:
```cpp
    void offerReceived(const QString &fromSessionId, const QString &sdp, const QString &sid);
    void answerReceived(const QString &fromSessionId, const QString &sdp);
```

New:
```cpp
    void offerReceived(const QString &fromSessionId, const QString &sdp, const QString &sid, const QString &roomType);
    void answerReceived(const QString &fromSessionId, const QString &sdp, const QString &roomType);
```

- [ ] **Step 2: Add roomType param to sendSessionMessage, sendOffer, sendAnswer**

In `src/core/SignalingClient.h`, replace the method declarations (lines 47-57):

Old:
```cpp
    void sendOffer(const QString &toSessionId, const QString &sdp,
                   const QString &sid, const QString &nick = {});
    void sendAnswer(const QString &toSessionId, const QString &sdp,
                    const QString &sid, const QString &nick = {});
    void sendCandidate(const QString &toSessionId, const QJsonObject &candidate,
                       const QString &sid);
    void sendEndOfCandidates(const QString &toSessionId, const QString &sid);
    void requestOffer(const QString &sessionId, const QString &roomType = "video");
    void sendSessionMessage(const QString &toSessionId, const QString &type,
                            const QJsonObject &payload, const QString &sid,
                            const QJsonObject &extraData = {});
```

New:
```cpp
    void sendOffer(const QString &toSessionId, const QString &sdp,
                   const QString &sid, const QString &nick = {},
                   const QString &roomType = "video");
    void sendAnswer(const QString &toSessionId, const QString &sdp,
                    const QString &sid, const QString &nick = {},
                    const QString &roomType = "video");
    void sendCandidate(const QString &toSessionId, const QJsonObject &candidate,
                       const QString &sid, const QString &roomType = "video");
    void sendEndOfCandidates(const QString &toSessionId, const QString &sid);
    void requestOffer(const QString &sessionId, const QString &roomType = "video");
    void sendSessionMessage(const QString &toSessionId, const QString &type,
                            const QJsonObject &payload, const QString &sid,
                            const QJsonObject &extraData = {},
                            const QString &roomType = "video");
    void sendBroadcastMessage(const QJsonObject &data);
```

- [ ] **Step 3: Update sendSessionMessage to use roomType param**

In `src/core/SignalingClient.cpp`, change the `sendSessionMessage` function signature (line 393) and replace the hardcoded roomType (line 411):

Update function signature:
```cpp
void SignalingClient::sendSessionMessage(const QString &toSessionId, const QString &type,
                                         const QJsonObject &payload, const QString &sid,
                                         const QJsonObject &extraData, const QString &roomType)
```

Replace line 411:
Old: `data["roomType"] = QString("video");`
New: `data["roomType"] = roomType;`

- [ ] **Step 4: Update sendOffer to pass roomType**

In `src/core/SignalingClient.cpp`, update sendOffer (line 425):

Change signature to:
```cpp
void SignalingClient::sendOffer(const QString &toSessionId, const QString &sdp,
                                const QString &sid, const QString &nick, const QString &roomType)
```

Change the sendSessionMessage call (line 438) to pass roomType:
Old: `sendSessionMessage(toSessionId, "offer", payload, sid, extra);`
New: `sendSessionMessage(toSessionId, "offer", payload, sid, extra, roomType);`

- [ ] **Step 5: Update sendAnswer to pass roomType**

In `src/core/SignalingClient.cpp`, update sendAnswer (line 442):

Change signature to:
```cpp
void SignalingClient::sendAnswer(const QString &toSessionId, const QString &sdp,
                                  const QString &sid, const QString &nick, const QString &roomType)
```

Change the sendSessionMessage call (line 449) to pass roomType:
Old: `sendSessionMessage(toSessionId, "answer", payload, sid);`
New: `sendSessionMessage(toSessionId, "answer", payload, sid, {}, roomType);`

- [ ] **Step 6: Update sendCandidate to pass roomType**

In `src/core/SignalingClient.cpp`, update sendCandidate (line 453):

Change signature to:
```cpp
void SignalingClient::sendCandidate(const QString &toSessionId, const QJsonObject &candidate,
                                     const QString &sid, const QString &roomType)
```

Change the sendSessionMessage call (line 458) to pass roomType:
Old: `sendSessionMessage(toSessionId, "candidate", payload, sid);`
New: `sendSessionMessage(toSessionId, "candidate", payload, sid, {}, roomType);`

- [ ] **Step 7: Parse roomType from incoming messages and emit with signals**

In `src/core/SignalingClient.cpp`, in the message handler (around line 160), update the offer and answer emit calls:

For offer (line 165):
Old: `emit offerReceived(senderSessionId, sdp, sid);`
New:
```cpp
            QString roomType = msgData["roomType"].toString("video");
            emit offerReceived(senderSessionId, sdp, sid, roomType);
```

For answer (line 170-171):
Old: `emit answerReceived(senderSessionId, sdp);`
New:
```cpp
            QString roomType = msgData["roomType"].toString("video");
            emit answerReceived(senderSessionId, sdp, roomType);
```

- [ ] **Step 8: Implement sendBroadcastMessage**

In `src/core/SignalingClient.cpp`, add after `sendEndOfCandidates` (after line 464):

```cpp
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
```

- [ ] **Step 9: Build and verify**

```bash
/c/Qt/Tools/CMake_64/bin/cmake.exe --build /c/build/talq --target talq 2>&1 | tail -10
```

Expected: compile errors in CallManager.cpp because offerReceived/answerReceived signal connections have wrong arity. That's OK — we fix it in Task 4.

Actually, build may fail. To make it compile, we need to update CallManager's signal connections too. Let's do a quick fix: in CallManager.cpp, find the two `connect` calls for `offerReceived` and `answerReceived` and add the extra parameter.

Find `connect(m_signaling, &SignalingClient::offerReceived` and update the lambda to accept the extra `roomType` param (just ignore it for now):

Old pattern: `this, &CallManager::onOfferReceived`
New: `this, [this](const QString &from, const QString &sdp, const QString &sid, const QString &roomType) { Q_UNUSED(roomType); onOfferReceived(from, sdp, sid); }`

Find `connect(m_signaling, &SignalingClient::answerReceived` and update:
Old pattern: `this, &CallManager::onAnswerReceived`
New: `this, [this](const QString &from, const QString &sdp, const QString &roomType) { Q_UNUSED(roomType); onAnswerReceived(from, sdp); }`

Now build should succeed.

- [ ] **Step 10: Commit**

```bash
cd /c/src/talk-desktop-qt
git add src/core/SignalingClient.h src/core/SignalingClient.cpp src/core/CallManager.cpp
git commit -m "feat(signaling): add roomType parameter to signaling methods and signals"
```

---

### Task 2: Create ScreenSharePipeline

**Files:**
- Create: `src/core/ScreenSharePipeline.h`
- Create: `src/core/ScreenSharePipeline.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Create ScreenSharePipeline.h**

Create `src/core/ScreenSharePipeline.h`:

```cpp
#pragma once

#include <QObject>
#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>
#include "SignalingClient.h"

/**
 * Send-only screen capture pipeline for MCU screen sharing.
 * Captures the primary monitor via d3d11screencapturesrc, encodes as VP8,
 * sends via RTP to the MCU on a separate webrtcbin (roomType "screen").
 * No audio. No data channels needed beyond the required "status" channel.
 */
class ScreenSharePipeline : public QObject
{
    Q_OBJECT

public:
    explicit ScreenSharePipeline(QObject *parent = nullptr);
    ~ScreenSharePipeline() override;

    bool start(const QString &stunServer, const QList<TurnServer> &turnServers = {});
    void stop();
    void setRemoteAnswer(const QString &sdp);
    void addIceCandidate(const QString &candidate, int sdpMLineIndex, const QString &sdpMid);
    bool isRunning() const { return m_running; }

signals:
    void localOfferReady(const QString &sdp);
    void iceCandidateReady(const QString &candidate, int sdpMLineIndex, const QString &sdpMid);
    void iceStateChanged(const QString &state);
    void error(const QString &message);

public slots:
    void pollBus();

private:
    void cleanup();

    GstElement *m_pipeline = nullptr;
    GstElement *m_webrtcbin = nullptr;
    bool m_running = false;
    bool m_remoteDescSet = false;
    QList<QPair<int, QString>> m_pendingCandidates;

    static void onNegotiationNeeded(GstElement *webrtc, gpointer userData);
    static void onIceCandidate(GstElement *webrtc, guint mlineIndex, gchar *candidate, gpointer userData);
    static void onOfferCreated(GstPromise *promise, gpointer userData);
    static void onIceStateChanged(GObject *obj, GParamSpec *pspec, gpointer userData);
};
```

- [ ] **Step 2: Create ScreenSharePipeline.cpp**

Create `src/core/ScreenSharePipeline.cpp`:

```cpp
#include "core/ScreenSharePipeline.h"
#include <QDebug>
#include <QPointer>
#include <QRegularExpression>
#include <QUrl>

ScreenSharePipeline::ScreenSharePipeline(QObject *parent)
    : QObject(parent)
{
}

ScreenSharePipeline::~ScreenSharePipeline()
{
    stop();
}

bool ScreenSharePipeline::start(const QString &stunServer, const QList<TurnServer> &turnServers)
{
    if (m_running) return false;

    m_pipeline = gst_pipeline_new(nullptr);
    m_webrtcbin = gst_element_factory_make("webrtcbin", nullptr);

    if (!m_pipeline || !m_webrtcbin) {
        emit error("Failed to create screen share pipeline elements");
        cleanup();
        return false;
    }

    if (!stunServer.isEmpty()) {
        QString gstStun = stunServer;
        if (gstStun.startsWith("stun:") && !gstStun.startsWith("stun://"))
            gstStun.replace("stun:", "stun://");
        g_object_set(m_webrtcbin, "stun-server", gstStun.toUtf8().constData(), nullptr);
    }
    g_object_set(m_webrtcbin, "bundle-policy",
                 GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE, nullptr);

    for (const auto &turn : turnServers) {
        for (const auto &url : turn.urls) {
            QString gstUrl = url;
            gstUrl.remove(QRegularExpression("\\?transport=.*$"));
            if (gstUrl.startsWith("turn:") && !gstUrl.startsWith("turn://"))
                gstUrl.replace("turn:", "turn://");
            if (gstUrl.startsWith("turns:") && !gstUrl.startsWith("turns://"))
                gstUrl.replace("turns:", "turns://");
            QString escapedUser = QString(QUrl::toPercentEncoding(turn.username));
            QString escapedCred = QString(QUrl::toPercentEncoding(turn.credential));
            gstUrl.replace("://", QString("://%1:%2@").arg(escapedUser, escapedCred));
            gboolean ret = FALSE;
            g_signal_emit_by_name(m_webrtcbin, "add-turn-server", gstUrl.toUtf8().constData(), &ret);
        }
    }

    // Screen capture source
    GstElement *screenSrc = gst_element_factory_make("d3d11screencapturesrc", nullptr);
    if (!screenSrc) {
        emit error("d3d11screencapturesrc not available");
        cleanup();
        return false;
    }
    g_object_set(screenSrc, "monitor-index", -1, "show-cursor", TRUE, nullptr);

    GstElement *videoRate = gst_element_factory_make("videorate", nullptr);
    GstElement *rateCaps = gst_element_factory_make("capsfilter", nullptr);
    GstElement *videoConvert = gst_element_factory_make("videoconvert", nullptr);
    GstElement *videoScale = gst_element_factory_make("videoscale", nullptr);
    GstElement *scaleCaps = gst_element_factory_make("capsfilter", nullptr);
    GstElement *vp8enc = gst_element_factory_make("vp8enc", nullptr);
    GstElement *rtpvp8pay = gst_element_factory_make("rtpvp8pay", nullptr);
    GstElement *ssrcFilter = gst_element_factory_make("capsfilter", nullptr);

    if (!videoRate || !rateCaps || !videoConvert || !videoScale || !scaleCaps
        || !vp8enc || !rtpvp8pay || !ssrcFilter) {
        emit error("Failed to create screen share encoding elements");
        cleanup();
        return false;
    }

    // Cap framerate to 30fps
    {
        GstCaps *rc = gst_caps_from_string("video/x-raw,framerate=30/1");
        g_object_set(rateCaps, "caps", rc, nullptr);
        gst_caps_unref(rc);
    }

    // Cap resolution to 1080p
    {
        GstCaps *sc = gst_caps_from_string("video/x-raw,width=[1,1920],height=[1,1080]");
        g_object_set(scaleCaps, "caps", sc, nullptr);
        gst_caps_unref(sc);
    }

    // Encoder settings
    g_object_set(vp8enc, "deadline", (gint64)1, "threads", 4,
                 "target-bitrate", 2000000, nullptr);

    // SSRC
    guint32 videoSsrc = g_random_int();
    g_object_set(rtpvp8pay, "ssrc", videoSsrc, nullptr);
    {
        GstCaps *ssrcCaps = gst_caps_from_string("application/x-rtp");
        gst_caps_set_simple(ssrcCaps, "ssrc", G_TYPE_UINT, videoSsrc, nullptr);
        g_object_set(ssrcFilter, "caps", ssrcCaps, nullptr);
        gst_caps_unref(ssrcCaps);
    }

    gst_bin_add_many(GST_BIN(m_pipeline), screenSrc, videoRate, rateCaps,
                     videoConvert, videoScale, scaleCaps,
                     vp8enc, rtpvp8pay, ssrcFilter, m_webrtcbin, nullptr);

    if (!gst_element_link_many(screenSrc, videoRate, rateCaps, videoConvert,
                               videoScale, scaleCaps, vp8enc, rtpvp8pay,
                               ssrcFilter, nullptr)) {
        emit error("Failed to link screen share chain");
        cleanup();
        return false;
    }

    // Link to webrtcbin
    GstPad *ssrcSrcPad = gst_element_get_static_pad(ssrcFilter, "src");
    GstPad *sinkPad = gst_element_request_pad_simple(m_webrtcbin, "sink_%u");
    gst_pad_link(ssrcSrcPad, sinkPad);

    // Set transceiver to sendonly VP8
    GstWebRTCRTPTransceiver *vt = nullptr;
    g_object_get(sinkPad, "transceiver", &vt, nullptr);
    if (vt) {
        GstCaps *vc = gst_caps_from_string(
            "application/x-rtp,media=video,encoding-name=VP8,clock-rate=90000,payload=96");
        g_object_set(vt, "direction", GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY,
                     "codec-preferences", vc, nullptr);
        gst_caps_unref(vc);
        gst_object_unref(vt);
    }
    gst_object_unref(ssrcSrcPad);
    gst_object_unref(sinkPad);

    // Data channel — Janus requires at least one for publisher registration
    {
        GstWebRTCDataChannel *dc = nullptr;
        g_signal_emit_by_name(m_webrtcbin, "create-data-channel", "status", nullptr, &dc);
        if (dc) {
            qDebug() << "ScreenSharePipeline: created data channel 'status'";
            g_object_unref(dc);
        }
    }

    // Signals
    g_signal_connect(m_webrtcbin, "on-negotiation-needed",
                     G_CALLBACK(onNegotiationNeeded), this);
    g_signal_connect(m_webrtcbin, "on-ice-candidate",
                     G_CALLBACK(onIceCandidate), this);
    g_signal_connect(m_webrtcbin, "notify::ice-connection-state",
                     G_CALLBACK(onIceStateChanged), this);

    GstStateChangeReturn ret = gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        emit error("Failed to start screen share pipeline");
        cleanup();
        return false;
    }

    m_running = true;
    qDebug() << "ScreenSharePipeline: started, capturing primary monitor";
    return true;
}

void ScreenSharePipeline::stop()
{
    if (!m_running) return;
    cleanup();
    m_running = false;
    qDebug() << "ScreenSharePipeline: stopped";
}

void ScreenSharePipeline::cleanup()
{
    if (m_webrtcbin)
        g_signal_handlers_disconnect_by_data(m_webrtcbin, this);
    if (m_pipeline) {
        gst_element_set_state(m_pipeline, GST_STATE_NULL);
        gst_object_unref(m_pipeline);
        m_pipeline = nullptr;
    }
    m_webrtcbin = nullptr;
    m_remoteDescSet = false;
    m_pendingCandidates.clear();
}

void ScreenSharePipeline::setRemoteAnswer(const QString &sdp)
{
    if (!m_webrtcbin) return;

    QByteArray sdpUtf8 = sdp.toUtf8();
    GstSDPMessage *sdpMsg;
    gst_sdp_message_new(&sdpMsg);
    gst_sdp_message_parse_buffer((const guint8 *)sdpUtf8.constData(), sdpUtf8.size(), sdpMsg);

    GstWebRTCSessionDescription *desc = gst_webrtc_session_description_new(
        GST_WEBRTC_SDP_TYPE_ANSWER, sdpMsg);
    g_signal_emit_by_name(m_webrtcbin, "set-remote-description", desc, nullptr);
    gst_webrtc_session_description_free(desc);

    m_remoteDescSet = true;
    for (const auto &c : m_pendingCandidates)
        g_signal_emit_by_name(m_webrtcbin, "add-ice-candidate", c.first, c.second.toUtf8().constData());
    m_pendingCandidates.clear();
    qDebug() << "ScreenSharePipeline: set remote answer";
}

void ScreenSharePipeline::addIceCandidate(const QString &candidate, int sdpMLineIndex, const QString &sdpMid)
{
    Q_UNUSED(sdpMid)
    if (!m_webrtcbin) return;
    if (!m_remoteDescSet) {
        m_pendingCandidates.append({sdpMLineIndex, candidate});
        return;
    }
    g_signal_emit_by_name(m_webrtcbin, "add-ice-candidate",
                          sdpMLineIndex, candidate.toUtf8().constData());
}

void ScreenSharePipeline::pollBus()
{
    if (!m_pipeline) return;
    GstBus *bus = gst_element_get_bus(m_pipeline);
    GstMessage *msg;
    while ((msg = gst_bus_pop(bus)) != nullptr) {
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            GError *err = nullptr; gchar *dbg = nullptr;
            gst_message_parse_error(msg, &err, &dbg);
            qWarning() << "ScreenSharePipeline ERROR:" << err->message;
            g_clear_error(&err); g_free(dbg);
        }
        gst_message_unref(msg);
    }
    gst_object_unref(bus);
}

// --- Static callbacks ---

void ScreenSharePipeline::onNegotiationNeeded(GstElement *webrtc, gpointer userData)
{
    auto *self = static_cast<ScreenSharePipeline *>(userData);
    GstPromise *promise = gst_promise_new_with_change_func(onOfferCreated, self, nullptr);
    g_signal_emit_by_name(webrtc, "create-offer", nullptr, promise);
}

void ScreenSharePipeline::onOfferCreated(GstPromise *promise, gpointer userData)
{
    auto *self = static_cast<ScreenSharePipeline *>(userData);
    const GstStructure *reply = gst_promise_get_reply(promise);
    GstWebRTCSessionDescription *offer = nullptr;
    gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, nullptr);

    if (!offer || !self->m_webrtcbin) {
        if (offer) gst_webrtc_session_description_free(offer);
        gst_promise_unref(promise);
        return;
    }

    g_signal_emit_by_name(self->m_webrtcbin, "set-local-description", offer, nullptr);

    gchar *sdpText = gst_sdp_message_as_text(offer->sdp);
    QString sdp = QString::fromUtf8(sdpText);
    g_free(sdpText);
    gst_webrtc_session_description_free(offer);
    gst_promise_unref(promise);

    QPointer<ScreenSharePipeline> guard(self);
    QMetaObject::invokeMethod(self, [guard, sdp]() {
        if (!guard) return;
        emit guard->localOfferReady(sdp);
    }, Qt::QueuedConnection);
}

void ScreenSharePipeline::onIceCandidate(GstElement *, guint mlineIndex, gchar *candidate, gpointer userData)
{
    auto *self = static_cast<ScreenSharePipeline *>(userData);
    QString c = QString::fromUtf8(candidate);
    int ml = static_cast<int>(mlineIndex);
    QPointer<ScreenSharePipeline> guard(self);
    QMetaObject::invokeMethod(self, [guard, c, ml]() {
        if (!guard) return;
        emit guard->iceCandidateReady(c, ml, QString("0"));
    }, Qt::QueuedConnection);
}

void ScreenSharePipeline::onIceStateChanged(GObject *obj, GParamSpec *, gpointer userData)
{
    auto *self = static_cast<ScreenSharePipeline *>(userData);
    GstWebRTCICEConnectionState state;
    g_object_get(obj, "ice-connection-state", &state, nullptr);
    const char *names[] = {"new", "checking", "connected", "completed", "failed", "disconnected", "closed"};
    int idx = static_cast<int>(state);
    QString stateName = (idx < 7) ? names[idx] : "unknown";
    QPointer<ScreenSharePipeline> guard(self);
    QMetaObject::invokeMethod(self, [guard, stateName]() {
        if (!guard) return;
        emit guard->iceStateChanged(stateName);
    }, Qt::QueuedConnection);
}
```

- [ ] **Step 3: Add to CMakeLists.txt**

In `CMakeLists.txt`, add after `src/core/PeerPipeline.cpp` (around line 50):

```
    src/core/ScreenSharePipeline.cpp
```

Add the same line in the second source list (around line 124) if there is a duplicate list.

- [ ] **Step 4: Build and verify**

```bash
cmd.exe //c "taskkill /IM talq.exe /F" 2>/dev/null
/c/Qt/Tools/CMake_64/bin/cmake.exe --build /c/build/talq --target talq 2>&1 | tail -10
```

Expected: successful build.

- [ ] **Step 5: Commit**

```bash
cd /c/src/talk-desktop-qt
git add src/core/ScreenSharePipeline.h src/core/ScreenSharePipeline.cpp CMakeLists.txt
git commit -m "feat(screenshare): add ScreenSharePipeline for d3d11 screen capture"
```

---

### Task 3: CallManager — screen share send orchestration

**Files:**
- Modify: `src/core/CallManager.h`
- Modify: `src/core/CallManager.cpp`

- [ ] **Step 1: Add members, signals, and method to CallManager.h**

Add include at top of CallManager.h:
```cpp
#include "core/ScreenSharePipeline.h"
```

Add public method after `toggleCamera()`:
```cpp
    Q_INVOKABLE void toggleScreenShare();
    bool isScreenSharing() const { return m_screenSharing; }
```

Add signal after `remoteMediaChanged`:
```cpp
    void screenShareChanged();
    void remoteScreenProviderChanged();
```

Add private members after `m_localVideoProvider`:
```cpp
    // Screen sharing
    ScreenSharePipeline *m_screenSharePipeline = nullptr;
    bool m_screenSharing = false;
    QString m_screenShareSid;
    VideoFrameProvider *m_remoteScreenProvider = nullptr;
    QHash<QString, SubscribePipeline*> m_screenSubscribers;
```

- [ ] **Step 2: Implement toggleScreenShare()**

In `src/core/CallManager.cpp`, add after `toggleCamera()`:

```cpp
void CallManager::toggleScreenShare()
{
    if (m_state != Active && m_state != Connecting) return;

    m_screenSharing = !m_screenSharing;

    if (m_screenSharing) {
        m_screenSharePipeline = new ScreenSharePipeline(this);

        connect(m_screenSharePipeline, &ScreenSharePipeline::localOfferReady,
                this, [this](const QString &sdp) {
            // Send offer to ourselves (MCU mode: publisher sends to own session)
            m_screenShareSid = QString::number(qHash(sdp)).left(10);
            m_signaling->sendOffer(m_signaling->sessionId(), sdp, m_screenShareSid, {}, "screen");
            qDebug() << "CallManager: sent screen share offer, sid=" << m_screenShareSid;
        });

        connect(m_screenSharePipeline, &ScreenSharePipeline::iceCandidateReady,
                this, [this](const QString &candidate, int mline, const QString &mid) {
            QJsonObject c;
            c["candidate"] = candidate;
            c["sdpMLineIndex"] = mline;
            c["sdpMid"] = mid;
            m_signaling->sendCandidate(m_signaling->sessionId(), c, m_screenShareSid, "screen");
        });

        connect(m_screenSharePipeline, &ScreenSharePipeline::iceStateChanged,
                this, [this](const QString &state) {
            qDebug() << "CallManager: screen share ICE:" << state;
            if (state == "failed") {
                qWarning() << "CallManager: screen share ICE failed";
                m_screenSharing = false;
                m_screenSharePipeline->stop();
                m_screenSharePipeline->deleteLater();
                m_screenSharePipeline = nullptr;
                emit screenShareChanged();
            }
        });

        connect(m_screenSharePipeline, &ScreenSharePipeline::error,
                this, [this](const QString &msg) {
            qWarning() << "CallManager: screen share error:" << msg;
            m_screenSharing = false;
            m_screenSharePipeline->stop();
            m_screenSharePipeline->deleteLater();
            m_screenSharePipeline = nullptr;
            emit screenShareChanged();
        });

        // Connect pollBus to the existing GLib timer
        connect(&m_glibTimer, &QTimer::timeout, m_screenSharePipeline, &ScreenSharePipeline::pollBus);

        if (!m_screenSharePipeline->start(m_stunServer, m_turnServers)) {
            qWarning() << "CallManager: failed to start screen share pipeline";
            m_screenSharing = false;
            m_screenSharePipeline->deleteLater();
            m_screenSharePipeline = nullptr;
        }
    } else {
        // Stop sharing
        if (m_screenSharePipeline) {
            m_screenSharePipeline->stop();
            m_screenSharePipeline->deleteLater();
            m_screenSharePipeline = nullptr;
        }
        // Broadcast unshareScreen
        QJsonObject data;
        data["roomType"] = QString("screen");
        data["type"] = QString("unshareScreen");
        m_signaling->sendBroadcastMessage(data);
        qDebug() << "CallManager: stopped screen sharing";
    }

    emit screenShareChanged();
}
```

- [ ] **Step 3: Route screen share signaling in offerReceived/answerReceived**

In `src/core/CallManager.cpp`, find the `connect(m_signaling, &SignalingClient::offerReceived` call (which we updated in Task 1 to a lambda). Replace it:

```cpp
    connect(m_signaling, &SignalingClient::offerReceived,
            this, [this](const QString &from, const QString &sdp, const QString &sid, const QString &roomType) {
        if (roomType == "screen") {
            // Incoming screen share — create a subscriber for it
            qDebug() << "CallManager: received screen share offer from" << from.left(20);
            if (m_screenSubscribers.contains(from)) {
                m_screenSubscribers[from]->setRemoteOffer(sdp);
                return;
            }
            auto *sub = new SubscribePipeline(from, this);
            connect(sub, &SubscribePipeline::localAnswerReady,
                    this, [this, from, sid](const QString &answerSdp) {
                m_signaling->sendAnswer(from, answerSdp, sid, {}, "screen");
            });
            connect(sub, &SubscribePipeline::iceCandidateReady,
                    this, [this, from, sid](const QString &candidate, int mline, const QString &mid) {
                QJsonObject c;
                c["candidate"] = candidate;
                c["sdpMLineIndex"] = mline;
                c["sdpMid"] = mid;
                m_signaling->sendCandidate(from, c, sid, "screen");
            });
            connect(sub, &SubscribePipeline::iceStateChanged,
                    this, [this](const QString &state) {
                qDebug() << "CallManager: screen subscriber ICE:" << state;
            });
            m_screenSubscribers[from] = sub;
            if (!sub->start(m_stunServer, m_turnServers)) {
                m_screenSubscribers.remove(from);
                sub->deleteLater();
                return;
            }
            m_remoteScreenProvider = sub->videoProvider();
            emit remoteScreenProviderChanged();
            sub->setRemoteOffer(sdp);
            return;
        }
        onOfferReceived(from, sdp, sid);
    });
```

Update the answerReceived connection similarly:

```cpp
    connect(m_signaling, &SignalingClient::answerReceived,
            this, [this](const QString &from, const QString &sdp, const QString &roomType) {
        if (roomType == "screen" && m_screenSharePipeline) {
            m_screenSharePipeline->setRemoteAnswer(sdp);
            qDebug() << "CallManager: set screen share answer";
            return;
        }
        onAnswerReceived(from, sdp);
    });
```

- [ ] **Step 4: Clean up screen share in teardown()**

In `src/core/CallManager.cpp`, find `teardown()`. Add before the `setState(Idle)` call:

```cpp
    // Clean up screen sharing
    if (m_screenSharePipeline) {
        m_screenSharePipeline->stop();
        m_screenSharePipeline->deleteLater();
        m_screenSharePipeline = nullptr;
    }
    m_screenSharing = false;
    for (auto *sub : m_screenSubscribers)
        sub->deleteLater();
    m_screenSubscribers.clear();
    m_remoteScreenProvider = nullptr;
```

- [ ] **Step 5: Build and verify**

```bash
cmd.exe //c "taskkill /IM talq.exe /F" 2>/dev/null
/c/Qt/Tools/CMake_64/bin/cmake.exe --build /c/build/talq --target talq 2>&1 | tail -10
```

- [ ] **Step 6: Commit**

```bash
cd /c/src/talk-desktop-qt
git add src/core/CallManager.h src/core/CallManager.cpp
git commit -m "feat(screenshare): CallManager screen share orchestration and signaling routing"
```

---

### Task 4: CallDialog — screen share button and remote screen display

**Files:**
- Modify: `src/ui/CallDialog.h`
- Modify: `src/ui/CallDialog.cpp`

- [ ] **Step 1: Add member to CallDialog.h**

In `src/ui/CallDialog.h`, add with the other button members:

```cpp
    QPushButton *m_shareBtn = nullptr;
```

- [ ] **Step 2: Add screen share button in buildUi()**

In `src/ui/CallDialog.cpp` `buildUi()`, after the camera button block (after `activeLayout->addWidget(m_cameraBtn);`, around line 180), add:

```cpp
    m_shareBtn = new QPushButton(m_activeRow);
    m_shareBtn->setCursor(Qt::PointingHandCursor);
    m_shareBtn->setToolTip("Share screen");
    m_shareBtn->setStyleSheet(circleButtonStyle("#3a3a36", "#e4e0da", "#4a4a46"));
    m_shareBtn->setText("\xF0\x9F\x96\xA5");  // desktop monitor emoji
    activeLayout->addWidget(m_shareBtn);
```

- [ ] **Step 3: Connect button and signals**

In `src/ui/CallDialog.cpp` `buildUi()`, in the button connections section (after `connect(m_cameraBtn, ...)`), add:

```cpp
    connect(m_shareBtn, &QPushButton::clicked, m_callManager, &CallManager::toggleScreenShare);
```

In the constructor (after the existing signal connections), add:

```cpp
    connect(m_callManager, &CallManager::screenShareChanged, this, [this]() {
        if (m_callManager->isScreenSharing()) {
            m_shareBtn->setStyleSheet(circleButtonStyle("#2ec4b6", "white", "#3ed4c6"));
        } else {
            m_shareBtn->setStyleSheet(circleButtonStyle("#3a3a36", "#e4e0da", "#4a4a46"));
        }
    });
    connect(m_callManager, &CallManager::remoteScreenProviderChanged, this, [this]() {
        auto *provider = m_callManager->remoteScreenProvider();
        if (provider) {
            connect(provider, &VideoFrameProvider::imageReady, this, [this](const QImage &img) {
                if (img.width() > 32 && img.height() > 32) {
                    if (!m_remoteVideo->isVisible()) {
                        m_remoteVideo->show();
                        setMinimumSize(600, 450);
                        resize(800, 600);
                    }
                    m_remoteVideo->setImage(img);
                }
            });
        }
    });
```

- [ ] **Step 4: Add remoteScreenProvider accessor to CallManager.h**

In `src/core/CallManager.h`, add public accessor:

```cpp
    VideoFrameProvider *remoteScreenProvider() const { return m_remoteScreenProvider; }
```

- [ ] **Step 5: Build and verify**

```bash
cmd.exe //c "taskkill /IM talq.exe /F" 2>/dev/null
/c/Qt/Tools/CMake_64/bin/cmake.exe --build /c/build/talq --target talq 2>&1 | tail -10
```

- [ ] **Step 6: Commit**

```bash
cd /c/src/talk-desktop-qt
git add src/ui/CallDialog.h src/ui/CallDialog.cpp src/core/CallManager.h
git commit -m "feat(screenshare): share button in CallDialog and remote screen display"
```

---

### Task 5: Smoke test — deploy and verify

- [ ] **Step 1: Deploy**

```bash
cmd.exe //c "taskkill /IM talq.exe /F" 2>/dev/null
/c/Qt/Tools/CMake_64/bin/cmake.exe --build /c/build/talq --target talq 2>&1 | tail -5
cd /c/build/talq && bash /c/src/talk-desktop-qt/scripts/deploy-dev.sh
```

- [ ] **Step 2: Manual test checklist**

1. Start a call TalQ→browser
2. Click the share button (monitor icon) in TalQ → button turns teal
3. Browser should show TalQ's screen share
4. Click share button again → stops sharing, button returns to default
5. In browser: share screen → TalQ should display the browser's screen share
6. Hang up during screen share → clean teardown, no crash
7. Check logs for: `ScreenSharePipeline: started`, `screen share ICE: connected`

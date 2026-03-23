# Video Calls: Local Preview, Renegotiation & P2P Support

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix video calls so camera preview shows locally, outgoing video reaches the remote peer via renegotiation, and add P2P call support for 1:1 calls alongside existing MCU mode.

**Architecture:** Add a `tee` element in the camera capture chain to split video to both the WebRTC encoder and a local preview appsink. Fix mid-call renegotiation by manually triggering `create-offer` after adding video track. Add a new `PeerPipeline` class for P2P 1:1 calls that uses a single webrtcbin for both sending and receiving. CallManager selects MCU vs P2P based on `SignalingClient::hasMcu()` and participant count.

**Tech Stack:** GStreamer 1.x (webrtcbin, tee, appsink), Qt 6.8 (QVideoSink, QML VideoOutput), C++17

---

## File Structure

| File | Action | Responsibility |
|------|--------|---------------|
| `src/core/PublishPipeline.h` | Modify | Add `localVideoProvider()`, tee + preview appsink, `renegotiate()` signal |
| `src/core/PublishPipeline.cpp` | Modify | Tee camera to preview appsink, emit renegotiation after enableCamera |
| `src/core/PeerPipeline.h` | Create | Single webrtcbin P2P pipeline: send audio+video, receive audio+video |
| `src/core/PeerPipeline.cpp` | Create | Full P2P implementation with renegotiation support |
| `src/core/CallManager.h` | Modify | Add `localVideoProvider`, `m_peerPipeline`, P2P call flow methods |
| `src/core/CallManager.cpp` | Modify | P2P vs MCU routing, local preview wiring, renegotiation handling |
| `src/qml/CallWindow.qml` | Modify | Add PIP VideoOutput for local preview, fix anchor error on line 103 |
| `CMakeLists.txt` | Modify | Add PeerPipeline.cpp to sources |

---

## Task 1: Add local camera preview to PublishPipeline (MCU mode)

**Files:**
- Modify: `src/core/PublishPipeline.h`
- Modify: `src/core/PublishPipeline.cpp`

- [ ] **Step 1: Add preview members to PublishPipeline.h**

Add after the existing video element members (line 61):

```cpp
// Local preview
GstElement *m_tee = nullptr;
GstElement *m_previewQueue = nullptr;
GstElement *m_previewConvert = nullptr;
GstElement *m_previewAppsink = nullptr;
VideoFrameProvider *m_localVideoProvider = nullptr;
```

Add public accessor and include (after line 7):

```cpp
#include "VideoFrameProvider.h"
```

Add to public section (after line 33):

```cpp
VideoFrameProvider *localVideoProvider() const { return m_localVideoProvider; }
```

Add static callback declaration (after line 67):

```cpp
static GstFlowReturn onPreviewSample(GstAppSink *sink, gpointer userData);
```

- [ ] **Step 2: Create VideoFrameProvider in constructor**

In `PublishPipeline.cpp` constructor (line 8), add:

```cpp
m_localVideoProvider = new VideoFrameProvider(this);
```

- [ ] **Step 3: Modify enableCamera to use tee for preview**

Replace the camera pipeline construction in `enableCamera()`. After creating the encoder and payloader (line 193), add tee and preview branch:

```cpp
m_tee = gst_element_factory_make("tee", "camera-tee");
m_previewQueue = gst_element_factory_make("queue", "preview-queue");
m_previewConvert = gst_element_factory_make("videoconvert", "preview-convert");
m_previewAppsink = gst_element_factory_make("appsink", "preview-sink");

if (!m_tee || !m_previewQueue || !m_previewConvert || !m_previewAppsink) {
    qWarning() << "PublishPipeline: failed to create preview elements";
    // Continue without preview — video still works
} else {
    GstCaps *previewCaps = gst_caps_from_string("video/x-raw,format=I420");
    g_object_set(m_previewAppsink,
        "emit-signals", TRUE,
        "caps", previewCaps,
        "drop", TRUE,
        "max-buffers", 1,
        nullptr);
    gst_caps_unref(previewCaps);
    g_signal_connect(m_previewAppsink, "new-sample",
        G_CALLBACK(onPreviewSample), this);
}
```

Modify the pipeline construction to insert tee after videoconvert. The chain becomes:

**JPEG path:** `ksvideosrc → capsfilter → jpegdec → videoconvert → tee → [queue → encoder → payloader] + [preview-queue → preview-convert → preview-appsink]`

Replace the existing bin_add and link code (lines 228-238) with:

```cpp
GstElement *encQueue = gst_element_factory_make("queue", "enc-queue");

if (jpegdec) {
    if (m_tee) {
        gst_bin_add_many(GST_BIN(m_pipeline), m_cameraSrc, m_videoCapsFilter, jpegdec,
            m_videoConvert, m_tee, encQueue, m_videoEncoder, m_videoPayloader,
            m_previewQueue, m_previewConvert, m_previewAppsink, nullptr);
        linked = gst_element_link_many(m_cameraSrc, m_videoCapsFilter, jpegdec, m_videoConvert, m_tee, nullptr)
              && gst_element_link_many(encQueue, m_videoEncoder, m_videoPayloader, nullptr)
              && gst_element_link_many(m_previewQueue, m_previewConvert, m_previewAppsink, nullptr);
        // Link tee pads manually
        if (linked) {
            GstPad *teeSrc1 = gst_element_request_pad_simple(m_tee, "src_%u");
            GstPad *encSink = gst_element_get_static_pad(encQueue, "sink");
            linked = (gst_pad_link(teeSrc1, encSink) == GST_PAD_LINK_OK);
            gst_object_unref(teeSrc1);
            gst_object_unref(encSink);
        }
        if (linked) {
            GstPad *teeSrc2 = gst_element_request_pad_simple(m_tee, "src_%u");
            GstPad *prevSink = gst_element_get_static_pad(m_previewQueue, "sink");
            linked = (gst_pad_link(teeSrc2, prevSink) == GST_PAD_LINK_OK);
            gst_object_unref(teeSrc2);
            gst_object_unref(prevSink);
        }
    } else {
        gst_bin_add_many(GST_BIN(m_pipeline), m_cameraSrc, m_videoCapsFilter, jpegdec,
            m_videoConvert, m_videoEncoder, m_videoPayloader, nullptr);
        linked = gst_element_link_many(m_cameraSrc, m_videoCapsFilter, jpegdec,
            m_videoConvert, m_videoEncoder, m_videoPayloader, nullptr);
    }
} else {
    // Raw capture fallback (no jpegdec) — same pattern with/without tee
    if (m_tee) {
        gst_bin_add_many(GST_BIN(m_pipeline), m_cameraSrc, m_videoConvert, m_videoCapsFilter,
            m_tee, encQueue, m_videoEncoder, m_videoPayloader,
            m_previewQueue, m_previewConvert, m_previewAppsink, nullptr);
        linked = gst_element_link_many(m_cameraSrc, m_videoConvert, m_videoCapsFilter, m_tee, nullptr)
              && gst_element_link_many(encQueue, m_videoEncoder, m_videoPayloader, nullptr)
              && gst_element_link_many(m_previewQueue, m_previewConvert, m_previewAppsink, nullptr);
        if (linked) {
            GstPad *teeSrc1 = gst_element_request_pad_simple(m_tee, "src_%u");
            GstPad *encSink = gst_element_get_static_pad(encQueue, "sink");
            linked = (gst_pad_link(teeSrc1, encSink) == GST_PAD_LINK_OK);
            gst_object_unref(teeSrc1);
            gst_object_unref(encSink);
        }
        if (linked) {
            GstPad *teeSrc2 = gst_element_request_pad_simple(m_tee, "src_%u");
            GstPad *prevSink = gst_element_get_static_pad(m_previewQueue, "sink");
            linked = (gst_pad_link(teeSrc2, prevSink) == GST_PAD_LINK_OK);
            gst_object_unref(teeSrc2);
            gst_object_unref(prevSink);
        }
    } else {
        gst_bin_add_many(GST_BIN(m_pipeline), m_cameraSrc, m_videoConvert, m_videoCapsFilter,
            m_videoEncoder, m_videoPayloader, nullptr);
        linked = gst_element_link_many(m_cameraSrc, m_videoConvert, m_videoCapsFilter,
            m_videoEncoder, m_videoPayloader, nullptr);
    }
}
```

Also sync state for new elements (after line 264):

```cpp
if (m_tee) gst_element_sync_state_with_parent(m_tee);
if (m_previewQueue) gst_element_sync_state_with_parent(m_previewQueue);
if (m_previewConvert) gst_element_sync_state_with_parent(m_previewConvert);
if (m_previewAppsink) gst_element_sync_state_with_parent(m_previewAppsink);
```

- [ ] **Step 4: Add preview sample callback**

Add at end of file:

```cpp
GstFlowReturn PublishPipeline::onPreviewSample(GstAppSink *sink, gpointer userData)
{
    auto *self = static_cast<PublishPipeline *>(userData);
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_OK;

    QMetaObject::invokeMethod(self->m_localVideoProvider, [self, sample]() {
        self->m_localVideoProvider->feedFrame(sample);
        gst_sample_unref(sample);
    }, Qt::QueuedConnection);

    return GST_FLOW_OK;
}
```

- [ ] **Step 5: Update disableCamera to clean up preview elements**

In `disableCamera()` (after line 301), add cleanup for new elements:

```cpp
removeElement(m_previewAppsink);
removeElement(m_previewConvert);
removeElement(m_previewQueue);
removeElement(m_tee);
```

Also add the encQueue element tracking — store as member `GstElement *m_encQueue = nullptr;` in the header, and `removeElement(m_encQueue);` in disableCamera.

- [ ] **Step 6: Build and test camera preview**

```bash
export PATH="/c/msys64/mingw64/bin:/c/Qt/Tools/CMake_64/bin:/c/Qt/Tools/Ninja:/c/Qt/6.8.2/mingw_64/bin:/c/Qt/Tools/mingw1310_64/bin:$PATH"
cd /c/build/talq && cmake --build . --target talq 2>&1 | tail -5
```

Expected: Compiles without errors.

- [ ] **Step 7: Commit**

```bash
git add src/core/PublishPipeline.h src/core/PublishPipeline.cpp
git commit -m "feat: add local camera preview via tee + appsink in PublishPipeline"
```

---

## Task 2: Fix mid-call video renegotiation (MCU mode)

**Files:**
- Modify: `src/core/PublishPipeline.h`
- Modify: `src/core/PublishPipeline.cpp`
- Modify: `src/core/CallManager.cpp`

- [ ] **Step 1: Add renegotiation signal to PublishPipeline**

In `PublishPipeline.h`, add signal (after line 42):

```cpp
void renegotiationNeeded();
```

- [ ] **Step 2: Emit renegotiation after enableCamera**

In `PublishPipeline::enableCamera()`, at the end (after setting `m_cameraEnabled = true`), add:

```cpp
// Manually trigger renegotiation — webrtcbin doesn't always fire
// on-negotiation-needed when pads are added mid-call
qDebug() << "PublishPipeline: triggering renegotiation for video";
GstPromise *promise = gst_promise_new_with_change_func(onOfferCreated, this, nullptr);
g_signal_emit_by_name(m_webrtcbin, "create-offer", nullptr, promise);
```

- [ ] **Step 3: Wire renegotiation in CallManager**

In `CallManager::joinCallOnServer()`, after connecting `PublishPipeline::localOfferReady` (around line 428), the existing connection already handles sending new offers. But we need to make sure the MCU answer for renegotiation is handled.

Check: `onAnswerReceived` at line 644 already routes to `m_publishPipeline->setRemoteAnswer(sdp)`. This should work for renegotiation answers too.

No code change needed here — the existing offer/answer flow handles renegotiation. The fix is just triggering the new offer in Step 2.

- [ ] **Step 4: Build and test**

```bash
cd /c/build/talq && cmake --build . --target talq 2>&1 | tail -5
```

- [ ] **Step 5: Commit**

```bash
git add src/core/PublishPipeline.h src/core/PublishPipeline.cpp
git commit -m "fix: trigger renegotiation after enabling camera mid-call"
```

---

## Task 3: Wire local preview to CallWindow QML

**Files:**
- Modify: `src/core/CallManager.h`
- Modify: `src/core/CallManager.cpp`
- Modify: `src/qml/CallWindow.qml`

- [ ] **Step 1: Add localVideoProvider property to CallManager**

In `CallManager.h`, add Q_PROPERTY (after line 24):

```cpp
Q_PROPERTY(VideoFrameProvider* localVideoProvider READ localVideoProvider NOTIFY localVideoProviderChanged)
```

Add accessor (after line 40):

```cpp
VideoFrameProvider *localVideoProvider() const { return m_localVideoProvider; }
```

Add signal (after line 62):

```cpp
void localVideoProviderChanged();
```

Add member (after line 114):

```cpp
VideoFrameProvider *m_localVideoProvider = nullptr;
```

- [ ] **Step 2: Wire localVideoProvider when publisher starts**

In `CallManager::joinCallOnServer()`, after creating `m_publishPipeline` (line 421), add:

```cpp
m_localVideoProvider = m_publishPipeline->localVideoProvider();
emit localVideoProviderChanged();
```

In `CallManager::stopAllPipelines()`, after clearing `m_remoteVideoProvider` (line 521), add:

```cpp
m_localVideoProvider = nullptr;
emit localVideoProviderChanged();
```

- [ ] **Step 3: Add PIP VideoOutput to CallWindow.qml**

Add after the remoteVideo VideoOutput (after line 26):

```qml
// Local camera preview (PIP)
VideoOutput {
    id: localPreview
    anchors.right: parent.right
    anchors.bottom: controlBar.top
    anchors.rightMargin: 16
    anchors.bottomMargin: 16
    width: 160
    height: 120
    visible: callManager.isCameraOn && callManager.localVideoProvider && callManager.localVideoProvider.hasVideo
    fillMode: VideoOutput.PreserveAspectCrop
    z: 8

    // Mirror the preview so it feels natural
    transform: Scale { origin.x: localPreview.width / 2; xScale: -1 }

    Rectangle {
        anchors.fill: parent
        color: "transparent"
        border.color: "#40ffffff"
        border.width: 1
        radius: 4
        z: -1
    }
}

Connections {
    target: callManager
    function onLocalVideoProviderChanged() {
        if (callManager.localVideoProvider) {
            callManager.localVideoProvider.videoSink = localPreview.videoSink
        }
    }
}
```

- [ ] **Step 4: Fix the anchor error on line 103**

The QML warning `CallWindow.qml:103:5: Cannot anchor to an item that isn't a parent or sibling` — find and fix the offending anchor. Line 103 is the pulseRing anchoring to avatarCircle. Check that avatarCircle is a sibling (it's inside a ColumnLayout, pulseRing is outside). Fix:

```qml
// Change pulseRing to use fixed positioning instead of anchoring to avatarCircle
Rectangle {
    id: pulseRing
    x: (parent.width - width) / 2
    y: 24  // match the ColumnLayout topMargin
    width: 96; height: 96; radius: 48
    // ... rest unchanged
}
```

- [ ] **Step 5: Build and test**

```bash
cd /c/build/talq && cmake --build . --target talq 2>&1 | tail -5
```

Launch, start a call, toggle camera — should see PIP preview.

- [ ] **Step 6: Commit**

```bash
git add src/core/CallManager.h src/core/CallManager.cpp src/qml/CallWindow.qml
git commit -m "feat: local camera preview PIP in CallWindow"
```

---

## Task 4: Create PeerPipeline for P2P calls

**Files:**
- Create: `src/core/PeerPipeline.h`
- Create: `src/core/PeerPipeline.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Create PeerPipeline.h**

```cpp
#pragma once

#include <QObject>
#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>
#include "SignalingClient.h"
#include "VideoFrameProvider.h"

/**
 * Single-webrtcbin pipeline for P2P (peer-to-peer) 1:1 calls.
 * Sends local audio (+ optional video) and receives remote audio/video.
 * Caller creates offer; callee creates answer.
 * Supports mid-call renegotiation for camera toggle.
 */
class PeerPipeline : public QObject
{
    Q_OBJECT

public:
    explicit PeerPipeline(QObject *parent = nullptr);
    ~PeerPipeline() override;

    bool start(const QString &stunServer, const QList<TurnServer> &turnServers = {},
               const QString &audioInputDeviceId = {},
               const QString &audioOutputDeviceId = {});
    void stop();
    bool isRunning() const { return m_running; }

    // SDP exchange
    void createOffer();
    void setRemoteOffer(const QString &sdp);
    void setRemoteAnswer(const QString &sdp);
    void addIceCandidate(const QString &candidate, int sdpMLineIndex, const QString &sdpMid);

    // Audio
    void setMuted(bool muted);

    // Video
    void enableCamera(int deviceIndex, bool hd1080 = true);
    void disableCamera();

    VideoFrameProvider *localVideoProvider() const { return m_localVideoProvider; }
    VideoFrameProvider *remoteVideoProvider() const { return m_remoteVideoProvider; }

signals:
    void localOfferReady(const QString &sdp);
    void localAnswerReady(const QString &sdp);
    void iceCandidateReady(const QString &candidate, int sdpMLineIndex, const QString &sdpMid);
    void iceStateChanged(const QString &state);
    void audioLevelUpdated(double level);
    void error(const QString &message);
    void cameraError(const QString &reason);

public slots:
    void pollBus();

private:
    void cleanup();

    GstElement *m_pipeline = nullptr;
    GstElement *m_webrtcbin = nullptr;
    bool m_running = false;

    // Audio send
    GstElement *m_audioSrc = nullptr;

    // Video send
    GstElement *m_cameraSrc = nullptr;
    GstElement *m_videoConvert = nullptr;
    GstElement *m_videoCapsFilter = nullptr;
    GstElement *m_videoEncoder = nullptr;
    GstElement *m_videoPayloader = nullptr;
    GstElement *m_tee = nullptr;
    GstElement *m_encQueue = nullptr;
    GstElement *m_previewQueue = nullptr;
    GstElement *m_previewConvert = nullptr;
    GstElement *m_previewAppsink = nullptr;
    GstPad *m_videoSinkPad = nullptr;
    bool m_cameraEnabled = false;

    // Preview + remote video
    VideoFrameProvider *m_localVideoProvider = nullptr;
    VideoFrameProvider *m_remoteVideoProvider = nullptr;
    GstElement *m_videoAppsink = nullptr;

    // Audio receive device
    QString m_audioOutputDeviceId;

    // Receive chain builders (called from pad-added)
    void createAudioReceiveChain(GstPad *pad);
    void createVideoReceiveChain(GstPad *pad, const gchar *encoding);

    // GStreamer callbacks
    static void onNegotiationNeeded(GstElement *webrtc, gpointer userData);
    static void onIceCandidate(GstElement *webrtc, guint mlineIndex, gchar *candidate, gpointer userData);
    static void onOfferCreated(GstPromise *promise, gpointer userData);
    static void onAnswerCreated(GstPromise *promise, gpointer userData);
    static void onPadAdded(GstElement *webrtc, GstPad *pad, gpointer userData);
    static void onIceStateChanged(GObject *obj, GParamSpec *pspec, gpointer userData);
    static GstFlowReturn onPreviewSample(GstAppSink *sink, gpointer userData);
    static GstFlowReturn onRemoteVideoSample(GstAppSink *sink, gpointer userData);
};
```

- [ ] **Step 2: Create PeerPipeline.cpp**

Implement the P2P pipeline. Key differences from PublishPipeline:
- Single webrtcbin handles both send and receive
- `pad-added` callback handles incoming audio/video pads (same logic as SubscribePipeline)
- `createOffer()` for caller, `setRemoteOffer()` + answer for callee
- `enableCamera()` includes tee for local preview (same pattern as Task 1)
- After `enableCamera()`, manually calls `createOffer()` for renegotiation

The implementation follows the same GStreamer patterns already used in PublishPipeline and SubscribePipeline. The `start()` method creates the audio send chain (wasapi2src → audioconvert → audioresample → level → opusenc → rtpopuspay → webrtcbin). The `createAudioReceiveChain()` and `createVideoReceiveChain()` methods are identical to SubscribePipeline's versions.

- [ ] **Step 3: Add PeerPipeline to CMakeLists.txt**

Add after line 54 (`src/core/CallManager.cpp`):

```cmake
    src/core/PeerPipeline.cpp
```

- [ ] **Step 4: Build and verify compilation**

```bash
cd /c/build/talq && cmake --build . --target talq 2>&1 | tail -5
```

- [ ] **Step 5: Commit**

```bash
git add src/core/PeerPipeline.h src/core/PeerPipeline.cpp CMakeLists.txt
git commit -m "feat: add PeerPipeline for P2P 1:1 WebRTC calls"
```

---

## Task 5: Integrate P2P mode into CallManager

**Files:**
- Modify: `src/core/CallManager.h`
- Modify: `src/core/CallManager.cpp`

- [ ] **Step 1: Add P2P members to CallManager.h**

Add include (after line 10):

```cpp
#include "core/PeerPipeline.h"
```

Add member (after line 83):

```cpp
PeerPipeline *m_peerPipeline = nullptr;
bool m_useP2P = false;
```

- [ ] **Step 2: Add P2P decision logic**

In `CallManager::joinCallOnServer()`, after fetching STUN/TURN servers (around line 416), before creating the publisher pipeline, add mode selection:

```cpp
// Decide P2P vs MCU: use P2P for 1:1 when MCU is not forced
m_useP2P = !m_signaling->hasMcu();
qDebug() << "CallManager: call mode =" << (m_useP2P ? "P2P" : "MCU");
```

- [ ] **Step 3: Add P2P call flow**

After the mode selection, branch:

```cpp
if (m_useP2P) {
    // P2P: single pipeline for send + receive
    m_peerPipeline = new PeerPipeline(this);

    m_localVideoProvider = m_peerPipeline->localVideoProvider();
    emit localVideoProviderChanged();
    m_remoteVideoProvider = m_peerPipeline->remoteVideoProvider();
    emit remoteVideoProviderChanged();

    connect(m_peerPipeline, &PeerPipeline::localOfferReady,
            this, [this](const QString &sdp) {
        QString sid = QString::number(QDateTime::currentMSecsSinceEpoch());
        m_signaling->sendOffer(m_remoteSessionId, sdp, sid);
        qDebug() << "CallManager: sent P2P offer to" << m_remoteSessionId.left(20);
    });

    connect(m_peerPipeline, &PeerPipeline::localAnswerReady,
            this, [this](const QString &sdp) {
        QString sid = QString::number(QDateTime::currentMSecsSinceEpoch());
        m_signaling->sendAnswer(m_remoteSessionId, sdp, sid);
        qDebug() << "CallManager: sent P2P answer to" << m_remoteSessionId.left(20);
    });

    connect(m_peerPipeline, &PeerPipeline::iceCandidateReady,
            this, [this](const QString &candidate, int mline, const QString &mid) {
        QJsonObject c;
        c["candidate"] = candidate;
        c["sdpMLineIndex"] = mline;
        c["sdpMid"] = mid;
        QString sid = QString::number(QDateTime::currentMSecsSinceEpoch());
        m_signaling->sendCandidate(m_remoteSessionId, c, sid);
    });

    connect(m_peerPipeline, &PeerPipeline::iceStateChanged,
            this, [this](const QString &state) {
        qDebug() << "CallManager: P2P ICE:" << state;
        if (state == "connected" || state == "completed") {
            if (m_state == Connecting) {
                setState(Active);
                m_durationTimer.start();
            }
        }
    });

    connect(m_peerPipeline, &PeerPipeline::audioLevelUpdated,
            this, [this](double level) {
        if (qAbs(m_audioLevel - level) > 0.02) {
            m_audioLevel = level;
            emit audioLevelChanged();
        }
    });

    connect(m_peerPipeline, &PeerPipeline::cameraError, this, [this](const QString &reason) {
        qWarning() << "CallManager: P2P camera error:" << reason;
        m_cameraOn = false;
        emit cameraChanged();
    });

    m_peerPipeline->start(m_stunServer, turnServers,
        m_deviceManager ? m_deviceManager->selectedInputDeviceId() : QString(),
        m_deviceManager ? m_deviceManager->selectedOutputDeviceId() : QString());
    m_glibTimer.start(20);

    // If we're the caller (outgoing), create the offer
    // If callee, wait for offer via onOfferReceived
    if (m_state == Outgoing || m_state == Connecting) {
        if (!m_remoteSessionId.isEmpty()) {
            m_peerPipeline->createOffer();
        }
        // else: offer will be created when remote peer joins
    }

} else {
    // MCU mode — existing code (unchanged)
    // ... keep everything from line 421 to 496
}
```

- [ ] **Step 4: Update onOfferReceived for P2P**

Modify `CallManager::onOfferReceived()` (line 597) to handle P2P:

```cpp
void CallManager::onOfferReceived(const QString &fromSessionId, const QString &sdp, const QString &sid)
{
    qDebug() << "CallManager: received offer from" << fromSessionId.left(20) << "sid=" << sid;

    if (m_useP2P && m_peerPipeline) {
        // P2P: set remote offer on our single pipeline
        m_peerPipeline->setRemoteOffer(sdp);
        return;
    }

    // MCU mode — existing code unchanged
    // ...
}
```

- [ ] **Step 5: Update onAnswerReceived for P2P**

Modify `CallManager::onAnswerReceived()` (line 644):

```cpp
void CallManager::onAnswerReceived(const QString &fromSessionId, const QString &sdp)
{
    qDebug() << "CallManager: received answer from" << fromSessionId.left(20);

    if (m_useP2P && m_peerPipeline) {
        m_peerPipeline->setRemoteAnswer(sdp);
        qDebug() << "CallManager: set P2P remote answer";
        return;
    }

    // MCU mode — existing code unchanged
    if (m_publishPipeline) {
        m_publishPipeline->setRemoteAnswer(sdp);
        qDebug() << "CallManager: set publisher remote answer";
    }
}
```

- [ ] **Step 6: Update toggleCamera for P2P**

Modify `CallManager::toggleCamera()` (line 346):

```cpp
void CallManager::toggleCamera() {
    m_cameraOn = !m_cameraOn;
    emit cameraChanged();

    if (m_useP2P && m_peerPipeline) {
        if (m_cameraOn) {
            int videoDevice = m_deviceManager ? qMax(0, m_deviceManager->selectedVideoInput()) : 0;
            bool hd1080 = QSettings().value("video/resolution", 0).toInt() == 0;
            m_peerPipeline->enableCamera(videoDevice, hd1080);
        } else {
            m_peerPipeline->disableCamera();
        }
    } else if (m_publishPipeline) {
        if (m_cameraOn) {
            int videoDevice = m_deviceManager ? qMax(0, m_deviceManager->selectedVideoInput()) : 0;
            bool hd1080 = QSettings().value("video/resolution", 0).toInt() == 0;
            m_publishPipeline->enableCamera(videoDevice, hd1080);
        } else {
            m_publishPipeline->disableCamera();
        }
    }
}
```

- [ ] **Step 7: Update toggleMute for P2P**

Modify `CallManager::toggleMute()` (line 340):

```cpp
void CallManager::toggleMute() {
    m_muted = !m_muted;
    if (m_useP2P && m_peerPipeline) m_peerPipeline->setMuted(m_muted);
    else if (m_publishPipeline) m_publishPipeline->setMuted(m_muted);
    emit muteChanged();
}
```

- [ ] **Step 8: Update stopAllPipelines for P2P**

In `CallManager::stopAllPipelines()`, add P2P cleanup:

```cpp
if (m_peerPipeline) {
    m_peerPipeline->stop();
    m_peerPipeline->deleteLater();
    m_peerPipeline = nullptr;
}
```

- [ ] **Step 9: Update candidateReceived for P2P**

In the `candidateReceived` lambda (around line 140), add P2P handling:

```cpp
if (m_useP2P && m_peerPipeline) {
    m_peerPipeline->addIceCandidate(cStr, mline, mid);
} else {
    // existing MCU candidate routing
    if (fromSessionId == m_signaling->sessionId() && m_publishPipeline) {
        m_publishPipeline->addIceCandidate(cStr, mline, mid);
    } else if (m_subscribePipelines.contains(fromSessionId)) {
        m_subscribePipelines[fromSessionId]->addIceCandidate(cStr, mline, mid);
    }
}
```

- [ ] **Step 10: Update onParticipantJoinedCall for P2P**

In `onParticipantJoinedCall()` (line 561), when in P2P mode, create offer instead of requesting MCU offer:

```cpp
if (m_useP2P && m_peerPipeline) {
    m_peerPipeline->createOffer();
    qDebug() << "CallManager: creating P2P offer for joined peer";
} else {
    m_signaling->requestOffer(sessionId, "video");
    qDebug() << "CallManager: sent requestOffer for remote peer";
}
```

- [ ] **Step 11: Build and test**

```bash
cd /c/build/talq && cmake --build . --target talq 2>&1 | tail -5
```

- [ ] **Step 12: Commit**

```bash
git add src/core/CallManager.h src/core/CallManager.cpp
git commit -m "feat: P2P call mode for 1:1 calls alongside MCU"
```

---

## Task 6: End-to-end testing

- [ ] **Step 1: Test MCU mode (if server still has MCU enabled)**

1. Start TalQ with `QT_FORCE_STDERR_LOGGING=1`
2. Call a browser user
3. Toggle camera mid-call
4. Verify in logs:
   - "PublishPipeline: triggering renegotiation for video"
   - Second "offer created, SDP length=" (should be longer — includes video)
   - "PublishPipeline: set remote answer" (MCU accepted renegotiation)
   - Local preview shows in PIP
5. Verify browser user sees your video

- [ ] **Step 2: Test P2P mode**

1. On server: disable MCU by commenting `type = janus` in signaling server.conf, restart signaling
2. Start TalQ, verify log shows "call mode = P2P"
3. Call a browser user
4. Toggle camera — verify preview + remote peer sees video
5. Toggle mute — verify audio mutes

- [ ] **Step 3: Commit final state**

```bash
git add -A
git commit -m "test: verify video calls with preview, renegotiation, and P2P mode"
```

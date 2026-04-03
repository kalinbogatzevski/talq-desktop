# Data Channel Media State Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Send and receive media state (audioOn/Off, videoOn/Off, speaking/stoppedSpeaking) over WebRTC data channels to match the Nextcloud Talk browser protocol.

**Architecture:** Publisher pipeline stores its existing "status" data channel and exposes a send method. Subscriber pipeline listens for incoming data channels and emits parsed messages. CallManager orchestrates both: sending state on toggles and reacting to received state. Speaking detection thresholds existing audio levels.

**Tech Stack:** GStreamer webrtcbin data channel API, Qt signals, QJsonDocument

**Spec:** `docs/superpowers/specs/2026-04-03-datachannel-media-state-design.md`

---

## File Map

| File | Change | Responsibility |
|---|---|---|
| `src/core/PublishPipeline.h` | Modify | Add `m_statusDataChannel` member, `sendStatusMessage()` method |
| `src/core/PublishPipeline.cpp` | Modify | Store DC pointer on creation, implement send, null in cleanup |
| `src/core/SubscribePipeline.h` | Modify | Add `mediaStateReceived` signal, static callback declarations |
| `src/core/SubscribePipeline.cpp` | Modify | Connect `on-data-channel`, parse JSON, emit signal |
| `src/core/CallManager.h` | Modify | Add `m_speaking`, `m_speakingGrace` members |
| `src/core/CallManager.cpp` | Modify | DC sends in `broadcastMediaState()`, connect subscriber signal, speaking detection |

---

### Task 1: PublishPipeline — store data channel and expose send method

**Files:**
- Modify: `src/core/PublishPipeline.h:58-78` (add member + method)
- Modify: `src/core/PublishPipeline.cpp:268-279` (store DC instead of discarding)
- Modify: `src/core/PublishPipeline.cpp:358-381` (null pointer in cleanup)

- [ ] **Step 1: Add member and method to PublishPipeline.h**

In `src/core/PublishPipeline.h`, add the data channel pointer to the private members block (after `m_lvlDbg`) and the public send method (after `localVideoProvider()`):

Add public method after `localVideoProvider()` (line 45):

```cpp
void sendStatusMessage(const QByteArray &json);
```

Add private member after `m_lvlDbg` (line 78):

```cpp
GstWebRTCDataChannel *m_statusDataChannel = nullptr;
```

- [ ] **Step 2: Store DC pointer in start() instead of discarding**

In `src/core/PublishPipeline.cpp`, replace the data channel creation block (lines 272-279):

Old:
```cpp
    {
        GstWebRTCDataChannel *dc = nullptr;
        g_signal_emit_by_name(m_webrtcbin, "create-data-channel", "status", nullptr, &dc);
        if (dc) {
            qDebug() << "PublishPipeline: created data channel 'status'";
            g_object_unref(dc);
        }
    }
```

New:
```cpp
    {
        GstWebRTCDataChannel *dc = nullptr;
        g_signal_emit_by_name(m_webrtcbin, "create-data-channel", "status", nullptr, &dc);
        if (dc) {
            m_statusDataChannel = dc;  // takes ownership of the ref
            qDebug() << "PublishPipeline: created data channel 'status'";
        }
    }
```

- [ ] **Step 3: Implement sendStatusMessage()**

Add at the end of `src/core/PublishPipeline.cpp`, before the static callback functions (before `onNegotiationNeeded`):

```cpp
void PublishPipeline::sendStatusMessage(const QByteArray &json)
{
    if (!m_statusDataChannel || !m_running) return;
    gst_webrtc_data_channel_send_string(m_statusDataChannel, json.constData());
}
```

- [ ] **Step 4: Null the DC pointer in cleanup()**

In `src/core/PublishPipeline.cpp` `cleanup()`, add after `m_webrtcbin = nullptr;` (line 358):

```cpp
    if (m_statusDataChannel) {
        g_object_unref(m_statusDataChannel);
        m_statusDataChannel = nullptr;
    }
```

Note: unref BEFORE nulling `m_pipeline` would be ideal, but since the pipeline owns the webrtcbin which owns the channel, we just need to release our ref. Move this block to just before the `if (m_pipeline)` block (before line 349).

- [ ] **Step 5: Build and verify compilation**

Run:
```bash
/c/Qt/Tools/CMake_64/bin/cmake.exe --build /c/build/talq --target talq 2>&1 | tail -5
```

Expected: successful build, no errors.

- [ ] **Step 6: Commit**

```bash
cd /c/src/talk-desktop-qt
git add src/core/PublishPipeline.h src/core/PublishPipeline.cpp
git commit -m "feat(calls): store status data channel and expose sendStatusMessage()"
```

---

### Task 2: SubscribePipeline — receive data channel messages

**Files:**
- Modify: `src/core/SubscribePipeline.h:34-38` (add signal + callback declarations)
- Modify: `src/core/SubscribePipeline.cpp:68-74` (connect on-data-channel signal)

- [ ] **Step 1: Add signal and static callbacks to SubscribePipeline.h**

In `src/core/SubscribePipeline.h`, add a new signal after `error` (line 38):

```cpp
    void mediaStateReceived(const QString &type);
```

Add two static callback declarations in the private section after `onNewVideoSample` (line 65):

```cpp
    static void onDataChannel(GstElement *webrtc, GstWebRTCDataChannel *channel, gpointer userData);
    static void onDataChannelMessage(GstWebRTCDataChannel *channel, gchar *str, gpointer userData);
```

- [ ] **Step 2: Connect on-data-channel signal in start()**

In `src/core/SubscribePipeline.cpp`, add after the `notify::ice-connection-state` connection (after line 74):

```cpp
    g_signal_connect(m_webrtcbin, "on-data-channel",
                     G_CALLBACK(onDataChannel), this);
```

- [ ] **Step 3: Implement the data channel callbacks**

Add at the end of `src/core/SubscribePipeline.cpp` (after `onIceStateChanged`):

```cpp
void SubscribePipeline::onDataChannel(GstElement *, GstWebRTCDataChannel *channel, gpointer userData)
{
    auto *self = static_cast<SubscribePipeline *>(userData);
    qDebug() << "SubscribePipeline: incoming data channel";
    g_signal_connect(channel, "on-message-string",
                     G_CALLBACK(onDataChannelMessage), self);
}

void SubscribePipeline::onDataChannelMessage(GstWebRTCDataChannel *, gchar *str, gpointer userData)
{
    auto *self = static_cast<SubscribePipeline *>(userData);
    QByteArray raw(str);
    QJsonDocument doc = QJsonDocument::fromJson(raw);
    if (doc.isNull()) return;

    QString type = doc.object().value("type").toString();
    if (type.isEmpty()) return;

    QPointer<SubscribePipeline> guard(self);
    QMetaObject::invokeMethod(self, [guard, type]() {
        if (!guard) return;
        qDebug() << "SubscribePipeline: DC message:" << type;
        emit guard->mediaStateReceived(type);
    }, Qt::QueuedConnection);
}
```

- [ ] **Step 4: Add QJsonDocument include**

In `src/core/SubscribePipeline.cpp`, add at the top with other includes:

```cpp
#include <QJsonDocument>
#include <QJsonObject>
```

- [ ] **Step 5: Build and verify compilation**

Run:
```bash
/c/Qt/Tools/CMake_64/bin/cmake.exe --build /c/build/talq --target talq 2>&1 | tail -5
```

Expected: successful build, no errors.

- [ ] **Step 6: Commit**

```bash
cd /c/src/talk-desktop-qt
git add src/core/SubscribePipeline.h src/core/SubscribePipeline.cpp
git commit -m "feat(calls): receive media state from subscriber data channels"
```

---

### Task 3: CallManager — send media state on data channel

**Files:**
- Modify: `src/core/CallManager.cpp:490-510` (broadcastMediaState)

- [ ] **Step 1: Add data channel sends to broadcastMediaState()**

In `src/core/CallManager.cpp`, replace the TODO block in `broadcastMediaState()` (lines 506-509):

Old:
```cpp
    // TODO: Also send media state via the publisher data channel ("status" label).
    // The browser sends "audioOn"/"audioOff" and "videoOn"/"videoOff" as plain
    // strings on the GStreamer data channel created with create-data-channel "status".
    // This requires GStreamer data channel send APIs; signaling path works for now.
```

New:
```cpp
    // Send via data channel (matches browser Talk protocol)
    if (m_publishPipeline && m_publishPipeline->isRunning()) {
        QByteArray dcType;
        if (media == "audio") dcType = enabled ? R"({"type":"audioOn"})" : R"({"type":"audioOff"})";
        else if (media == "video") dcType = enabled ? R"({"type":"videoOn"})" : R"({"type":"videoOff"})";
        if (!dcType.isEmpty())
            m_publishPipeline->sendStatusMessage(dcType);
    }
```

- [ ] **Step 2: Build and verify compilation**

Run:
```bash
/c/Qt/Tools/CMake_64/bin/cmake.exe --build /c/build/talq --target talq 2>&1 | tail -5
```

Expected: successful build, no errors.

- [ ] **Step 3: Commit**

```bash
cd /c/src/talk-desktop-qt
git add src/core/CallManager.cpp
git commit -m "feat(calls): send media state via publisher data channel"
```

---

### Task 4: CallManager — receive media state from subscriber data channel

**Files:**
- Modify: `src/core/CallManager.cpp:982-1018` (subscriber creation block)

- [ ] **Step 1: Connect subscriber mediaStateReceived signal**

In `src/core/CallManager.cpp`, add a new `connect()` block in the subscriber creation section. Insert after the `iceStateChanged` connection (after line 1014, before the `error` connection on line 1016):

```cpp
    connect(sub, &SubscribePipeline::mediaStateReceived,
            this, [this](const QString &type) {
        if (type == "audioOn")       { m_remoteAudioMuted = false; emit remoteMediaChanged(); }
        else if (type == "audioOff") { m_remoteAudioMuted = true;  emit remoteMediaChanged(); }
        else if (type == "videoOn")  { m_remoteVideoMuted = false; emit remoteMediaChanged(); }
        else if (type == "videoOff") { m_remoteVideoMuted = true;  emit remoteMediaChanged(); }
        else if (type == "speaking" || type == "stoppedSpeaking") {
            // Received but no UI action yet — future: highlight active speaker
            qDebug() << "CallManager: remote" << type;
        }
    });
```

- [ ] **Step 2: Build and verify compilation**

Run:
```bash
/c/Qt/Tools/CMake_64/bin/cmake.exe --build /c/build/talq --target talq 2>&1 | tail -5
```

Expected: successful build, no errors.

- [ ] **Step 3: Commit**

```bash
cd /c/src/talk-desktop-qt
git add src/core/CallManager.cpp
git commit -m "feat(calls): handle incoming media state from subscriber data channel"
```

---

### Task 5: CallManager — speaking detection and broadcast

**Files:**
- Modify: `src/core/CallManager.h:119-146` (add members)
- Modify: `src/core/CallManager.cpp:875-881` (onAudioLevelUpdated)
- Modify: `src/core/CallManager.cpp:446-453` (toggleMute — reset speaking on mute)
- Modify: `src/core/CallManager.cpp:855-870` (teardown — reset speaking)

- [ ] **Step 1: Add speaking members to CallManager.h**

In `src/core/CallManager.h`, add after `m_cameraOn` (line 120):

```cpp
    bool m_speaking = false;
    QTimer m_speakingGrace;
```

- [ ] **Step 2: Initialize speakingGrace timer in constructor**

In `src/core/CallManager.cpp`, add after the `m_statsTimer` setup (after line 221):

```cpp
    m_speakingGrace.setSingleShot(true);
    m_speakingGrace.setInterval(500);
    connect(&m_speakingGrace, &QTimer::timeout, this, [this]() {
        if (m_speaking && m_audioLevel <= 0.05) {
            m_speaking = false;
            if (m_publishPipeline && m_publishPipeline->isRunning())
                m_publishPipeline->sendStatusMessage(R"({"type":"stoppedSpeaking"})");
        }
    });
```

- [ ] **Step 3: Add speaking detection to onAudioLevelUpdated()**

In `src/core/CallManager.cpp`, replace the existing `onAudioLevelUpdated` (lines 875-881):

Old:
```cpp
void CallManager::onAudioLevelUpdated(double level)
{
    if (qAbs(m_audioLevel - level) > 0.02) {
        m_audioLevel = level;
        emit audioLevelChanged();
    }
}
```

New:
```cpp
void CallManager::onAudioLevelUpdated(double level)
{
    if (qAbs(m_audioLevel - level) > 0.02) {
        m_audioLevel = level;
        emit audioLevelChanged();
    }

    // Speaking detection — only when not muted and publish pipeline is active
    if (m_muted || !m_publishPipeline || !m_publishPipeline->isRunning()) return;

    if (level > 0.05) {
        m_speakingGrace.stop();
        if (!m_speaking) {
            m_speaking = true;
            m_publishPipeline->sendStatusMessage(R"({"type":"speaking"})");
        }
    } else if (m_speaking && !m_speakingGrace.isActive()) {
        m_speakingGrace.start();
    }
}
```

- [ ] **Step 4: Reset speaking state on mute**

In `src/core/CallManager.cpp` `toggleMute()`, add after `emit muteChanged();` (after line 450):

```cpp
    // Stop speaking broadcast immediately on mute
    if (m_muted && m_speaking) {
        m_speakingGrace.stop();
        m_speaking = false;
        if (m_publishPipeline && m_publishPipeline->isRunning())
            m_publishPipeline->sendStatusMessage(R"({"type":"stoppedSpeaking"})");
    }
```

- [ ] **Step 5: Reset speaking state in teardown()**

In `src/core/CallManager.cpp` `teardown()`, add after `m_remoteAudioMuted = true;` (after line 868):

```cpp
    m_speaking = false;
    m_speakingGrace.stop();
```

- [ ] **Step 6: Build and verify compilation**

Run:
```bash
/c/Qt/Tools/CMake_64/bin/cmake.exe --build /c/build/talq --target talq 2>&1 | tail -5
```

Expected: successful build, no errors.

- [ ] **Step 7: Commit**

```bash
cd /c/src/talk-desktop-qt
git add src/core/CallManager.h src/core/CallManager.cpp
git commit -m "feat(calls): speaking detection and broadcast via data channel"
```

---

### Task 6: Smoke test — deploy and verify with browser

- [ ] **Step 1: Deploy debug build**

```bash
cd /c/build/talq && bash /c/src/talk-desktop-qt/scripts/deploy-dev.sh
```

- [ ] **Step 2: Manual test checklist**

1. Start a call between TalQ and a browser tab on ncloud.123net.link
2. In TalQ: mute → check browser shows muted indicator
3. In TalQ: unmute → check browser shows unmuted
4. In browser: mute → check TalQ shows muted (mute emoji on peer name)
5. In browser: unmute → check TalQ removes mute indicator
6. In TalQ: toggle camera → check browser reflects video state
7. In TalQ: speak → check browser shows speaking indicator
8. In TalQ: stop speaking → check browser clears speaking after ~500ms

- [ ] **Step 3: Check debug logs**

Look for these log lines:
- `PublishPipeline: created data channel 'status'` — DC created
- `SubscribePipeline: incoming data channel` — DC received from MCU
- `SubscribePipeline: DC message: audioOn` (or audioOff/videoOn/videoOff) — parsed incoming
- `CallManager: broadcast unmute audio` — signaling + DC sent together

# TalQ v0.8.0 — Video Calls + Call Polish Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add video call support (receive + optional camera send) and fix call polish issues (TURN, decline, race condition, device selection).

**Architecture:** Extends existing dual-pipeline WebRTC architecture. `SubscribePipeline` gets a video decode branch (`rtpvp8depay`/`rtph264depay` → decoder → `videoconvert` → `appsink`). New `VideoFrameProvider` bridges GStreamer frames to Qt's `QVideoSink`. `PublishPipeline` gets optional camera capture (`ksvideosrc` → `openh264enc`). P2 fixes are independent changes to `CallManager`, `SignalingClient`, and `MediaDeviceManager`.

**Tech Stack:** Qt 6.8.2 (aqtinstall), GStreamer 1.26.9 (MSYS2 MinGW64), C++20, QML, Ninja

**Spec:** `docs/superpowers/specs/2026-03-21-v080-video-calls-design.md`

**Prerequisite:** Install Qt6::Multimedia module (not included in base aqtinstall):
```bash
pip install aqtinstall
aqt install-qt windows desktop 6.8.2 win64_mingw --modules qtmultimedia -O C:/Qt
```

**Build command:**
```bash
export PATH="/c/msys64/mingw64/bin:/c/Qt/Tools/CMake_64/bin:/c/Qt/Tools/Ninja:/c/Qt/6.8.2/mingw_64/bin:$PATH"
cd C:/build/talq
cmake C:/Projects/talk-desktop-qt -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=C:/Qt/6.8.2/mingw_64
cmake --build . --target talq
```

**Run command:**
```bash
export PATH="/c/Qt/6.8.2/mingw_64/bin:/c/msys64/mingw64/bin:$PATH"
export QT_FORCE_STDERR_LOGGING=1
C:/build/talq/talq.exe 2>&1 | tee /tmp/talq-debug.log
```

**Test user:** `test-talq` / `talQing123@` on `https://ncloud.123net.link`, 1:1 conversation with kalin (token `u2f3gbu4`)

---

## File Structure

### New files
| File | Responsibility |
|------|---------------|
| `src/core/VideoFrameProvider.h` | Header: GStreamer appsink → QVideoFrame → QVideoSink bridge |
| `src/core/VideoFrameProvider.cpp` | Implementation: frame extraction, format conversion, thread marshaling |

### Modified files
| File | Changes |
|------|---------|
| `CMakeLists.txt` | Add `Qt6::Multimedia`, `VideoFrameProvider` sources, new GStreamer plugin DLLs |
| `src/core/SubscribePipeline.h` | Add `VideoFrameProvider*` member, video state, getter |
| `src/core/SubscribePipeline.cpp` | Video decode branch in `onPadAdded()`, codec detection |
| `src/core/PublishPipeline.h` | Camera methods, video elements, device/resolution params |
| `src/core/PublishPipeline.cpp` | Camera capture chain, `enableCamera()`/`disableCamera()`, updated `start()` |
| `src/core/CallManager.h` | `remoteVideoSink` property, `m_joinedCall`, `m_userActionReady`, TURN structs |
| `src/core/CallManager.cpp` | TURN parsing, video wiring, decline fix, race fix, device passthrough |
| `src/core/SignalingClient.h` | `TurnServer` struct, TURN getter |
| `src/core/SignalingClient.cpp` | Parse TURN credentials from signaling settings |
| `src/core/MediaDeviceManager.h` | Device ID storage, new getters |
| `src/core/MediaDeviceManager.cpp` | Extract `device.strid` / device index during enumeration |
| `src/qml/CallWindow.qml` | `VideoOutput`, overlay controls, auto-hide, camera toggle |
| `src/qml/IncomingCallPopup.qml` | `Component.onCompleted` signal for race fix |
| `src/qml/SettingsDialog.qml` | Camera combo, resolution preset, "applies next call" note |

---

## Task 1: Fix Incoming Call Decline (P2)

**Files:**
- Modify: `src/core/CallManager.h:58-70`
- Modify: `src/core/CallManager.cpp:290-319` (declineCall), `225-236` (startCall), `267-288` (acceptCall), `468-483` (teardown)

- [ ] **Step 1: Add `m_joinedCall` tracking to CallManager.h**

In `src/core/CallManager.h`, add to private members (after line 85):
```cpp
bool m_joinedCall = false;
```

- [ ] **Step 2: Set `m_joinedCall = true` after successful call join**

In `src/core/CallManager.cpp`, find `joinCallOnServer()` — after the successful POST `/call/{token}` response, add:
```cpp
m_joinedCall = true;
```

- [ ] **Step 3: Reset `m_joinedCall` in teardown**

In `src/core/CallManager.cpp` `teardown()` (line ~468), add before `setState(Idle)`:
```cpp
m_joinedCall = false;
```

- [ ] **Step 4: Rewrite `declineCall()` with proper sequencing**

Replace `declineCall()` (lines 290-319) with:
```cpp
void CallManager::declineCall()
{
    if (m_state != Incoming) return;

    qDebug() << "CallManager: declining call for token" << m_callToken;
    setState(Ending);

    if (m_joinedCall) {
        // We joined the call — leave it
        m_api->del(QString("apps/spreed/api/v4/call/%1").arg(m_callToken),
            [this](bool ok, const QJsonObject &, int) {
                qDebug() << "CallManager: leave call API" << (ok ? "succeeded" : "failed");
            });
    }

    // Always leave the room — triggers participantLeftRoom event for caller
    m_api->del(QString("apps/spreed/api/v4/room/%1/participants/active").arg(m_callToken),
        [this](bool ok, const QJsonObject &, int) {
            qDebug() << "CallManager: leave room API" << (ok ? "succeeded" : "failed");
            teardown("declined");
        });

    // Preserve cooldown tracking to prevent re-detection
    m_lastDeclinedToken = m_callToken;
    m_lastDeclinedTime = QDateTime::currentDateTime();
}
```

- [ ] **Step 5: Build and verify**

Build. Start app, log in as test-talq. Have kalin call test-talq from browser. Decline. Verify:
- No 404 error in logs
- Caller sees participant left (call ends or shows disconnected)
- App returns to Idle state cleanly

- [ ] **Step 6: Commit**

```bash
git add src/core/CallManager.h src/core/CallManager.cpp
git commit -m "fix: incoming call decline — track join state, leave room instead of call"
```

---

## Task 2: Fix Auto-Decline Race Condition (P2)

**Files:**
- Modify: `src/core/CallManager.h`
- Modify: `src/core/CallManager.cpp:290-319`
- Modify: `src/qml/IncomingCallPopup.qml`

- [ ] **Step 1: Add `m_userActionReady` flag to CallManager.h**

Add to private members:
```cpp
bool m_userActionReady = false;
```

- [ ] **Step 2: Guard `declineCall()` and `acceptCall()` with the flag**

In `declineCall()`, right after the existing `if (m_state != Incoming) return;` guard:
```cpp
if (!m_userActionReady) {
    qDebug() << "CallManager: ignoring decline — UI not ready";
    return;
}
```

In `acceptCall()`, right after the existing state validation:
```cpp
if (!m_userActionReady) {
    qDebug() << "CallManager: ignoring accept — UI not ready";
    return;
}
```

- [ ] **Step 3: Reset flag on incoming call detection, add setter**

In `CallManager.h`, add public slot:
```cpp
Q_INVOKABLE void setUserActionReady();
```

In `CallManager.cpp`:
```cpp
void CallManager::setUserActionReady()
{
    m_userActionReady = true;
    qDebug() << "CallManager: user action ready (popup loaded)";
}
```

In `onIncomingCallDetected()`, before `setState(Incoming)`:
```cpp
m_userActionReady = false;
```

- [ ] **Step 4: Remove the 2s timer guard**

Find and remove the `m_declineGuardTimer` or the `< 2s` elapsed check in `declineCall()`. Replace with the `m_userActionReady` check from step 2.

- [ ] **Step 5: Signal from IncomingCallPopup.qml on Component.onCompleted**

In `src/qml/IncomingCallPopup.qml`, add inside the root Window:
```qml
Component.onCompleted: {
    callManager.setUserActionReady()
}
```

- [ ] **Step 6: Reset flag in teardown**

In `teardown()`:
```cpp
m_userActionReady = false;
```

- [ ] **Step 7: Build and verify**

Build. Have kalin call test-talq. Verify:
- Incoming call popup appears and stays (no auto-decline)
- Accept and decline buttons work normally
- Check logs for "user action ready" message appearing before any accept/decline

- [ ] **Step 8: Commit**

```bash
git add src/core/CallManager.h src/core/CallManager.cpp src/qml/IncomingCallPopup.qml
git commit -m "fix: auto-decline race — gate actions on popup Component.onCompleted"
```

---

## Task 3: MediaDeviceManager Device Paths (P2)

**Files:**
- Modify: `src/core/MediaDeviceManager.h`
- Modify: `src/core/MediaDeviceManager.cpp:11-66`

- [ ] **Step 1: Add video selection and device ID getters to header**

In `src/core/MediaDeviceManager.h`, add private member:
```cpp
int m_selectedVideo = -1;
```

Add Q_PROPERTY:
```cpp
Q_PROPERTY(int selectedVideoInput READ selectedVideoInput WRITE setSelectedVideoInput NOTIFY selectedChanged)
```

Add public methods:
```cpp
int selectedVideoInput() const { return m_selectedVideo; }
void setSelectedVideoInput(int idx) { if (m_selectedVideo != idx) { m_selectedVideo = idx; emit selectedChanged(); } }
QString selectedInputDeviceId() const;
QString selectedOutputDeviceId() const;
```

- [ ] **Step 2: Set `md.id` to GStreamer device strid during enumeration**

In `src/core/MediaDeviceManager.cpp` `refresh()`, find where `md.id = md.name` is set. Replace with:
```cpp
GstStructure *props = gst_device_get_properties(device);
if (props) {
    const gchar *strid = gst_structure_get_string(props, "device.strid");
    if (!strid)
        strid = gst_structure_get_string(props, "device.path");
    md.id = strid ? QString::fromUtf8(strid) : md.name;
    gst_structure_free(props);
} else {
    md.id = md.name;
}
```

This uses the existing `MediaDevice.id` field — no parallel lists needed.

- [ ] **Step 3: Implement device ID getters**

```cpp
QString MediaDeviceManager::selectedInputDeviceId() const
{
    if (m_selectedInput >= 0 && m_selectedInput < m_audioInputs.size())
        return m_audioInputs[m_selectedInput].id;
    return {};
}

QString MediaDeviceManager::selectedOutputDeviceId() const
{
    if (m_selectedOutput >= 0 && m_selectedOutput < m_audioOutputs.size())
        return m_audioOutputs[m_selectedOutput].id;
    return {};
}
```

- [ ] **Step 4: Build and verify**

Build. Open Settings dialog, select different mic/speaker. Check debug logs show device IDs being extracted.

- [ ] **Step 5: Commit**

```bash
git add src/core/MediaDeviceManager.h src/core/MediaDeviceManager.cpp
git commit -m "feat: store GStreamer device IDs in MediaDeviceManager"
```

---

## Task 4: TURN Server Configuration (P2)

**Files:**
- Modify: `src/core/SignalingClient.h`
- Modify: `src/core/CallManager.cpp:356-376` (joinCallOnServer, where STUN is parsed)
- Modify: `src/core/PublishPipeline.h`
- Modify: `src/core/PublishPipeline.cpp:14-91`
- Modify: `src/core/SubscribePipeline.h`
- Modify: `src/core/SubscribePipeline.cpp:15-66`

- [ ] **Step 1: Define TurnServer struct**

In `src/core/SignalingClient.h`, add before the class:
```cpp
struct TurnServer {
    QStringList urls;
    QString username;
    QString credential;
};
```

- [ ] **Step 2: Parse TURN servers in CallManager::joinCallOnServer()**

In `src/core/CallManager.cpp`, find where `stunservers` is parsed (~line 361). After STUN extraction, add TURN parsing:
```cpp
QList<TurnServer> turnServers;
auto turnArr = settings["turnservers"].toArray();
for (const auto &ts : turnArr) {
    auto obj = ts.toObject();
    TurnServer turn;
    auto urls = obj["urls"].toArray();
    for (const auto &u : urls)
        turn.urls.append(u.toString());
    turn.username = obj["username"].toString();
    turn.credential = obj["credential"].toString();
    if (!turn.urls.isEmpty())
        turnServers.append(turn);
}
qDebug() << "CallManager: found" << turnServers.size() << "TURN servers";
```

- [ ] **Step 3: Update PublishPipeline::start() signature**

In `src/core/PublishPipeline.h`, change:
```cpp
bool start(const QString &stunServer, const QList<TurnServer> &turnServers,
           const QString &audioDeviceId = {});
```

In `src/core/PublishPipeline.cpp` `start()`, after setting STUN server (~line 28), add TURN:
```cpp
for (const auto &turn : turnServers) {
    for (const auto &url : turn.urls) {
        // Parse RFC 7065 TURN URL into GStreamer format
        QString gstUrl = url;
        gstUrl.remove(QRegularExpression("\\?transport=.*$"));
        // Ensure scheme has ://
        if (gstUrl.startsWith("turn:") && !gstUrl.startsWith("turn://"))
            gstUrl.replace("turn:", "turn://");
        if (gstUrl.startsWith("turns:") && !gstUrl.startsWith("turns://"))
            gstUrl.replace("turns:", "turns://");
        // URL-encode credentials (Nextcloud uses timestamp:username format with colons)
        QString escapedUser = QString(QUrl::toPercentEncoding(turn.username));
        QString escapedCred = QString(QUrl::toPercentEncoding(turn.credential));
        gstUrl.replace("://", QString("://%1:%2@").arg(escapedUser, escapedCred));
        qDebug() << "PublishPipeline: adding TURN server" << gstUrl;
        gboolean ret = FALSE;
        g_signal_emit_by_name(m_webrtcbin, "add-turn-server", gstUrl.toUtf8().constData(), &ret);
        qDebug() << "  result:" << (ret ? "ok" : "failed");
    }
}
```

- [ ] **Step 4: Update SubscribePipeline::start() the same way**

In `src/core/SubscribePipeline.h`, change:
```cpp
bool start(const QString &stunServer, const QList<TurnServer> &turnServers,
           const QString &audioOutputDeviceId = {});
```

Add the same TURN loop in `SubscribePipeline.cpp` `start()` after STUN setup.

- [ ] **Step 5: Update CallManager call sites**

In `CallManager.cpp`, wherever `m_publishPipeline->start(stunServer)` is called, pass TURN:
```cpp
m_publishPipeline->start(stunServer, turnServers);
```
Same for `SubscribePipeline::start()`.

- [ ] **Step 6: Build and verify**

Build. Make a call. Check logs for "adding TURN server" with correct URLs and credentials. Verify call still connects (STUN should work as before, TURN is additive).

- [ ] **Step 7: Commit**

```bash
git add src/core/SignalingClient.h src/core/CallManager.cpp src/core/PublishPipeline.h src/core/PublishPipeline.cpp src/core/SubscribePipeline.h src/core/SubscribePipeline.cpp
git commit -m "feat: parse and configure TURN servers from signaling settings"
```

---

## Task 5: Wire Device Selection to Pipelines (P2)

**Files:**
- Modify: `src/core/PublishPipeline.cpp:33` (wasapi2src creation)
- Modify: `src/core/SubscribePipeline.cpp:170-180` (wasapi2sink creation)
- Modify: `src/core/CallManager.cpp` (pass device IDs)

- [ ] **Step 1: Apply audio input device to PublishPipeline**

In `src/core/PublishPipeline.cpp` `start()`, after creating `wasapi2src` (~line 33), set device:
```cpp
if (!audioDeviceId.isEmpty()) {
    g_object_set(source, "device", audioDeviceId.toUtf8().constData(), nullptr);
    qDebug() << "PublishPipeline: using audio input device" << audioDeviceId;
}
```

- [ ] **Step 2: Apply audio output device to SubscribePipeline**

In `src/core/SubscribePipeline.cpp` `onPadAdded()`, where `wasapi2sink` is created (~line 170), store the device ID and apply it:

Add member to header:
```cpp
QString m_audioOutputDeviceId;
```

Set in `start()`:
```cpp
m_audioOutputDeviceId = audioOutputDeviceId;
```

In `onPadAdded()`, after creating sink:
```cpp
auto *self = static_cast<SubscribePipeline *>(userData);
if (!self->m_audioOutputDeviceId.isEmpty()) {
    g_object_set(sink, "device", self->m_audioOutputDeviceId.toUtf8().constData(), nullptr);
}
```

- [ ] **Step 3: Pass device IDs from CallManager**

In `CallManager.cpp`, update pipeline creation:
```cpp
m_publishPipeline->start(stunServer, turnServers,
    m_deviceManager->selectedInputDeviceId());

// In subscribe pipeline creation:
sub->start(stunServer, turnServers,
    m_deviceManager->selectedOutputDeviceId());
```

Add `MediaDeviceManager*` to CallManager if not already there. Store reference from constructor.

- [ ] **Step 4: Add "applies next call" note to SettingsDialog**

In `src/qml/SettingsDialog.qml`, after the speaker ComboBox, add:
```qml
Text {
    text: "Changes apply to next call"
    color: Theme.textSecondary
    font.pixelSize: 11
    Layout.alignment: Qt.AlignHCenter
    visible: callManager.state !== 0  // visible during active call
}
```

- [ ] **Step 5: Build and verify**

Build. Open Settings, select a specific mic. Make a call. Verify in logs the device ID is passed to wasapi2src.

- [ ] **Step 6: Commit**

```bash
git add src/core/PublishPipeline.h src/core/PublishPipeline.cpp src/core/SubscribePipeline.h src/core/SubscribePipeline.cpp src/core/CallManager.h src/core/CallManager.cpp src/qml/SettingsDialog.qml
git commit -m "feat: wire device selection to audio pipelines"
```

---

## Task 6: VideoFrameProvider + CMake Foundation (P1)

**Files:**
- Create: `src/core/VideoFrameProvider.h`
- Create: `src/core/VideoFrameProvider.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Update CMakeLists.txt**

Add `Qt6::Multimedia` to find_package (~line 7):
```cmake
find_package(Qt6 REQUIRED COMPONENTS Core Gui Network Qml Quick QuickControls2 Sql WebSockets Widgets Multimedia)
```

Add `gstapp-1.0` to GST_LIBRARIES (~line 30) for appsink API:
```cmake
set(GST_LIBRARIES gstreamer-1.0 gstsdp-1.0 gstwebrtc-1.0 gstapp-1.0 gobject-2.0 glib-2.0 intl)
```

Add source files to the target (~line 51 area):
```cmake
src/core/VideoFrameProvider.h
src/core/VideoFrameProvider.cpp
```

Add `Qt6::Multimedia` to target_link_libraries (~line 90):
```cmake
Qt6::Multimedia
```

- [ ] **Step 2: Create VideoFrameProvider.h**

```cpp
#pragma once
#include <QObject>
#include <QVideoSink>
#include <QVideoFrame>
#include <QVideoFrameFormat>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>

class VideoFrameProvider : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVideoSink* videoSink READ videoSink WRITE setVideoSink NOTIFY videoSinkChanged)
    Q_PROPERTY(bool hasVideo READ hasVideo NOTIFY hasVideoChanged)

public:
    explicit VideoFrameProvider(QObject *parent = nullptr);

    QVideoSink *videoSink() const { return m_videoSink; }
    void setVideoSink(QVideoSink *sink);
    bool hasVideo() const { return m_hasVideo; }

    void feedFrame(GstSample *sample);

signals:
    void videoSinkChanged();
    void hasVideoChanged();

private:
    QVideoSink *m_videoSink = nullptr;
    bool m_hasVideo = false;
};
```

- [ ] **Step 3: Create VideoFrameProvider.cpp**

```cpp
#include "VideoFrameProvider.h"
#include <QDebug>

VideoFrameProvider::VideoFrameProvider(QObject *parent)
    : QObject(parent)
{
}

void VideoFrameProvider::setVideoSink(QVideoSink *sink)
{
    if (m_videoSink != sink) {
        m_videoSink = sink;
        emit videoSinkChanged();
    }
}

void VideoFrameProvider::feedFrame(GstSample *sample)
{
    if (!m_videoSink || !sample) return;

    GstCaps *caps = gst_sample_get_caps(sample);
    if (!caps) return;

    GstStructure *s = gst_caps_get_structure(caps, 0);
    int width = 0, height = 0;
    gst_structure_get_int(s, "width", &width);
    gst_structure_get_int(s, "height", &height);
    if (width <= 0 || height <= 0) return;

    const gchar *format = gst_structure_get_string(s, "format");
    QVideoFrameFormat::PixelFormat pixFmt = QVideoFrameFormat::Format_Invalid;
    if (format && g_strcmp0(format, "I420") == 0)
        pixFmt = QVideoFrameFormat::Format_YUV420P;
    else if (format && g_strcmp0(format, "NV12") == 0)
        pixFmt = QVideoFrameFormat::Format_NV12;
    else {
        qWarning() << "VideoFrameProvider: unsupported pixel format" << (format ? format : "null");
        return;
    }

    GstBuffer *buf = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (!gst_buffer_map(buf, &map, GST_MAP_READ)) return;

    QVideoFrameFormat fmt(QSize(width, height), pixFmt);
    QVideoFrame frame(fmt);
    if (frame.map(QVideoFrame::WriteOnly)) {
        // I420: 3 planes (Y, U, V). Copy each explicitly.
        int ySize = width * height;
        int uvSize = (width / 2) * (height / 2);
        const uchar *src = map.data;
        if ((qsizetype)(ySize + 2 * uvSize) <= (qsizetype)map.size) {
            memcpy(frame.bits(0), src, ySize);
            if (frame.planeCount() >= 3) {
                memcpy(frame.bits(1), src + ySize, uvSize);
                memcpy(frame.bits(2), src + ySize + uvSize, uvSize);
            }
        }
        frame.unmap();
        m_videoSink->setVideoFrame(frame);

        if (!m_hasVideo) {
            m_hasVideo = true;
            emit hasVideoChanged();
        }
    }

    gst_buffer_unmap(buf, &map);
}
```

- [ ] **Step 4: Build and verify**

Build. The new class should compile. No runtime changes yet.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt src/core/VideoFrameProvider.h src/core/VideoFrameProvider.cpp
git commit -m "feat: add VideoFrameProvider — GStreamer appsink to QVideoFrame bridge"
```

---

## Task 7: SubscribePipeline Video Receive (P1)

**Files:**
- Modify: `src/core/SubscribePipeline.h`
- Modify: `src/core/SubscribePipeline.cpp:135-189` (onPadAdded)

- [ ] **Step 1: Add video members to SubscribePipeline.h**

Add includes:
```cpp
#include "VideoFrameProvider.h"
```

Add private members:
```cpp
VideoFrameProvider *m_videoProvider = nullptr;
GstElement *m_videoAppsink = nullptr;
```

Add public getter:
```cpp
VideoFrameProvider *videoProvider() const { return m_videoProvider; }
```

Add static callback:
```cpp
static GstFlowReturn onNewVideoSample(GstAppSink *sink, gpointer userData);
```

- [ ] **Step 2: Create VideoFrameProvider in constructor**

In `SubscribePipeline.cpp` constructor, add:
```cpp
m_videoProvider = new VideoFrameProvider(this);
```

- [ ] **Step 3: Rewrite onPadAdded() to handle both audio and video**

Replace the `onPadAdded` method (lines 135-189):
```cpp
void SubscribePipeline::onPadAdded(GstElement *, GstPad *pad, gpointer userData)
{
    auto *self = static_cast<SubscribePipeline *>(userData);

    GstCaps *caps = gst_pad_get_current_caps(pad);
    if (!caps) caps = gst_pad_query_caps(pad, nullptr);
    if (!caps) return;

    GstStructure *s = gst_caps_get_structure(caps, 0);
    const gchar *media = gst_structure_get_string(s, "media");
    const gchar *encoding = gst_structure_get_string(s, "encoding-name");

    qDebug() << "SubscribePipeline: pad added, media=" << media << "encoding=" << encoding;

    bool isAudio = (media && g_strcmp0(media, "audio") == 0)
                || (encoding && g_ascii_strcasecmp(encoding, "OPUS") == 0);
    bool isVideo = (media && g_strcmp0(media, "video") == 0)
                || (encoding && (g_ascii_strcasecmp(encoding, "VP8") == 0
                              || g_ascii_strcasecmp(encoding, "H264") == 0));

    // Copy encoding string before unreffing caps (encoding points into caps memory)
    QByteArray encodingCopy = encoding ? QByteArray(encoding) : QByteArray();
    gst_caps_unref(caps);

    if (isAudio) {
        self->createAudioChain(pad);
    } else if (isVideo) {
        self->createVideoChain(pad, encodingCopy.constData());
    } else {
        qDebug() << "SubscribePipeline: skipping unknown pad type";
    }
}
```

- [ ] **Step 4: Extract existing audio chain into createAudioChain()**

Add declaration to header:
```cpp
void createAudioChain(GstPad *pad);
void createVideoChain(GstPad *pad, const gchar *encoding);
```

Move the existing audio chain code (depay → dec → convert → resample → sink) from `onPadAdded` into `createAudioChain()`. Keep it functionally identical, just refactored.

- [ ] **Step 5: Implement createVideoChain()**

```cpp
void SubscribePipeline::createVideoChain(GstPad *pad, const gchar *encoding)
{
    qDebug() << "SubscribePipeline: creating video chain for" << encoding;

    // Select depayloader and decoder based on codec
    GstElement *depay = nullptr;
    GstElement *decoder = nullptr;

    if (encoding && g_ascii_strcasecmp(encoding, "VP8") == 0) {
        depay = gst_element_factory_make("rtpvp8depay", nullptr);
        decoder = gst_element_factory_make("vp8dec", nullptr);
    } else {
        // Default to H.264
        depay = gst_element_factory_make("rtph264depay", nullptr);
        decoder = gst_element_factory_make("openh264dec", nullptr);
    }

    GstElement *convert = gst_element_factory_make("videoconvert", nullptr);
    GstElement *appsink = gst_element_factory_make("appsink", nullptr);

    if (!depay || !decoder || !convert || !appsink) {
        qWarning() << "SubscribePipeline: failed to create video elements";
        return;
    }

    // Configure appsink: emit signals, I420 format, drop old frames
    GstCaps *sinkCaps = gst_caps_from_string("video/x-raw,format=I420");
    g_object_set(appsink,
        "emit-signals", TRUE,
        "caps", sinkCaps,
        "drop", TRUE,
        "max-buffers", 1,
        nullptr);
    gst_caps_unref(sinkCaps);

    // Connect new-sample signal
    g_signal_connect(appsink, "new-sample",
        G_CALLBACK(onNewVideoSample), this);
    m_videoAppsink = appsink;

    gst_bin_add_many(GST_BIN(m_pipeline), depay, decoder, convert, appsink, nullptr);
    gst_element_link_many(depay, decoder, convert, appsink, nullptr);

    gst_element_sync_state_with_parent(depay);
    gst_element_sync_state_with_parent(decoder);
    gst_element_sync_state_with_parent(convert);
    gst_element_sync_state_with_parent(appsink);

    GstPad *sinkPad = gst_element_get_static_pad(depay, "sink");
    GstPadLinkReturn ret = gst_pad_link(pad, sinkPad);
    gst_object_unref(sinkPad);

    if (ret != GST_PAD_LINK_OK)
        qWarning() << "SubscribePipeline: video pad link failed:" << ret;
    else
        qDebug() << "SubscribePipeline: video chain linked successfully";
}
```

- [ ] **Step 6: Implement onNewVideoSample callback**

```cpp
GstFlowReturn SubscribePipeline::onNewVideoSample(GstAppSink *sink, gpointer userData)
{
    auto *self = static_cast<SubscribePipeline *>(userData);
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_OK;

    // Marshal to Qt main thread
    QMetaObject::invokeMethod(self->m_videoProvider, [self, sample]() {
        self->m_videoProvider->feedFrame(sample);
        gst_sample_unref(sample);
    }, Qt::QueuedConnection);

    return GST_FLOW_OK;
}
```

- [ ] **Step 7: Build and verify**

Build. Make a call from browser (with video enabled in browser). Check logs for "creating video chain" and "video chain linked successfully". Video won't display yet (no QML wiring) but frames should be decoded.

- [ ] **Step 8: Commit**

```bash
git add src/core/SubscribePipeline.h src/core/SubscribePipeline.cpp
git commit -m "feat: video receive in SubscribePipeline — VP8/H264 decode to appsink"
```

---

## Task 8: PublishPipeline Camera Send (P1)

**Files:**
- Modify: `src/core/PublishPipeline.h`
- Modify: `src/core/PublishPipeline.cpp`

- [ ] **Step 1: Add camera members to PublishPipeline.h**

Add members:
```cpp
GstElement *m_cameraSrc = nullptr;
GstElement *m_videoConvert = nullptr;
GstElement *m_videoCapsFilter = nullptr;
GstElement *m_videoEncoder = nullptr;
GstElement *m_videoPayloader = nullptr;
GstPad *m_videoSinkPad = nullptr;
bool m_cameraEnabled = false;
```

Add methods:
```cpp
void enableCamera(int deviceIndex, bool hd1080 = true);
void disableCamera();

signals:
    void cameraError(const QString &reason);
```

- [ ] **Step 2: Implement enableCamera()**

```cpp
void PublishPipeline::enableCamera(int deviceIndex, bool hd1080)
{
    if (m_cameraEnabled || !m_pipeline) return;

    qDebug() << "PublishPipeline: enabling camera, device" << deviceIndex
             << (hd1080 ? "1080p" : "720p");

    m_cameraSrc = gst_element_factory_make("ksvideosrc", nullptr);
    if (!m_cameraSrc) {
        emit cameraError("Camera capture plugin (ksvideosrc) not available");
        return;
    }
    g_object_set(m_cameraSrc, "device-index", deviceIndex, nullptr);

    m_videoConvert = gst_element_factory_make("videoconvert", nullptr);
    m_videoCapsFilter = gst_element_factory_make("capsfilter", nullptr);
    m_videoEncoder = gst_element_factory_make("openh264enc", nullptr);
    m_videoPayloader = gst_element_factory_make("rtph264pay", nullptr);

    if (!m_videoConvert || !m_videoCapsFilter || !m_videoEncoder || !m_videoPayloader) {
        emit cameraError("Failed to create video encoding elements");
        // Clean up partially created elements
        if (m_cameraSrc) { gst_object_unref(m_cameraSrc); m_cameraSrc = nullptr; }
        if (m_videoConvert) { gst_object_unref(m_videoConvert); m_videoConvert = nullptr; }
        if (m_videoCapsFilter) { gst_object_unref(m_videoCapsFilter); m_videoCapsFilter = nullptr; }
        if (m_videoEncoder) { gst_object_unref(m_videoEncoder); m_videoEncoder = nullptr; }
        if (m_videoPayloader) { gst_object_unref(m_videoPayloader); m_videoPayloader = nullptr; }
        return;
    }

    // Resolution caps
    int w = hd1080 ? 1920 : 1280;
    int h = hd1080 ? 1080 : 720;
    int bitrate = hd1080 ? 3000000 : 1500000;
    QString capsStr = QString("video/x-raw,width=%1,height=%2,framerate=30/1").arg(w).arg(h);
    GstCaps *caps = gst_caps_from_string(capsStr.toUtf8().constData());
    g_object_set(m_videoCapsFilter, "caps", caps, nullptr);
    gst_caps_unref(caps);

    // Encoder settings
    g_object_set(m_videoEncoder,
        "bitrate", bitrate,
        "rate-control", 1,  // bitrate mode
        "complexity", 1,    // medium
        nullptr);

    gst_bin_add_many(GST_BIN(m_pipeline),
        m_cameraSrc, m_videoConvert, m_videoCapsFilter,
        m_videoEncoder, m_videoPayloader, nullptr);

    if (!gst_element_link_many(m_cameraSrc, m_videoConvert, m_videoCapsFilter,
                                m_videoEncoder, m_videoPayloader, nullptr)) {
        qWarning() << "PublishPipeline: failed to link video chain";
        emit cameraError("Failed to link video pipeline");
        disableCamera();
        return;
    }

    // Request a new sink pad from webrtcbin for video
    m_videoSinkPad = gst_element_request_pad_simple(m_webrtcbin, "sink_%u");
    GstPad *payloaderSrc = gst_element_get_static_pad(m_videoPayloader, "src");
    GstPadLinkReturn ret = gst_pad_link(payloaderSrc, m_videoSinkPad);
    gst_object_unref(payloaderSrc);

    if (ret != GST_PAD_LINK_OK) {
        qWarning() << "PublishPipeline: video pad link failed:" << ret;
        emit cameraError("Failed to connect video to WebRTC");
        disableCamera();
        return;
    }

    // Sync all video elements to pipeline state
    gst_element_sync_state_with_parent(m_cameraSrc);
    gst_element_sync_state_with_parent(m_videoConvert);
    gst_element_sync_state_with_parent(m_videoCapsFilter);
    gst_element_sync_state_with_parent(m_videoEncoder);
    gst_element_sync_state_with_parent(m_videoPayloader);

    m_cameraEnabled = true;
    qDebug() << "PublishPipeline: camera enabled successfully";
}
```

- [ ] **Step 3: Implement disableCamera()**

```cpp
void PublishPipeline::disableCamera()
{
    if (!m_cameraEnabled && !m_cameraSrc) return;

    qDebug() << "PublishPipeline: disabling camera";

    auto removeElement = [this](GstElement *&el) {
        if (el) {
            gst_element_set_state(el, GST_STATE_NULL);
            gst_bin_remove(GST_BIN(m_pipeline), el);
            el = nullptr;  // bin_remove takes ownership
        }
    };

    // Unlink payloader from webrtcbin first
    if (m_videoPayloader && m_videoSinkPad) {
        GstPad *src = gst_element_get_static_pad(m_videoPayloader, "src");
        if (src) {
            gst_pad_unlink(src, m_videoSinkPad);
            gst_object_unref(src);
        }
    }
    if (m_videoSinkPad) {
        gst_element_release_request_pad(m_webrtcbin, m_videoSinkPad);
        gst_object_unref(m_videoSinkPad);
        m_videoSinkPad = nullptr;
    }

    removeElement(m_videoPayloader);
    removeElement(m_videoEncoder);
    removeElement(m_videoCapsFilter);
    removeElement(m_videoConvert);
    removeElement(m_cameraSrc);

    m_cameraEnabled = false;
}
```

- [ ] **Step 4: Call disableCamera() in stop()**

In `PublishPipeline::stop()`, add before the existing cleanup:
```cpp
disableCamera();
```

- [ ] **Step 5: Build and verify**

Build. Camera won't be enabled yet from UI (no QML wiring), but verify compilation. Can test with a temporary `enableCamera(0)` call after `start()` returns true if desired.

- [ ] **Step 6: Commit**

```bash
git add src/core/PublishPipeline.h src/core/PublishPipeline.cpp
git commit -m "feat: camera capture in PublishPipeline — ksvideosrc + openh264enc"
```

---

## Task 9: CallManager Video Wiring (P1)

**Files:**
- Modify: `src/core/CallManager.h`
- Modify: `src/core/CallManager.cpp`
- Modify: `src/main.cpp` (if needed for MediaDeviceManager passthrough)

- [ ] **Step 1: Add video properties to CallManager.h**

Add includes:
```cpp
#include "VideoFrameProvider.h"
```

Add Q_PROPERTY:
```cpp
Q_PROPERTY(VideoFrameProvider* remoteVideoProvider READ remoteVideoProvider NOTIFY remoteVideoProviderChanged)
```

Add members:
```cpp
VideoFrameProvider *m_remoteVideoProvider = nullptr;
MediaDeviceManager *m_deviceManager = nullptr;
```

Add getter and signal:
```cpp
VideoFrameProvider *remoteVideoProvider() const { return m_remoteVideoProvider; }
signals:
    void remoteVideoProviderChanged();
```

Add invokable:
```cpp
Q_INVOKABLE void toggleCamera();
```

- [ ] **Step 2: Store MediaDeviceManager reference**

Update CallManager constructor to accept MediaDeviceManager:
```cpp
CallManager(ApiClient *api, SignalingClient *signaling, MediaDeviceManager *deviceMgr, QObject *parent = nullptr);
```

In constructor body:
```cpp
m_deviceManager = deviceMgr;
```

Update `src/main.cpp` call site (~line 85):
```cpp
CallManager callManager(&api, &signaling, &deviceManager);
```

- [ ] **Step 3: Wire video provider from SubscribePipeline**

When a SubscribePipeline is created (in the offer-received handler), after starting it, connect its video provider:
```cpp
m_remoteVideoProvider = sub->videoProvider();
emit remoteVideoProviderChanged();
```

In `stopAllPipelines()`, add:
```cpp
m_remoteVideoProvider = nullptr;
emit remoteVideoProviderChanged();
```

- [ ] **Step 4: Implement toggleCamera()**

```cpp
void CallManager::toggleCamera()
{
    m_cameraOn = !m_cameraOn;
    emit cameraChanged();

    if (m_cameraOn && m_publishPipeline) {
        int videoDevice = m_deviceManager ? m_deviceManager->selectedVideoInput() : 0;
        bool hd1080 = QSettings().value("video/resolution", 0).toInt() == 0; // 0=1080p, 1=720p
        m_publishPipeline->enableCamera(videoDevice, hd1080);
    } else if (!m_cameraOn && m_publishPipeline) {
        m_publishPipeline->disableCamera();
    }
}
```

- [ ] **Step 5: Connect camera error signal**

When creating PublishPipeline:
```cpp
connect(m_publishPipeline, &PublishPipeline::cameraError, this, [this](const QString &reason) {
    qWarning() << "CallManager: camera error:" << reason;
    m_cameraOn = false;
    emit cameraChanged();
});
```

- [ ] **Step 6: Build and verify**

Build. Make a call. Check logs for video provider being set when remote sends video.

- [ ] **Step 7: Commit**

```bash
git add src/core/CallManager.h src/core/CallManager.cpp src/main.cpp
git commit -m "feat: CallManager video wiring — provider, camera toggle, device passthrough"
```

---

## Task 10: CallWindow Video Layout (P1)

**Files:**
- Modify: `src/qml/CallWindow.qml`

- [ ] **Step 1: Add VideoOutput import and element**

At top of file, add import:
```qml
import QtMultimedia
```

Inside the root Window, add `VideoOutput` as the first child (behind everything):
```qml
VideoOutput {
    id: remoteVideo
    anchors.fill: parent
    visible: callManager.remoteVideoProvider && callManager.remoteVideoProvider.hasVideo
    videoSink: callManager.remoteVideoProvider ? callManager.remoteVideoProvider.videoSink : null
    fillMode: VideoOutput.PreserveAspectCrop
}
```

- [ ] **Step 2: Hide avatar when video is active**

Wrap the avatar circle and peer name in a visibility condition:
```qml
visible: !remoteVideo.visible
```

Apply to the avatar `Rectangle`, the peer name `Text`, the status `Text`, and the waveform `Canvas`.

- [ ] **Step 3: Create overlay control bar**

Replace the existing bottom control bar with an overlay version:
```qml
Rectangle {
    id: controlBar
    anchors.bottom: parent.bottom
    anchors.left: parent.left
    anchors.right: parent.right
    height: 70
    color: remoteVideo.visible ? "#80000000" : "transparent"
    opacity: controlBarVisible ? 1.0 : 0.0

    Behavior on opacity { NumberAnimation { duration: 300 } }

    property bool controlBarVisible: true

    // ... existing control buttons moved here
}
```

- [ ] **Step 4: Add auto-hide on mouse inactivity**

Add to root Window:
```qml
MouseArea {
    anchors.fill: parent
    hoverEnabled: true
    propagateComposedEvents: true
    onPositionChanged: (mouse) => {
        controlBar.controlBarVisible = true
        hideTimer.restart()
        mouse.accepted = false
    }
    onClicked: (mouse) => mouse.accepted = false
}

Timer {
    id: hideTimer
    interval: 3000
    onTriggered: {
        if (remoteVideo.visible)
            controlBar.controlBarVisible = false
    }
}
```

- [ ] **Step 5: Add camera toggle button**

In the control bar, add a camera button next to the mute button:
```qml
Rectangle {
    width: 48; height: 48
    radius: 24
    color: callManager.isCameraOn ? "#2ecc71" : "#555"
    visible: callManager.state === 4 || callManager.state === 3  // Active or Connecting

    Text {
        anchors.centerIn: parent
        text: callManager.isCameraOn ? "\uD83D\uDCF7" : "\uD83D\uDCF7\u0336"
        font.pixelSize: 20
    }

    MouseArea {
        anchors.fill: parent
        onClicked: callManager.toggleCamera()
    }
}
```

- [ ] **Step 6: Add duration overlay**

Top-center duration display for video mode:
```qml
Rectangle {
    anchors.top: parent.top
    anchors.horizontalCenter: parent.horizontalCenter
    anchors.topMargin: 16
    width: durationText.width + 24
    height: 32
    radius: 16
    color: "#80000000"
    visible: remoteVideo.visible && callManager.state === 4

    Text {
        id: durationText
        anchors.centerIn: parent
        text: formatDuration(callManager.callDuration)
        color: "white"
        font.pixelSize: 14
    }
}
```

- [ ] **Step 7: Build and verify**

Build. Make a video call from browser to app. Verify:
- Remote video fills the window
- Avatar hidden when video is active
- Controls auto-hide after 3s, reappear on mouse move
- Camera toggle button visible during active call
- Duration overlay shows at top

- [ ] **Step 8: Commit**

```bash
git add src/qml/CallWindow.qml
git commit -m "feat: CallWindow video layout — VideoOutput, overlay controls, auto-hide"
```

---

## Task 11: SettingsDialog Camera + Resolution (P1)

**Files:**
- Modify: `src/qml/SettingsDialog.qml`

- [ ] **Step 1: Add camera selection ComboBox**

After the speaker section, add:
```qml
Text {
    text: "Camera"
    color: Theme.textPrimary
    font.pixelSize: 14
    font.bold: true
    Layout.topMargin: 16
}

ComboBox {
    Layout.fillWidth: true
    model: deviceManager.videoInputNames
    currentIndex: deviceManager.selectedVideoInput
    onActivated: (index) => deviceManager.selectedVideoInput = index
    enabled: deviceManager.videoInputNames.length > 0

    Text {
        anchors.centerIn: parent
        text: "No cameras found"
        visible: deviceManager.videoInputNames.length === 0
        color: Theme.textSecondary
    }
}
```

- [ ] **Step 2: Add resolution preset**

```qml
Text {
    text: "Video Quality"
    color: Theme.textPrimary
    font.pixelSize: 14
    font.bold: true
    Layout.topMargin: 12
}

ComboBox {
    Layout.fillWidth: true
    model: ["Full HD (1080p)", "HD (720p)"]
    currentIndex: settings.value("video/resolution", 0)
    onActivated: (index) => {
        settings.setValue("video/resolution", index)
    }
}
```

Note: QML `Settings` import or a `QSettings` wrapper exposed as context property is needed. Use:
```qml
import Qt.labs.settings
Settings { id: settings; category: "video" }
```

Or expose from C++. The simplest approach: use `Qt.labs.settings` inline.
```

- [ ] **Step 3: Build and verify**

Build. Open Settings dialog. Verify camera combo shows detected cameras (or "No cameras found"). Resolution combo shows presets.

- [ ] **Step 4: Commit**

```bash
git add src/qml/SettingsDialog.qml
git commit -m "feat: camera selection and resolution preset in SettingsDialog"
```

---

## Task 12: GStreamer Plugin Deployment

**Files:**
- Modify: build scripts / CONTINUE.md

- [ ] **Step 1: Copy new plugin DLLs to gst-plugins/**

```bash
cd C:/build/talq
cp /c/msys64/mingw64/lib/gstreamer-1.0/libgst{vpx,openh264,videoconvertscale,winks}.dll gst-plugins/
```

Verify `libgstapp.dll` is already there (used for appsink).

- [ ] **Step 2: Verify plugins load**

```bash
export GST_PLUGIN_PATH=C:/build/talq/gst-plugins
GST_DEBUG=3 C:/build/talq/talq.exe 2>&1 | grep -i "vpx\|openh264\|winks\|videoconvert"
```

- [ ] **Step 3: Update CONTINUE.md build section**

Update the plugin copy line to include new DLLs:
```bash
mkdir -p gst-plugins && cp /c/msys64/mingw64/lib/gstreamer-1.0/libgst{coreelements,audioconvert,audioresample,autodetect,dtls,nice,opus,rtp,rtpmanager,srtp,wasapi2,webrtc,app,level,vpx,openh264,videoconvertscale,winks}.dll gst-plugins/
```

- [ ] **Step 4: Commit**

```bash
git add CONTINUE.md
git commit -m "docs: update plugin list for video call support"
```

---

## Task 13: Integration Verification

- [ ] **Step 1: Full build from clean**

```bash
cd C:/build/talq
cmake C:/Projects/talk-desktop-qt -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=C:/Qt/6.8.2/mingw_64
cmake --build . --target talq
```

- [ ] **Step 2: Test outgoing audio-only call**

1. Launch app, log in as test-talq
2. Start call to kalin (no camera)
3. Verify audio works bidirectionally
4. Verify TURN servers appear in logs
5. Hang up cleanly

- [ ] **Step 3: Test incoming call + decline**

1. Have kalin call test-talq from browser
2. Verify popup appears, stays (no auto-decline)
3. Decline the call
4. Verify no 404 errors, clean state reset

- [ ] **Step 4: Test incoming call + accept**

1. Have kalin call test-talq from browser (with video)
2. Accept call
3. Verify remote video appears in CallWindow
4. Verify audio works
5. Hang up

- [ ] **Step 5: Test camera toggle**

1. During active call, click camera toggle
2. Verify camera activates (logs show "camera enabled")
3. Verify remote peer sees video in browser
4. Toggle off, verify camera stops

- [ ] **Step 6: Test device selection**

1. Open Settings, select specific mic/speaker/camera
2. Make a call
3. Verify selected devices are used (check logs for device IDs)

- [ ] **Step 7: Tag release**

```bash
git tag -a v0.8.0 -m "v0.8.0: Video Calls + Call Polish"
```

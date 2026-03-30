# Video Source Swap Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace pipeline teardown/recreate on camera toggle with in-place video source swapping via GStreamer pad probes, eliminating UI freezes.

**Architecture:** One permanent rtpvp8pay + webrtcbin video sink pad. Dummy and camera source chains are swapped upstream of a permanent vp8enc using pad probe blocking. No renegotiation, no pipeline teardown on toggle.

**Tech Stack:** GStreamer 1.x (webrtcbin, vp8enc, rtpvp8pay, pad probes), Qt 6 Widgets, C++20

---

## File Structure

| File | Action | Responsibility |
|------|--------|----------------|
| `src/core/PublishPipeline.h` | Modify | New member layout: permanent vs swappable elements, swap helpers |
| `src/core/PublishPipeline.cpp` | Rewrite video sections | Permanent tail setup, pad-probe swap in enableCamera/disableCamera |
| `src/core/CallManager.cpp` | Modify | toggleCamera calls enableCamera/disableCamera directly (both modes) |

---

### Task 1: Restructure PublishPipeline.h — permanent vs swappable members

**Files:**
- Modify: `src/core/PublishPipeline.h`

- [ ] **Step 1: Replace video member variables**

Replace the current video members block (lines 60-75) with:

```cpp
    // ── Video: permanent tail (pipeline lifetime) ──
    GstElement *m_videoEncoder = nullptr;   // vp8enc "pub-videoenc"
    GstElement *m_videoPayloader = nullptr; // rtpvp8pay "pub-videopay"
    GstPad *m_videoSinkPad = nullptr;       // permanent webrtcbin sink pad

    // ── Video: dummy source (active when camera off) ──
    GstElement *m_dummySrc = nullptr;       // videotestsrc "pub-dummyvideo"
    GstElement *m_dummyConv = nullptr;      // videoconvert "pub-dummyconv"
    GstElement *m_dummyCaps = nullptr;      // capsfilter (16x16@1fps)
    bool m_dummyActive = false;

    // ── Video: camera source (active when camera on) ──
    GstElement *m_cameraSrc = nullptr;
    GstElement *m_cameraConv = nullptr;
    GstElement *m_cameraCaps = nullptr;
    GstElement *m_tee = nullptr;
    GstElement *m_encQueue = nullptr;
    GstElement *m_previewQueue = nullptr;
    GstElement *m_previewConvert = nullptr;
    GstElement *m_previewAppsink = nullptr;
    bool m_cameraEnabled = false;

    // ── Local preview ──
    VideoFrameProvider *m_localVideoProvider = nullptr;
```

- [ ] **Step 2: Add private swap helpers**

Add after the static callback declarations:

```cpp
    void setupPermanentVideoTail();
    void activateDummySource();
    void deactivateDummySource();
    void activateCameraSource(int deviceIndex, bool hd1080);
    void deactivateCameraSource();
    static GstPadProbeReturn onSwapProbe(GstPad *pad, GstPadProbeInfo *info, gpointer userData);
```

- [ ] **Step 3: Build and verify compilation**

Run: `/c/Qt/Tools/CMake_64/bin/cmake.exe --build /c/build/talq --target talq 2>&1 | tail -5`
Expected: Compile errors (methods not yet implemented) — that's fine, just verifying header is valid.

- [ ] **Step 4: Commit**

```bash
git add src/core/PublishPipeline.h
git commit -m "refactor: restructure PublishPipeline.h — permanent vs swappable video members"
```

---

### Task 2: Implement permanent video tail setup

**Files:**
- Modify: `src/core/PublishPipeline.cpp`

- [ ] **Step 1: Add setupPermanentVideoTail method**

Add this new method. It creates the encoder + payloader + webrtcbin pad that persist for the entire pipeline lifetime:

```cpp
void PublishPipeline::setupPermanentVideoTail()
{
    m_videoEncoder = gst_element_factory_make("vp8enc", "pub-videoenc");
    m_videoPayloader = gst_element_factory_make("rtpvp8pay", "pub-videopay");

    if (!m_videoEncoder || !m_videoPayloader) {
        qWarning() << "PublishPipeline: failed to create permanent video elements";
        return;
    }

    // Low bitrate default (dummy source) — enableCamera reconfigures
    g_object_set(m_videoEncoder, "deadline", (gint64)1, "target-bitrate", 10000, nullptr);

    gst_bin_add_many(GST_BIN(m_pipeline), m_videoEncoder, m_videoPayloader, nullptr);
    gst_element_link(m_videoEncoder, m_videoPayloader);

    // Request permanent webrtcbin sink pad for video
    GstPad *paySrc = gst_element_get_static_pad(m_videoPayloader, "src");
    m_videoSinkPad = gst_element_request_pad_simple(m_webrtcbin, "sink_%u");

    // Configure transceiver
    GstWebRTCRTPTransceiver *vt = nullptr;
    g_object_get(m_videoSinkPad, "transceiver", &vt, nullptr);
    if (vt) {
        GstCaps *vc = gst_caps_from_string(
            "application/x-rtp,media=video,encoding-name=VP8,clock-rate=90000,payload=96");
        g_object_set(vt, "direction", GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY,
                     "codec-preferences", vc, nullptr);
        gst_caps_unref(vc);
        gst_object_unref(vt);
    }

    gst_pad_link(paySrc, m_videoSinkPad);
    gst_object_unref(paySrc);

    qDebug() << "PublishPipeline: permanent video tail ready (enc + pay + webrtcbin pad)";
}
```

- [ ] **Step 2: Add activateDummySource method**

```cpp
void PublishPipeline::activateDummySource()
{
    if (m_dummyActive) return;

    m_dummySrc = gst_element_factory_make("videotestsrc", "pub-dummyvideo");
    m_dummyConv = gst_element_factory_make("videoconvert", "pub-dummyconv");
    m_dummyCaps = gst_element_factory_make("capsfilter", "pub-dummycaps");

    if (!m_dummySrc || !m_dummyConv || !m_dummyCaps) {
        qWarning() << "PublishPipeline: failed to create dummy video elements";
        return;
    }

    g_object_set(m_dummySrc, "pattern", 2 /* black */, "is-live", TRUE, nullptr);
    GstCaps *caps = gst_caps_from_string("video/x-raw,width=16,height=16,framerate=1/1");
    g_object_set(m_dummyCaps, "caps", caps, nullptr);
    gst_caps_unref(caps);

    gst_bin_add_many(GST_BIN(m_pipeline), m_dummySrc, m_dummyConv, m_dummyCaps, nullptr);
    gst_element_link_many(m_dummySrc, m_dummyConv, m_dummyCaps, m_videoEncoder, nullptr);

    gst_element_sync_state_with_parent(m_dummySrc);
    gst_element_sync_state_with_parent(m_dummyConv);
    gst_element_sync_state_with_parent(m_dummyCaps);

    // Encoder: low bitrate for dummy
    g_object_set(m_videoEncoder, "target-bitrate", 10000, nullptr);

    m_dummyActive = true;
    qDebug() << "PublishPipeline: dummy video source activated (16x16 black)";
}
```

- [ ] **Step 3: Add deactivateDummySource method**

```cpp
void PublishPipeline::deactivateDummySource()
{
    if (!m_dummyActive) return;

    // Unlink dummy from encoder
    gst_element_unlink(m_dummyCaps, m_videoEncoder);

    // NULL + remove
    auto remove = [this](GstElement *&el) {
        if (!el) return;
        gst_element_set_state(el, GST_STATE_NULL);
        gst_bin_remove(GST_BIN(m_pipeline), el);
        el = nullptr;  // bin_remove takes ownership
    };
    remove(m_dummyCaps);
    remove(m_dummyConv);
    remove(m_dummySrc);

    m_dummyActive = false;
    qDebug() << "PublishPipeline: dummy video source deactivated";
}
```

- [ ] **Step 4: Commit**

```bash
git add src/core/PublishPipeline.cpp
git commit -m "feat: permanent video tail + dummy source activate/deactivate"
```

---

### Task 3: Rewrite start() to use permanent tail + dummy

**Files:**
- Modify: `src/core/PublishPipeline.cpp`

- [ ] **Step 1: Replace the dummy video setup in start()**

In the `start()` method, replace the entire block from `if (!m_cameraEnabled) {` through the dummy video setup (lines 157-197) with:

```cpp
    // Set up permanent video encoder + payloader + webrtcbin pad
    setupPermanentVideoTail();

    // Try camera if requested
    if (withVideo) {
        enableCamera(videoDeviceIndex, hd1080);
    }

    // If camera not active, use dummy source
    if (!m_cameraEnabled) {
        activateDummySource();
    }
```

Also remove the old `enableCamera` call that was at line 151 and any duplicate dummy code.

- [ ] **Step 2: Remove the old enableCamera call at start**

The old code at lines 150-156 (`if (withVideo) { enableCamera(...); ... }`) should be removed since it's replaced by the block above.

- [ ] **Step 3: Build and verify**

Run: `/c/Qt/Tools/CMake_64/bin/cmake.exe --build /c/build/talq --target talq 2>&1 | tail -5`
Expected: May have errors from the old enableCamera/disableCamera — those are rewritten in the next task.

- [ ] **Step 4: Commit**

```bash
git add src/core/PublishPipeline.cpp
git commit -m "refactor: start() uses permanent video tail + dummy source"
```

---

### Task 4: Rewrite enableCamera with pad-probe swap

**Files:**
- Modify: `src/core/PublishPipeline.cpp`

- [ ] **Step 1: Implement the pad-probe callback**

```cpp
struct SwapData {
    PublishPipeline *self;
    int deviceIndex;
    bool hd1080;
};

GstPadProbeReturn PublishPipeline::onSwapProbe(GstPad *pad, GstPadProbeInfo *info, gpointer userData)
{
    Q_UNUSED(pad)
    Q_UNUSED(info)
    auto *data = static_cast<SwapData *>(userData);
    auto *self = data->self;

    // 1. Remove dummy source (we're on the streaming thread, pipeline is blocked)
    self->deactivateDummySource();

    // 2. Build and activate camera source
    self->activateCameraSource(data->deviceIndex, data->hd1080);

    delete data;

    // Remove probe (let data flow again)
    return GST_PAD_PROBE_REMOVE;
}
```

- [ ] **Step 2: Rewrite enableCamera**

Replace the entire existing `enableCamera` method with:

```cpp
void PublishPipeline::enableCamera(int deviceIndex, bool hd1080)
{
    if (m_cameraEnabled || !m_pipeline || !m_videoEncoder) return;

    qDebug() << "PublishPipeline: enabling camera, device" << deviceIndex;

    if (m_dummyActive) {
        // Pipeline is running — use pad probe to safely swap on streaming thread
        GstPad *encSink = gst_element_get_static_pad(m_videoEncoder, "sink");
        auto *data = new SwapData{this, deviceIndex, hd1080};
        gst_pad_add_probe(encSink, GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM,
                          onSwapProbe, data, nullptr);
        gst_object_unref(encSink);
    } else {
        // Pipeline not yet playing (called from start()) — direct setup
        activateCameraSource(deviceIndex, hd1080);
    }
}
```

- [ ] **Step 3: Implement activateCameraSource**

```cpp
void PublishPipeline::activateCameraSource(int deviceIndex, bool hd1080)
{
    bool testVideo = !qEnvironmentVariableIsEmpty("TALQ_TEST_AUDIO");

    // Create camera source
    if (testVideo) {
        m_cameraSrc = gst_element_factory_make("videotestsrc", nullptr);
        if (m_cameraSrc)
            g_object_set(m_cameraSrc, "is-live", TRUE, "pattern", 0 /* SMPTE */, nullptr);
    }
    if (!m_cameraSrc) {
        m_cameraSrc = gst_element_factory_make("mfvideosrc", nullptr);
        if (!m_cameraSrc)
            m_cameraSrc = gst_element_factory_make("ksvideosrc", nullptr);
    }
    if (!m_cameraSrc) {
        QMetaObject::invokeMethod(this, [this]() { emit cameraError("No camera plugin"); }, Qt::QueuedConnection);
        // Reactivate dummy if we deactivated it
        if (!m_dummyActive) activateDummySource();
        return;
    }
    if (!testVideo)
        g_object_set(m_cameraSrc, "device-index", deviceIndex, nullptr);

    m_cameraConv = gst_element_factory_make("videoconvert", nullptr);
    m_cameraCaps = gst_element_factory_make("capsfilter", nullptr);

    if (testVideo) {
        int w = hd1080 ? 1920 : 1280, h = hd1080 ? 1080 : 720;
        QString capsStr = QString("video/x-raw,width=%1,height=%2,framerate=30/1").arg(w).arg(h);
        GstCaps *caps = gst_caps_from_string(capsStr.toUtf8().constData());
        g_object_set(m_cameraCaps, "caps", caps, nullptr);
        gst_caps_unref(caps);
    } else {
        GstCaps *caps = gst_caps_from_string("video/x-raw");
        g_object_set(m_cameraCaps, "caps", caps, nullptr);
        gst_caps_unref(caps);
    }

    // Create tee + preview branch
    m_tee = gst_element_factory_make("tee", "camera-tee");
    m_encQueue = gst_element_factory_make("queue", "enc-queue");
    m_previewQueue = gst_element_factory_make("queue", "preview-queue");
    m_previewConvert = gst_element_factory_make("videoconvert", "preview-convert");
    m_previewAppsink = gst_element_factory_make("appsink", "preview-sink");

    if (m_encQueue) g_object_set(m_encQueue, "leaky", 2, "max-size-buffers", 3, nullptr);
    if (m_previewQueue) g_object_set(m_previewQueue, "leaky", 2, "max-size-buffers", 2, nullptr);

    bool hasTee = m_tee && m_encQueue && m_previewQueue && m_previewConvert && m_previewAppsink;

    if (hasTee) {
        GstCaps *previewCaps = gst_caps_from_string("video/x-raw,format=I420");
        g_object_set(m_previewAppsink, "emit-signals", TRUE, "caps", previewCaps,
                     "drop", TRUE, "max-buffers", 1, nullptr);
        gst_caps_unref(previewCaps);
        g_signal_connect(m_previewAppsink, "new-sample", G_CALLBACK(onPreviewSample), this);

        gst_bin_add_many(GST_BIN(m_pipeline), m_cameraSrc, m_cameraConv, m_cameraCaps,
            m_tee, m_encQueue, m_previewQueue, m_previewConvert, m_previewAppsink, nullptr);

        gst_element_link_many(m_cameraSrc, m_cameraConv, m_cameraCaps, m_tee, nullptr);
        gst_element_link_many(m_encQueue, m_videoEncoder, nullptr);
        gst_element_link_many(m_previewQueue, m_previewConvert, m_previewAppsink, nullptr);

        // Link tee src pads
        GstPad *teeSrcEnc = gst_element_request_pad_simple(m_tee, "src_%u");
        GstPad *encQueueSink = gst_element_get_static_pad(m_encQueue, "sink");
        gst_pad_link(teeSrcEnc, encQueueSink);
        gst_object_unref(teeSrcEnc);
        gst_object_unref(encQueueSink);

        GstPad *teeSrcPreview = gst_element_request_pad_simple(m_tee, "src_%u");
        GstPad *previewQueueSink = gst_element_get_static_pad(m_previewQueue, "sink");
        gst_pad_link(teeSrcPreview, previewQueueSink);
        gst_object_unref(teeSrcPreview);
        gst_object_unref(previewQueueSink);
    } else {
        // No preview — direct chain
        gst_bin_add_many(GST_BIN(m_pipeline), m_cameraSrc, m_cameraConv, m_cameraCaps, nullptr);
        gst_element_link_many(m_cameraSrc, m_cameraConv, m_cameraCaps, m_videoEncoder, nullptr);
    }

    // Sync all new elements to pipeline state
    auto syncEl = [](GstElement *el) { if (el) gst_element_sync_state_with_parent(el); };
    syncEl(m_cameraSrc);
    syncEl(m_cameraConv);
    syncEl(m_cameraCaps);
    syncEl(m_tee);
    syncEl(m_encQueue);
    syncEl(m_previewQueue);
    syncEl(m_previewConvert);
    syncEl(m_previewAppsink);

    // Reconfigure encoder for camera bitrate
    int bitrate = hd1080 ? 3000000 : 1500000;
    g_object_set(m_videoEncoder, "target-bitrate", bitrate, nullptr);

    m_cameraEnabled = true;
    qDebug() << "PublishPipeline: camera source activated, bitrate=" << bitrate;

    QMetaObject::invokeMethod(this, [this]() {
        emit cameraChanged();
    }, Qt::QueuedConnection);
}
```

- [ ] **Step 4: Add cameraChanged signal to header**

In PublishPipeline.h signals section, add:

```cpp
    void cameraChanged();  // emitted when camera source swaps in/out
```

- [ ] **Step 5: Build**

Run: `/c/Qt/Tools/CMake_64/bin/cmake.exe --build /c/build/talq --target talq 2>&1 | tail -5`

- [ ] **Step 6: Commit**

```bash
git add src/core/PublishPipeline.h src/core/PublishPipeline.cpp
git commit -m "feat: enableCamera via pad-probe swap — no pipeline teardown"
```

---

### Task 5: Rewrite disableCamera with pad-probe swap

**Files:**
- Modify: `src/core/PublishPipeline.cpp`

- [ ] **Step 1: Add deactivateCameraSource method**

```cpp
void PublishPipeline::deactivateCameraSource()
{
    // Unlink camera chain from encoder
    if (m_encQueue)
        gst_element_unlink(m_encQueue, m_videoEncoder);
    else if (m_cameraCaps)
        gst_element_unlink(m_cameraCaps, m_videoEncoder);

    if (m_previewAppsink)
        g_signal_handlers_disconnect_by_data(m_previewAppsink, this);

    auto remove = [this](GstElement *&el) {
        if (!el) return;
        gst_element_set_state(el, GST_STATE_NULL);
        gst_bin_remove(GST_BIN(m_pipeline), el);
        el = nullptr;
    };

    remove(m_previewAppsink);
    remove(m_previewConvert);
    remove(m_previewQueue);
    remove(m_tee);
    remove(m_encQueue);
    remove(m_cameraCaps);
    remove(m_cameraConv);
    remove(m_cameraSrc);

    m_cameraEnabled = false;
    qDebug() << "PublishPipeline: camera source deactivated";
}
```

- [ ] **Step 2: Rewrite disableCamera**

Replace the entire existing `disableCamera` method with:

```cpp
void PublishPipeline::disableCamera()
{
    if (!m_cameraEnabled) return;

    qDebug() << "PublishPipeline: disabling camera";

    if (m_pipeline && m_videoEncoder) {
        // Pipeline is running — use pad probe to safely swap
        GstPad *encSink = gst_element_get_static_pad(m_videoEncoder, "sink");
        gst_pad_add_probe(encSink, GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM,
            [](GstPad *, GstPadProbeInfo *, gpointer userData) -> GstPadProbeReturn {
                auto *self = static_cast<PublishPipeline *>(userData);
                self->deactivateCameraSource();
                self->activateDummySource();
                return GST_PAD_PROBE_REMOVE;
            }, this, nullptr);
        gst_object_unref(encSink);
    } else {
        // Not running — direct teardown
        deactivateCameraSource();
    }

    QMetaObject::invokeMethod(this, [this]() {
        m_localVideoProvider->feedFrame(nullptr);  // clear preview
        emit cameraChanged();
    }, Qt::QueuedConnection);
}
```

- [ ] **Step 3: Update cleanup() — don't release m_videoSinkPad (pipeline teardown handles it)**

In `cleanup()`, the video cleanup section should just clean up the swappable sources and the permanent tail. Replace all the video-related cleanup with:

```cpp
    // Clean up swappable video sources
    if (m_cameraEnabled)
        deactivateCameraSource();
    if (m_dummyActive)
        deactivateDummySource();

    // Permanent tail cleaned up by pipeline teardown below
    m_videoEncoder = nullptr;
    m_videoPayloader = nullptr;
    m_videoSinkPad = nullptr;
```

Note: `cleanup()` already calls `gst_element_set_state(NULL)` on the whole pipeline which cleans up all elements including the permanent tail and webrtcbin pad. We just need to null our pointers.

- [ ] **Step 4: Update stop() — remove disableCamera call**

In `stop()`, remove the `disableCamera()` call since `cleanup()` now handles all video teardown:

```cpp
void PublishPipeline::stop()
{
    if (!m_running) return;
    cleanup();
    m_running = false;
}
```

- [ ] **Step 5: Build and verify**

Run: `/c/Qt/Tools/CMake_64/bin/cmake.exe --build /c/build/talq --target talq 2>&1 | tail -5`

- [ ] **Step 6: Commit**

```bash
git add src/core/PublishPipeline.cpp
git commit -m "feat: disableCamera via pad-probe swap + cleanup refactor"
```

---

### Task 6: Update CallManager — direct enable/disable, no forceReconnect for camera

**Files:**
- Modify: `src/core/CallManager.cpp`

- [ ] **Step 1: Change toggleCamera to call enableCamera/disableCamera directly**

Replace the MCU branch in `toggleCamera`:

```cpp
void CallManager::toggleCamera() {
    m_cameraOn = !m_cameraOn;
    emit cameraChanged();

    if (m_publishPipeline) {
        if (m_cameraOn) {
            m_publishPipeline->enableCamera(videoDeviceIndex(), preferHd1080());
            m_localVideoProvider = m_publishPipeline->localVideoProvider();
        } else {
            m_publishPipeline->disableCamera();
            m_localVideoProvider = nullptr;
        }
        emit localVideoProviderChanged();
    } else if (m_useP2P && m_peerPipeline) {
        m_cameraOn ? m_peerPipeline->enableCamera(videoDeviceIndex(), preferHd1080())
                   : m_peerPipeline->disableCamera();
    }

    broadcastMediaState("video", m_cameraOn);
    updateCallFlags();
}
```

- [ ] **Step 2: Keep forceReconnectPublisher for network reconnection only**

No changes to `forceReconnectPublisher` itself — it's still used for network reconnection (not camera toggle). Just verify it still compiles.

- [ ] **Step 3: Build and test manually**

Run: `/c/Qt/Tools/CMake_64/bin/cmake.exe --build /c/build/talq --target talq 2>&1 | tail -5`
Then: `cd /c/build/talq && bash /c/src/talk-desktop-qt/scripts/deploy-dev.sh`

Manual test: start a call, toggle camera on/off. Should not freeze.

- [ ] **Step 4: Commit**

```bash
git add src/core/CallManager.cpp
git commit -m "feat: toggleCamera uses in-place swap — no forceReconnectPublisher"
```

---

### Task 7: SSRC sync update — use permanent payloader name

**Files:**
- Modify: `src/core/PublishPipeline.cpp`

- [ ] **Step 1: Update SSRC sync in onOfferCreated**

The video payloader is now always `"pub-videopay"`. Update the SSRC sync section:

```cpp
        if (videoSsrc) {
            GstElement *pay = gst_bin_get_by_name(GST_BIN(self->m_pipeline), "pub-videopay");
            if (pay) {
                g_object_set(pay, "ssrc", videoSsrc, nullptr);
                gst_object_unref(pay);
                qDebug() << "PublishPipeline: synced video SSRC to" << videoSsrc;
            }
        }
```

Remove the fallback lookup for `"pub-dummypay"` — it no longer exists.

- [ ] **Step 2: Build**

Run: `/c/Qt/Tools/CMake_64/bin/cmake.exe --build /c/build/talq --target talq 2>&1 | tail -5`

- [ ] **Step 3: Commit**

```bash
git add src/core/PublishPipeline.cpp
git commit -m "fix: SSRC sync uses permanent pub-videopay name"
```

---

### Task 8: Cleanup and docs

**Files:**
- Modify: `CONTINUE.md`
- Modify: `CHANGELOG.md`

- [ ] **Step 1: Update CONTINUE.md**

Remove "Camera doesn't work on this laptop" from next steps. Add entry to "What was done" section.

- [ ] **Step 2: Update CHANGELOG.md**

Add entry for the video source swap feature.

- [ ] **Step 3: Commit**

```bash
git add CONTINUE.md CHANGELOG.md
git commit -m "docs: video source swap — camera toggle without pipeline teardown"
```

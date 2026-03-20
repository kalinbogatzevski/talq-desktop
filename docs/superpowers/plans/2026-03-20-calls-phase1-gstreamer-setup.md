# Phase 1: GStreamer Setup & Audio Pipeline Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Install GStreamer, integrate it into the TalQ build system, and verify a working local audio pipeline (mic → speaker loopback) as proof of concept.

**Architecture:** GStreamer is added as a system dependency found via pkg-config. A new `GstPipeline` C++ class wraps pipeline construction/teardown. `gst_init()` is called in `main()` before the QML engine. A `MediaDeviceManager` class enumerates audio/video devices. Phase 1 does NOT involve networking or WebRTC — just local audio capture and playback to prove the stack works.

**Tech Stack:** GStreamer 1.24+ (MSYS2 MinGW packages), Qt 6.8.2, MinGW 13.1, C++20

**Spec:** `docs/superpowers/specs/2026-03-20-calls-design.md`

---

### File Structure

| File | Action | Responsibility |
|------|--------|---------------|
| `CMakeLists.txt` | Modify | Add GStreamer + Qt Multimedia dependencies |
| `src/main.cpp` | Modify | Add `gst_init()` call before QML engine |
| `src/core/GstPipeline.h` | Create | Pipeline builder — construct/start/stop GStreamer pipelines |
| `src/core/GstPipeline.cpp` | Create | Implementation |
| `src/core/MediaDeviceManager.h` | Create | Device enumeration via GstDeviceMonitor |
| `src/core/MediaDeviceManager.cpp` | Create | Implementation |

---

### Task 1: Install GStreamer via MSYS2

**Files:** None (system dependency)

This task installs GStreamer packages into the MSYS2 MinGW64 environment so that pkg-config can find them.

- [ ] **Step 1: Check if MSYS2 MinGW shell is available**

Run: `C:/msys64/msys2_shell.cmd -mingw64 -defterm -no-start -c "echo ok"`

If MSYS2 is not installed, download from https://www.msys2.org/ and install to `C:\msys64`.

- [ ] **Step 2: Install GStreamer packages**

Run in MSYS2 MinGW64 shell:
```bash
pacman -S --noconfirm \
  mingw-w64-x86_64-gstreamer \
  mingw-w64-x86_64-gst-plugins-base \
  mingw-w64-x86_64-gst-plugins-good \
  mingw-w64-x86_64-gst-plugins-bad \
  mingw-w64-x86_64-gst-plugins-ugly
```

- [ ] **Step 3: Verify pkg-config finds GStreamer**

Run: `pkg-config --cflags --libs gstreamer-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0`

Expected: Include paths and library flags printed without errors.

If pkg-config is not in PATH, add `C:/msys64/mingw64/lib/pkgconfig` to `PKG_CONFIG_PATH` env var and `C:/msys64/mingw64/bin` to `PATH`.

- [ ] **Step 4: Verify key GStreamer elements exist**

Run: `gst-inspect-1.0 wasapi2src` and `gst-inspect-1.0 audioconvert`

Expected: Plugin details printed for each element.

- [ ] **Step 5: Document the GStreamer path for CMake**

Note the paths needed for CMake:
- Include: `C:/msys64/mingw64/include/gstreamer-1.0` (and sub-includes)
- Libs: `C:/msys64/mingw64/lib`
- Bin: `C:/msys64/mingw64/bin` (for GStreamer DLLs at runtime)

No commit yet — system setup.

---

### Task 2: Add GStreamer to CMakeLists.txt

**Files:**
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add pkg-config GStreamer detection to CMakeLists.txt**

After the `find_package(Qt6 ...)` block (line 19), add:

```cmake
# GStreamer (for calls — webrtc/sdp modules added in Phase 2)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED gstreamer-1.0)
```

- [ ] **Step 2: Add Qt Multimedia to find_package**

Add `Multimedia` to the Qt6 find_package components list:

```cmake
find_package(Qt6 REQUIRED COMPONENTS
    Core Gui Network Qml Quick QuickControls2 Sql WebSockets Widgets
    Multimedia
)
```

- [ ] **Step 3: Add GStreamer include dirs and link libraries**

After `target_include_directories(talq PRIVATE src)` (line 77), add these NEW lines:

```cmake
target_include_directories(talq PRIVATE ${GST_INCLUDE_DIRS})
target_link_directories(talq PRIVATE ${GST_LIBRARY_DIRS})
```

In the EXISTING `target_link_libraries(talq PRIVATE ...)` block (line 79-89), add two new entries. Do NOT replace the block — keep all existing libraries including the `if(WIN32)` block with `dwmapi winmm` below it:

```cmake
    Qt6::Multimedia
    ${GST_LIBRARIES}
```

- [ ] **Step 4: Reconfigure and verify CMake finds everything**

Run:
```bash
cd C:/build/talk-qt
C:/Qt/Tools/CMake_64/bin/cmake.exe -DCMAKE_BUILD_TYPE=Release C:/src/talk-desktop-qt
```

Expected: No errors about GStreamer or Qt Multimedia. Output should include GStreamer paths.

- [ ] **Step 5: Build to verify no regressions**

Run: `cmake --build . --target talq`

Expected: Builds successfully with no new errors.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt
git commit -m "build: add GStreamer and Qt Multimedia dependencies for calls"
```

---

### Task 3: Add gst_init() to main.cpp

**Files:**
- Modify: `src/main.cpp`

- [ ] **Step 1: Add GStreamer include**

At the top of `main.cpp`, after the existing includes (before `int main`):

```cpp
#include <gst/gst.h>
```

- [ ] **Step 2: Add gst_init before QML engine**

In `main()`, right after the `QApplication app(argc, argv);` line (line 32), add:

```cpp
    // Initialize GStreamer for WebRTC calls
    gst_init(&argc, &argv);
```

This must be before `QQmlApplicationEngine engine;` (line 70).

- [ ] **Step 3: Build and run to verify no crash**

Run: Build and launch TalQ. It should start normally with GStreamer initialized silently in the background.

- [ ] **Step 4: Commit**

```bash
git add src/main.cpp
git commit -m "feat: initialize GStreamer in main() for call support"
```

---

### Task 4: Create GstPipeline class — audio loopback

**Files:**
- Create: `src/core/GstPipeline.h`
- Create: `src/core/GstPipeline.cpp`
- Modify: `CMakeLists.txt` (add source files)

- [ ] **Step 1: Create GstPipeline.h**

```cpp
#pragma once

#include <QObject>
#include <gst/gst.h>

/**
 * Wraps GStreamer pipeline construction and lifecycle.
 * Phase 1: audio loopback (mic → speaker) for testing.
 * Later phases add webrtcbin for actual calls.
 */
class GstPipeline : public QObject
{
    Q_OBJECT

public:
    explicit GstPipeline(QObject *parent = nullptr);
    ~GstPipeline() override;

    // Build and start a local audio loopback pipeline (mic → speaker)
    bool startAudioLoopback();

    // Build an audio-only call pipeline with webrtcbin (Phase 2+)
    // bool startAudioCall();

    // Stop and tear down the current pipeline
    void stop();

    bool isRunning() const { return m_running; }

signals:
    void error(const QString &message);

private:
    GstElement *m_pipeline = nullptr;
    bool m_running = false;

    void cleanup();
};
```

- [ ] **Step 2: Create GstPipeline.cpp**

```cpp
#include "core/GstPipeline.h"
#include <QDebug>

GstPipeline::GstPipeline(QObject *parent)
    : QObject(parent)
{
}

GstPipeline::~GstPipeline()
{
    stop();
}

bool GstPipeline::startAudioLoopback()
{
    if (m_running) {
        qWarning() << "GstPipeline: already running";
        return false;
    }

    // Build: wasapi2src → audioconvert → audioresample → wasapi2sink
    m_pipeline = gst_parse_launch(
        "wasapi2src ! audioconvert ! audioresample ! wasapi2sink",
        nullptr
    );

    if (!m_pipeline) {
        emit error("Failed to create audio loopback pipeline");
        return false;
    }

    GstStateChangeReturn ret = gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        emit error("Failed to start audio loopback pipeline");
        cleanup();
        return false;
    }

    m_running = true;
    qDebug() << "GstPipeline: audio loopback started";
    return true;
}

void GstPipeline::stop()
{
    if (!m_running)
        return;

    cleanup();
    m_running = false;
    qDebug() << "GstPipeline: stopped";
}

void GstPipeline::cleanup()
{
    if (m_pipeline) {
        gst_element_set_state(m_pipeline, GST_STATE_NULL);
        gst_object_unref(m_pipeline);
        m_pipeline = nullptr;
    }
}
```

- [ ] **Step 3: Add source files to CMakeLists.txt**

Add to `qt_add_executable(talq ...)`:
```
    src/core/GstPipeline.cpp
```

- [ ] **Step 4: Build**

Run: `cmake --build . --target talq`

Expected: Compiles with no errors. The class is not yet used by anything.

- [ ] **Step 5: Commit**

```bash
git add src/core/GstPipeline.h src/core/GstPipeline.cpp CMakeLists.txt
git commit -m "feat: add GstPipeline class with audio loopback"
```

---

### Task 5: Create MediaDeviceManager class

**Files:**
- Create: `src/core/MediaDeviceManager.h`
- Create: `src/core/MediaDeviceManager.cpp`
- Modify: `CMakeLists.txt` (add source files)

- [ ] **Step 1: Create MediaDeviceManager.h**

```cpp
#pragma once

#include <QObject>
#include <QStringList>
#include <gst/gst.h>

struct MediaDevice {
    QString id;       // GStreamer device name/path
    QString name;     // Human-readable display name
    QString type;     // "audio-input", "audio-output", "video-input"
};

/**
 * Enumerates audio/video devices via GStreamer's GstDeviceMonitor.
 * Provides device lists to QML for device picker UI.
 */
class MediaDeviceManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QStringList audioInputNames READ audioInputNames NOTIFY devicesChanged)
    Q_PROPERTY(QStringList audioOutputNames READ audioOutputNames NOTIFY devicesChanged)
    Q_PROPERTY(QStringList videoInputNames READ videoInputNames NOTIFY devicesChanged)

public:
    explicit MediaDeviceManager(QObject *parent = nullptr);
    ~MediaDeviceManager() override;

    Q_INVOKABLE void refresh();

    QStringList audioInputNames() const;
    QStringList audioOutputNames() const;
    QStringList videoInputNames() const;

    const QVector<MediaDevice> &audioInputs() const { return m_audioInputs; }
    const QVector<MediaDevice> &audioOutputs() const { return m_audioOutputs; }
    const QVector<MediaDevice> &videoInputs() const { return m_videoInputs; }

signals:
    void devicesChanged();

private:
    QVector<MediaDevice> m_audioInputs;
    QVector<MediaDevice> m_audioOutputs;
    QVector<MediaDevice> m_videoInputs;
};
```

- [ ] **Step 2: Create MediaDeviceManager.cpp**

```cpp
#include "core/MediaDeviceManager.h"
#include <QDebug>

MediaDeviceManager::MediaDeviceManager(QObject *parent)
    : QObject(parent)
{
}

MediaDeviceManager::~MediaDeviceManager() = default;

void MediaDeviceManager::refresh()
{
    m_audioInputs.clear();
    m_audioOutputs.clear();
    m_videoInputs.clear();

    GstDeviceMonitor *monitor = gst_device_monitor_new();

    // Add filters for audio and video devices
    gst_device_monitor_add_filter(monitor, "Audio/Source", nullptr);
    gst_device_monitor_add_filter(monitor, "Audio/Sink", nullptr);
    gst_device_monitor_add_filter(monitor, "Video/Source", nullptr);

    if (!gst_device_monitor_start(monitor)) {
        qWarning() << "MediaDeviceManager: failed to start device monitor";
        gst_object_unref(monitor);
        return;
    }

    GList *devices = gst_device_monitor_get_devices(monitor);
    for (GList *it = devices; it; it = it->next) {
        GstDevice *dev = GST_DEVICE(it->data);
        gchar *name = gst_device_get_display_name(dev);
        gchar *cls = gst_device_get_device_class(dev);

        MediaDevice md;
        md.name = QString::fromUtf8(name);
        md.id = md.name;  // Use display name as ID for now

        QString deviceClass = QString::fromUtf8(cls);
        if (deviceClass.contains("Source") && deviceClass.contains("Audio")) {
            md.type = "audio-input";
            m_audioInputs.append(md);
        } else if (deviceClass.contains("Sink") && deviceClass.contains("Audio")) {
            md.type = "audio-output";
            m_audioOutputs.append(md);
        } else if (deviceClass.contains("Source") && deviceClass.contains("Video")) {
            md.type = "video-input";
            m_videoInputs.append(md);
        }

        g_free(name);
        g_free(cls);
        gst_object_unref(dev);
    }
    g_list_free(devices);

    gst_device_monitor_stop(monitor);
    gst_object_unref(monitor);

    qDebug() << "MediaDeviceManager: found"
             << m_audioInputs.size() << "mic(s),"
             << m_audioOutputs.size() << "speaker(s),"
             << m_videoInputs.size() << "camera(s)";

    emit devicesChanged();
}

QStringList MediaDeviceManager::audioInputNames() const
{
    QStringList names;
    for (const auto &d : m_audioInputs)
        names << d.name;
    return names;
}

QStringList MediaDeviceManager::audioOutputNames() const
{
    QStringList names;
    for (const auto &d : m_audioOutputs)
        names << d.name;
    return names;
}

QStringList MediaDeviceManager::videoInputNames() const
{
    QStringList names;
    for (const auto &d : m_videoInputs)
        names << d.name;
    return names;
}
```

- [ ] **Step 3: Add source files to CMakeLists.txt**

Add to `qt_add_executable(talq ...)`:
```
    src/core/MediaDeviceManager.cpp
```

- [ ] **Step 4: Build**

Run: `cmake --build . --target talq`

Expected: Compiles. Class not yet used.

- [ ] **Step 5: Commit**

```bash
git add src/core/MediaDeviceManager.h src/core/MediaDeviceManager.cpp CMakeLists.txt
git commit -m "feat: add MediaDeviceManager for audio/video device enumeration"
```

---

### Task 6: Wire up and test — audio loopback smoke test

**Files:**
- Modify: `src/main.cpp`

This task temporarily wires GstPipeline and MediaDeviceManager into main.cpp for a manual smoke test. After verifying, the test code is removed — the real wiring happens in Phase 2 via CallManager.

- [ ] **Step 1: Add includes and instantiation in main.cpp**

After the existing includes, add:
```cpp
#include "core/GstPipeline.h"
#include "core/MediaDeviceManager.h"
```

After the existing services block (after `SignalingClient signaling(&api);`, line 67), add the instantiation:
```cpp
    // Call support (Phase 1: GStreamer proof-of-concept)
    MediaDeviceManager deviceManager;
    deviceManager.refresh();
    GstPipeline gstPipeline;
```

Then SEPARATELY, after the existing `setContextProperty` calls (after line 80, where `notifications` is exposed), add the QML exposure:
```cpp
    engine.rootContext()->setContextProperty("deviceManager", &deviceManager);
```

IMPORTANT: The `setContextProperty` call must be AFTER `QQmlApplicationEngine engine;` (line 70), not next to the instantiation.

- [ ] **Step 2: Reconfigure CMake (new files in build)**

Run: `cmake -DCMAKE_BUILD_TYPE=Release C:/src/talk-desktop-qt` in the build directory.

- [ ] **Step 3: Build and run**

Build and launch TalQ. Check console/debug output for:
```
MediaDeviceManager: found N mic(s), N speaker(s), N camera(s)
```

This confirms GStreamer device enumeration works on the system.

- [ ] **Step 4: (Optional manual test) Audio loopback**

To test the audio pipeline, temporarily add after `gstPipeline` creation:
```cpp
    gstPipeline.startAudioLoopback();
```

Build, run, speak into your mic — you should hear yourself through your speakers with slight delay. Then remove this line after confirming.

- [ ] **Step 5: Remove the temporary loopback test line, keep device manager**

Remove the `gstPipeline.startAudioLoopback()` line. Keep the `MediaDeviceManager` instantiation and QML exposure — it will be used by the call UI.

- [ ] **Step 6: Commit**

```bash
git add src/main.cpp
git commit -m "feat: wire GstPipeline and MediaDeviceManager into main, verify GStreamer works"
```

---

### Task 7: Verify GStreamer works without MSYS2 in PATH (deferred)

**Note:** Full GStreamer DLL bundling and installer changes are deferred to after Phase 3 (when calls actually work end-to-end). For Phase 1 development, GStreamer runs from the MSYS2 installation via PATH. No commit needed for this task — it's a checkpoint.

- [ ] **Step 1: Confirm MSYS2 bin is in PATH for development**

Ensure `C:/msys64/mingw64/bin` is in the system or user PATH so GStreamer DLLs are found at runtime during development.

- [ ] **Step 2: Verify talq.exe runs and enumerates devices**

Launch talq.exe with MSYS2 in PATH. Confirm device enumeration output in debug log.

---

## Phase 1 Complete Checklist

After all tasks:
- [ ] GStreamer installed and found by CMake
- [ ] `gst_init()` called in main.cpp
- [ ] GstPipeline class can build and run an audio loopback pipeline
- [ ] MediaDeviceManager enumerates mics, speakers, cameras
- [ ] Qt Multimedia added to build dependencies
- [ ] App starts and runs normally with GStreamer initialized
- [ ] GStreamer DLLs identified for installer bundling

**Next:** Phase 2 — SignalingClient extensions + CallManager state machine

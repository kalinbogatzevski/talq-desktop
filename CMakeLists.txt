cmake_minimum_required(VERSION 3.22)
project(talq VERSION 0.52.15 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS
    Concurrent
    Core
    Gui
    Multimedia
    Network
    OpenGL
    Sql
    Svg
    WebSockets
    Widgets
)

# GStreamer (for calls)
set(GSTREAMER_ROOT "C:/msys64/mingw64" CACHE PATH "GStreamer installation root")
set(GST_INCLUDE_DIRS
    "${GSTREAMER_ROOT}/include/gstreamer-1.0"
    "${GSTREAMER_ROOT}/include/glib-2.0"
    "${GSTREAMER_ROOT}/lib/glib-2.0/include"
    "${GSTREAMER_ROOT}/include"
)
set(GST_LIBRARY_DIRS "${GSTREAMER_ROOT}/lib")
set(GST_LIBRARIES gstreamer-1.0 gstsdp-1.0 gstwebrtc-1.0 gstapp-1.0 gstrtp-1.0 gobject-2.0 glib-2.0 intl)

find_package(Python3 COMPONENTS Interpreter)

add_custom_command(
    OUTPUT  ${CMAKE_SOURCE_DIR}/src/core/EmojiData_generated.cpp
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/scripts/gen-emoji-data.py
            ${CMAKE_SOURCE_DIR}/resources/emoji-data-source.json
            ${CMAKE_SOURCE_DIR}/src/core/EmojiData_generated.cpp
    DEPENDS ${CMAKE_SOURCE_DIR}/resources/emoji-data-source.json
            ${CMAKE_SOURCE_DIR}/scripts/gen-emoji-data.py
    COMMENT "Regenerating EmojiData_generated.cpp"
)

qt_add_executable(talq
    src/main.cpp
    src/core/ApiClient.cpp
    src/core/AuthManager.cpp
    src/core/MessageCache.cpp
    src/core/MessagePoller.cpp
    src/core/NotificationManager.cpp
    src/core/PushClient.cpp
    src/core/SignalingClient.cpp
    src/models/Conversation.cpp
    src/models/Message.cpp
    src/models/ConversationListModel.cpp
    src/models/MessageListModel.cpp
    src/models/ThreadListModel.cpp
    src/core/PublishPipeline.cpp
    src/core/SubscribePipeline.cpp
    src/core/SubscribeWebrtcSrc.cpp
    src/core/Diagnostics.cpp
    src/core/WasapiDucking.cpp
    src/core/VideoFrameProvider.h
    src/core/VideoFrameProvider.cpp
    src/core/MediaDeviceManager.cpp
    src/core/MicTester.h
    src/core/MicTester.cpp
    src/core/UserStatusManager.h
    src/core/UserStatusManager.cpp
    src/core/CallManager.cpp
    src/core/CallParticipant.cpp
    src/core/PeerPipeline.cpp
    src/core/ScreenSharePipeline.cpp
    src/core/CallSignaling.cpp
    src/core/DebugMonitor.cpp
    src/core/AppSettings.cpp
    src/core/UpdateChecker.h
    src/core/UpdateChecker.cpp
    src/painter/PainterTheme.cpp
    src/painter/LayoutEngine.cpp
    src/painter/ChatPainter.cpp
    src/painter/SidebarPainter.cpp
    src/painter/HeaderPainter.cpp
    src/painter/ThreadsPainter.cpp
    src/painter/EmojiTextRenderer.h
    src/painter/EmojiTextRenderer.cpp
    src/painter/VectorIcons.h
    src/ui/MainWindow.cpp
    src/ui/ComposerWidget.cpp
    src/ui/LoginWidget.cpp
    src/ui/SettingsDialog.cpp
    src/ui/BgPreviewSource.cpp
    src/ui/BgPreviewSource.h
    src/ui/CallStage.cpp
    src/ui/CallWindow.cpp
    src/ui/StatusPopover.h
    src/ui/StatusPopover.cpp
    src/ui/AppStyle.h
    src/ui/AppStyle.cpp
    src/ui/TalqIconButton.h
    src/ui/TalqIconButton.cpp
    src/ui/SharePickerDialog.cpp
    src/ui/SelectionBarWidget.h
    src/ui/SelectionBarWidget.cpp
    src/ui/ConversationPickerDialog.h
    src/ui/ConversationPickerDialog.cpp
    src/ui/ScheduledMessagesDialog.h
    src/ui/ScheduledMessagesDialog.cpp
    src/ui/NotificationPopup.h
    src/ui/NotificationPopup.cpp
    src/ui/NotificationStack.h
    src/ui/NotificationStack.cpp
    src/ui/EmojiPickerWidget.h
    src/ui/EmojiPickerWidget.cpp
    src/ui/ImageViewerDialog.h
    src/ui/ImageViewerDialog.cpp
    src/core/EmojiData.h
    src/core/EmojiData.cpp
    src/core/EmojiData_generated.cpp
    src/core/MentionCandidate.h
    src/core/BotInfo.h
    src/core/NcFileEntry.h
    src/core/NcUser.h
    src/core/Reminder.h
    src/core/RoomParticipant.h
    src/core/SearchHit.h
    src/ui/ConversationInfoDialog.h
    src/ui/ConversationInfoDialog.cpp
    src/ui/NewChatDialog.h
    src/ui/NewChatDialog.cpp
    src/ui/NextcloudFilePickerDialog.h
    src/ui/NextcloudFilePickerDialog.cpp
    src/ui/TopicTabBar.h
    src/ui/TopicTabBar.cpp
    src/ui/ShareOverlay.h
    src/ui/ShareOverlay.cpp
    src/ui/UpcomingRemindersDialog.h
    src/ui/UpcomingRemindersDialog.cpp
    src/core/BackgroundEngine.cpp
    src/core/BackgroundEngine.h
    src/core/BackgroundWorker.cpp
    src/core/BackgroundWorker.h
    src/core/BackgroundCompositor.cpp
    src/core/BackgroundCompositor.h
    src/core/TfliteSegmenter.cpp
    src/core/TfliteSegmenter.h
    resources/resources.qrc
    resources/twemoji.qrc
    resources/changelog.qrc
    resources/sounds.qrc
    resources/backgrounds.qrc
    resources/talq.rc
)

# Build timestamp
string(TIMESTAMP TALQ_BUILD_TS "%m%d-%H%M")
# Version / codename / build-timestamp defines are scoped to ONLY the source
# files that read them — deliberately NOT target-wide. As PRIVATE target defs
# they were injected into EVERY translation unit, so the per-build TALQ_BUILD_TS
# (and every version bump) changed every compile command and busted the ENTIRE
# ccache: each clean release cut recompiled all ~78 objects (~6 min) instead of
# hitting the cache. Scoped here, a cut recompiles only these few files; the
# rest are ccache hits. Only these 6 files reference the macros (grep-verified).
#
# Codename RULE: per MINOR LINE, shared across ALL patch releases of that line —
# do NOT vary it per patch. 150th-anniv April-Uprising thread: 0.39.x "Aprilsko
# Vastanie" → 0.40.x "Panagyurishte" → 0.41.x "Koprivshtitsa". 0.42.x pivoted to
# Hitchhiker's-Guide "Deep Thought" (computed 42). 0.43.x BETA = "Magrathea",
# the Guide's legendary planet-building world. Locked 2026-05-29.
set_source_files_properties(
    src/main.cpp
    src/core/CallManager.cpp
    src/core/SignalingClient.cpp
    src/core/UpdateChecker.cpp
    src/ui/SettingsDialog.cpp
    src/ui/MainWindow.cpp
    PROPERTIES COMPILE_DEFINITIONS
      "TALQ_VERSION=\"${PROJECT_VERSION}\";TALQ_VERSION_NAME=\"Bafana Bafana\";TALQ_BUILD_TS=\"${TALQ_BUILD_TS}\""
)

# Pre-release flag: when build-release.sh is invoked with --beta it
# passes -DTALQ_PRERELEASE=ON; that bakes the compile define into the
# binary so the UI can show a "PRE-RELEASE" badge regardless of which
# channel ultimately served the user. Stable builds are unaffected.
option(TALQ_PRERELEASE "Mark this binary as a pre-release/beta build" OFF)
if(TALQ_PRERELEASE)
    target_compile_definitions(talq PRIVATE TALQ_PRERELEASE)
    message(STATUS "TalQ: PRE-RELEASE build (UI will show a beta badge)")
endif()

# #20 Video background blur + replace. Build-time gate so the feature can be
# stripped out of slim ports until the TFLite + GL composite stack is fully
# wired. Default ON; runtime behaviour is ALSO gated by the
# Talk/Backgrounds/virtualBackgroundEnabled QSetting (default false), so
# turning this on at build time alone doesn't change any user-visible
# state — the engine is a pass-through until the user opts in.
option(TALQ_BG "Build the video-background blur + replace stack" ON)
if(TALQ_BG)
    target_compile_definitions(talq PRIVATE TALQ_BG)
    message(STATUS "TalQ: video-background stack ENABLED (#20)")
endif()

# #20 Phase 2e — ONNX Runtime for real selfie segmentation. Vendored
# under third_party/onnxruntime/ (Windows x64 prebuilt from
# microsoft/onnxruntime v1.20.1, MIT-licensed redistribution). The
# import library is mingw-generated via gendef + dlltool from the .dll.
# Default ON when TALQ_BG is on; the option exists so a slim port
# (e.g. a CI build that doesn't need the engine) can drop it.
option(TALQ_BG_ORT "Link ONNX Runtime for real selfie segmentation" ON)
if(TALQ_BG AND TALQ_BG_ORT)
    set(_ort_dir "${CMAKE_SOURCE_DIR}/third_party/onnxruntime")
    if(NOT EXISTS "${_ort_dir}/lib/libonnxruntime.dll.a")
        message(FATAL_ERROR
            "TALQ_BG_ORT=ON but ${_ort_dir}/lib/libonnxruntime.dll.a is "
            "missing. Re-run gendef/dlltool, or disable with "
            "-DTALQ_BG_ORT=OFF for a stub-only build.")
    endif()
    target_include_directories(talq PRIVATE "${_ort_dir}/include")
    target_link_directories(talq PRIVATE   "${_ort_dir}/lib")
    target_link_libraries(talq PRIVATE     onnxruntime)
    target_compile_definitions(talq PRIVATE TALQ_BG_ORT)
    message(STATUS "TalQ: ONNX Runtime ENABLED for selfie segmentation (#20 Phase 2e)")
endif()

# Branding. 123NET assets live outside the tree in /private (never
# published); the branded build pulls them in here. The default/public
# build is unbranded and references none of this.
if(TALQ_BRAND STREQUAL "123NET")
    # Brand identity/endpoints are NOT defined here (would leak into public
    # source). They come from the private include store at compile time.
    target_compile_definitions(talq PRIVATE TALQ_BRAND_123NET)
    set(_brand_qrc "${CMAKE_SOURCE_DIR}/private/branding/123net/brand.qrc")
    if(NOT EXISTS "${_brand_qrc}")
        message(FATAL_ERROR
            "TALQ_BRAND=123NET but ${_brand_qrc} is missing. "
            "The 123NET branding store is private and not part of the "
            "public source. Build without -DTALQ_BRAND for the generic app.")
    endif()
    target_sources(talq PRIVATE "${_brand_qrc}")
    # Private update-channel config (brand_updates.inc) is included by
    # AppSettings.h under #ifdef TALQ_BRAND_123NET; put it on the path.
    target_include_directories(talq PRIVATE
        "${CMAKE_SOURCE_DIR}/private/branding/123net")
    message(STATUS "Building 123NET branded version")
endif()

target_include_directories(talq PRIVATE src ${GST_INCLUDE_DIRS})
target_link_directories(talq PRIVATE ${GST_LIBRARY_DIRS})

target_link_libraries(talq PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::OpenGL
    Qt6::Sql
    Qt6::WebSockets
    Qt6::Widgets
    Qt6::Svg
    Qt6::Concurrent
    Qt6::Multimedia
    ${GST_LIBRARIES}
)

# Windows-specific settings
if(WIN32)
    # Debug builds: show console for logging. Release: GUI only (no console).
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        set_target_properties(talq PROPERTIES WIN32_EXECUTABLE TRUE)
    endif()
    target_link_libraries(talq PRIVATE dwmapi winmm ole32 uuid dbghelp)
endif()

install(TARGETS talq RUNTIME DESTINATION bin)

# ==== sync-public truncates here (the generic app target is complete above; the private test harnesses below reference tests/ files that are not in the public mirror) ====

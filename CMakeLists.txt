cmake_minimum_required(VERSION 3.22)
project(talq VERSION 0.39.0 LANGUAGES CXX)

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
    src/core/VideoFrameProvider.h
    src/core/VideoFrameProvider.cpp
    src/core/MediaDeviceManager.cpp
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
    src/ui/UpcomingRemindersDialog.h
    src/ui/UpcomingRemindersDialog.cpp
    src/core/BackgroundEngine.cpp
    src/core/BackgroundEngine.h
    src/core/BackgroundCompositor.cpp
    src/core/BackgroundCompositor.h
    resources/resources.qrc
    resources/twemoji.qrc
    resources/changelog.qrc
    resources/sounds.qrc
    resources/backgrounds.qrc
    resources/talq.rc
)

# Build timestamp
string(TIMESTAMP TALQ_BUILD_TS "%m%d-%H%M")
target_compile_definitions(talq PRIVATE
    TALQ_VERSION="${PROJECT_VERSION}"
    TALQ_BUILD_TS="${TALQ_BUILD_TS}"
    # Per-release codename (empty string = no codename pill). Set alongside
    # the version bump; shown on the Mission Control command bar.
    # "Aprilsko Vastanie": the Bulgarian April Uprising of 1876
    # (Българско Априлско Въстание) — the 150th anniversary falls in 2026.
    # Panagyurishte was one of the four revolutionary districts and the
    # town where the uprising was declared; Raina Knyaginya sewed its
    # tricolour flag at age 20. The codename was locked in 2026-05-24
    # while the project lead was physically in Panagyurishte for the
    # 24 May long weekend (Day of Bulgarian Enlightenment).
    TALQ_VERSION_NAME="Aprilsko Vastanie"
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

# ---- Call test harness (no GUI) ----
qt_add_executable(talq-call-test
    tests/call_test.cpp
    src/core/ApiClient.cpp
    src/core/SignalingClient.cpp
    src/core/PeerPipeline.cpp
    src/core/PublishPipeline.cpp
    src/core/ScreenSharePipeline.cpp
    src/core/SubscribePipeline.cpp
    src/core/SubscribeWebrtcSrc.cpp
    src/core/VideoFrameProvider.cpp
    src/core/CallSignaling.cpp
)
target_include_directories(talq-call-test PRIVATE src ${GST_INCLUDE_DIRS})
# SignalingClient.cpp references TALQ_VERSION (talq target defines it; the
# test target must too or it fails to compile).
target_compile_definitions(talq-call-test PRIVATE TALQ_VERSION="${PROJECT_VERSION}")
# Mirror TALQ_PRERELEASE so the harness exercises the SAME publish topology
# as the build under test: ON → 3-layer simulcast (lets the substream-
# selection scenarios run), OFF → single 720p. Without this the harness
# always built single-stream and couldn't test simulcast at all.
if(TALQ_PRERELEASE)
    target_compile_definitions(talq-call-test PRIVATE TALQ_PRERELEASE)
endif()
target_link_directories(talq-call-test PRIVATE ${GST_LIBRARY_DIRS})
target_link_libraries(talq-call-test PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::WebSockets
    Qt6::Multimedia
    ${GST_LIBRARIES}
)
if(WIN32)
    target_link_libraries(talq-call-test PRIVATE credui advapi32)
    # Console subsystem: it's a QCoreApplication test harness, so its
    # qDebug/stderr must reach the terminal (Qt defaults exes to the
    # WIN32 GUI subsystem, which detaches stdio).
    set_target_properties(talq-call-test PROPERTIES WIN32_EXECUTABLE FALSE)
endif()

# ---- Pure unit tests (no Qt runtime, no GStreamer) ----
# #18 substream policy — header-only pure function, no Qt deps. Builds in
# under a second; ctest can run it inline so a CallStage refactor breaking
# the tile-size policy fails the suite immediately.
add_executable(substream-policy-test tests/substream_policy_test.cpp)
target_include_directories(substream-policy-test PRIVATE src)
if(WIN32)
    set_target_properties(substream-policy-test PROPERTIES WIN32_EXECUTABLE FALSE)
endif()

# #19 version-compare — UpdateChecker's "is the remote build newer than
# what we're running" answer is what gates auto-update; a regression here
# either strands users on broken builds or downgrades them. Header-only,
# no Qt.
add_executable(version-compare-test tests/version_compare_test.cpp)
target_include_directories(version-compare-test PRIVATE src)
if(WIN32)
    set_target_properties(version-compare-test PROPERTIES WIN32_EXECUTABLE FALSE)
endif()

# #20 Phase 2a — BackgroundEngine lifecycle test. Pulls in Qt6::Core +
# Qt6::Gui (QImage) but no GStreamer; runs in a few hundred ms. Future
# phases expand this with composite-output assertions.
qt_add_executable(background-engine-test
    tests/background_engine_test.cpp
    src/core/BackgroundEngine.cpp
    src/core/BackgroundCompositor.cpp
    resources/backgrounds.qrc
)
target_include_directories(background-engine-test PRIVATE src)
target_link_libraries(background-engine-test PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::OpenGL
)
if(WIN32)
    set_target_properties(background-engine-test PROPERTIES WIN32_EXECUTABLE FALSE)
endif()

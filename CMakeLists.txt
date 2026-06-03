cmake_minimum_required(VERSION 3.22)
project(talq VERSION 0.48.8 LANGUAGES CXX)

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
      "TALQ_VERSION=\"${PROJECT_VERSION}\";TALQ_VERSION_NAME=\"Botev\";TALQ_BUILD_TS=\"${TALQ_BUILD_TS}\""
)


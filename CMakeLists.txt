cmake_minimum_required(VERSION 3.22)
project(talq VERSION 0.17.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS
    Core
    Gui
    Multimedia
    Network
    Sql
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
set(GST_LIBRARIES gstreamer-1.0 gstsdp-1.0 gstwebrtc-1.0 gstapp-1.0 gobject-2.0 glib-2.0 intl)

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
    src/core/VideoFrameProvider.h
    src/core/VideoFrameProvider.cpp
    src/core/MediaDeviceManager.cpp
    src/core/CallManager.cpp
    src/core/PeerPipeline.cpp
    src/core/ScreenSharePipeline.cpp
    src/core/CallSignaling.cpp
    src/core/DebugMonitor.cpp
    src/core/AppSettings.cpp
    src/painter/PainterTheme.cpp
    src/painter/LayoutEngine.cpp
    src/painter/ChatPainter.cpp
    src/painter/SidebarPainter.cpp
    src/painter/HeaderPainter.cpp
    src/painter/ThreadsPainter.cpp
    src/painter/EmojiTextRenderer.h
    src/painter/EmojiTextRenderer.cpp
    src/ui/MainWindow.cpp
    src/ui/ComposerWidget.cpp
    src/ui/LoginWidget.cpp
    src/ui/SettingsDialog.cpp
    src/ui/CallDialog.cpp
    src/ui/SharePickerDialog.cpp
    src/ui/SelectionBarWidget.h
    src/ui/SelectionBarWidget.cpp
    src/ui/ConversationPickerDialog.h
    src/ui/ConversationPickerDialog.cpp
    src/ui/NotificationPopup.h
    src/ui/NotificationPopup.cpp
    src/ui/EmojiPickerWidget.h
    src/ui/EmojiPickerWidget.cpp
    src/ui/ImageViewerDialog.h
    src/ui/ImageViewerDialog.cpp
    src/core/EmojiData.h
    src/core/EmojiData.cpp
    src/core/EmojiData_generated.cpp
    src/core/MentionCandidate.h
    resources/resources.qrc
    resources/twemoji.qrc
    resources/changelog.qrc
    resources/talq.rc
)

# Build timestamp
string(TIMESTAMP TALQ_BUILD_TS "%m%d-%H%M")
target_compile_definitions(talq PRIVATE
    TALQ_VERSION="${PROJECT_VERSION}"
    TALQ_BUILD_TS="${TALQ_BUILD_TS}"
)

# Branding
if(TALQ_BRAND STREQUAL "123NET")
    target_compile_definitions(talq PRIVATE
        TALQ_BRAND_123NET
        TALQ_BRAND_NAME="123NET Talk"
        TALQ_BRAND_SERVER="https://ncloud.123net.link"
    )
    message(STATUS "Building 123NET branded version")
endif()

target_include_directories(talq PRIVATE src ${GST_INCLUDE_DIRS})
target_link_directories(talq PRIVATE ${GST_LIBRARY_DIRS})

target_link_libraries(talq PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::Sql
    Qt6::WebSockets
    Qt6::Widgets
    Qt6::Multimedia
    ${GST_LIBRARIES}
)

# Windows-specific settings
if(WIN32)
    # Debug builds: show console for logging. Release: GUI only (no console).
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        set_target_properties(talq PROPERTIES WIN32_EXECUTABLE TRUE)
    endif()
    target_link_libraries(talq PRIVATE dwmapi winmm)
endif()

install(TARGETS talq RUNTIME DESTINATION bin)

# ---- Call test harness (no GUI) ----
qt_add_executable(talq-call-test
    tests/call_test.cpp
    src/core/ApiClient.cpp
    src/core/SignalingClient.cpp
    src/core/PeerPipeline.cpp
    src/core/PublishPipeline.cpp
    src/core/SubscribePipeline.cpp
    src/core/VideoFrameProvider.cpp
    src/core/CallSignaling.cpp
)
target_include_directories(talq-call-test PRIVATE src ${GST_INCLUDE_DIRS})
target_link_directories(talq-call-test PRIVATE ${GST_LIBRARY_DIRS})
target_link_libraries(talq-call-test PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::WebSockets
    Qt6::Multimedia
    ${GST_LIBRARIES}
)
if(WIN32)
    target_link_libraries(talq-call-test PRIVATE credui advapi32)
endif()

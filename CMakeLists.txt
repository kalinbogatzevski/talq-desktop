cmake_minimum_required(VERSION 3.22)
project(talq VERSION 0.6.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS
    Core
    Gui
    Network
    Qml
    Quick
    QuickControls2
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
set(GST_LIBRARIES gstreamer-1.0 gstsdp-1.0 gstwebrtc-1.0 gobject-2.0 glib-2.0 intl)

qt_add_executable(talq
    src/main.cpp
    src/core/ApiClient.cpp
    src/core/AuthManager.cpp
    src/core/MessageCache.cpp
    src/core/AvatarProvider.cpp
    src/core/MessagePoller.cpp
    src/core/NotificationManager.cpp
    src/core/PushClient.cpp
    src/core/SignalingClient.cpp
    src/core/FilePreviewProvider.cpp
    src/models/Conversation.cpp
    src/models/Message.cpp
    src/models/ConversationListModel.cpp
    src/models/MessageListModel.cpp
    src/models/ThreadListModel.cpp
    src/core/CallPipeline.cpp
    src/core/MediaDeviceManager.cpp
    src/core/CallManager.cpp
    resources/talq.rc
)

qt_policy(SET QTP0001 NEW)
qt_policy(SET QTP0004 OLD)

set_source_files_properties(src/qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(talq
    URI TalkQt
    VERSION 1.0
    QML_FILES
        src/qml/Main.qml
        src/qml/LoginView.qml
        src/qml/ConversationList.qml
        src/qml/ChatView.qml
        src/qml/Theme.qml
        src/qml/MessageBubble.qml
        src/qml/ConversationItem.qml
        src/qml/MessageComposer.qml
        src/qml/ThreadItem.qml
        src/qml/ThreadListView.qml
        src/qml/CallWindow.qml
        src/qml/IncomingCallPopup.qml
    RESOURCES
        resources/resources.qrc
)

# Build timestamp — changes on every reconfigure for tracking
string(TIMESTAMP TALQ_BUILD_TS "%m%d-%H%M")
target_compile_definitions(talq PRIVATE TALQ_BUILD_TS="${TALQ_BUILD_TS}")

# Branding: cmake -DTALQ_BRAND=123NET for dedicated 123NET build
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
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Sql
    Qt6::WebSockets
    Qt6::Widgets
    ${GST_LIBRARIES}
)

# Windows-specific settings
if(WIN32)
    set_target_properties(talq PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_libraries(talq PRIVATE dwmapi winmm)
endif()

install(TARGETS talq RUNTIME DESTINATION bin)

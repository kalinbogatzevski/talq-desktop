cmake_minimum_required(VERSION 3.22)
project(talk-qt VERSION 0.1.0 LANGUAGES CXX)

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
    WebSockets
)

qt_add_executable(talk-qt
    src/main.cpp
    src/core/ApiClient.cpp
    src/core/AuthManager.cpp
    src/core/MessagePoller.cpp
    src/models/Conversation.cpp
    src/models/Message.cpp
    src/models/ConversationListModel.cpp
    src/models/MessageListModel.cpp
    resources/talq.rc
)

qt_policy(SET QTP0001 NEW)
qt_policy(SET QTP0004 OLD)

set_source_files_properties(src/qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(talk-qt
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
    RESOURCES
        resources/resources.qrc
)

target_include_directories(talk-qt PRIVATE src)

target_link_libraries(talk-qt PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::WebSockets
)

# Windows-specific settings
if(WIN32)
    set_target_properties(talk-qt PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

install(TARGETS talk-qt RUNTIME DESTINATION bin)

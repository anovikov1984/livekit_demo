cmake_minimum_required(VERSION 3.21)

project(livekit_qt_player LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(LiveKit CONFIG REQUIRED)

add_executable(livekit_qt_player
    src/main.cpp
    src/mainwindow.cpp
    src/mainwindow.h
    src/livekitplayer.cpp
    src/livekitplayer.h
)

target_link_libraries(livekit_qt_player
    PRIVATE
        Qt6::Widgets
        LiveKit::livekit
)

set_target_properties(livekit_qt_player PROPERTIES
    BUILD_RPATH "${CMAKE_SOURCE_DIR}/third_party/livekit-sdk/lib"
)

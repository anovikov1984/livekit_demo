cmake_minimum_required(VERSION 3.21)

project(videowall LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGL OpenGLWidgets)
set(LiveKit_DIR "${CMAKE_SOURCE_DIR}/../third_party/livekit-sdk/lib/cmake/LiveKit")
find_package(LiveKit CONFIG REQUIRED)

add_executable(videowall
    src/main.cpp
    src/mainwindow.cpp
    src/mainwindow.h
    src/livekitplayer.cpp
    src/livekitplayer.h
    src/videocell.cpp
    src/videocell.h
)

target_link_libraries(videowall
    PRIVATE
        Qt6::Widgets
        Qt6::OpenGL
        Qt6::OpenGLWidgets
        LiveKit::livekit
)

set_target_properties(videowall PROPERTIES
    BUILD_RPATH "${CMAKE_SOURCE_DIR}/../third_party/livekit-sdk/lib"
)

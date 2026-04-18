# LiveKit Native Qt Player

Native Qt/C++ desktop player for LiveKit WebRTC video.

This app provides:
- a JSON input area where you paste:
  - `token`
  - `apiUrl`
- extracted `token`/`apiUrl` fields
- a `Play` button that connects with the LiveKit C++ SDK and renders remote video
- a `Stop` button to disconnect

No embedded browser/web snippets are used.

## Prerequisites

- Qt 6 (Widgets)
- CMake >= 3.21
- LiveKit C++ SDK installed on your machine

### Build and install LiveKit C++ SDK

Follow the official SDK build docs:

- [LiveKit C++ SDK repository](https://github.com/livekit/client-sdk-cpp)

After installing, make sure CMake can find `LiveKitConfig.cmake`.
If needed, pass `CMAKE_PREFIX_PATH` to your LiveKit install prefix.

## Build this app

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/Qt;/path/to/livekit/install"
cmake --build build
```

## Run

```bash
./build/livekit_qt_player
```

On macOS/Linux, ensure runtime linker paths include both `liblivekit` and
`liblivekit_ffi` from your LiveKit SDK install if they are not in a default
system location.

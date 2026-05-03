# LiveKit Native Qt Player

Native Qt/C++ desktop player for LiveKit WebRTC video.

This repo contains two apps:

- **`livekit_qt_player`** (repo root) — single-stream player, renders via `QLabel`
- **`videowall/`** — multi-stream grid renderer using OpenGL, described below

## videowall

Subscribes to N LiveKit video tracks in parallel and renders each one into a
cell of a square grid using OpenGL textured quads.

The UI has:
- a JSON input area where you paste an array of `{ "token": "...", "apiUrl": "wss://..." }` objects
- **Play All** / **Stop All** buttons

## Prerequisites

- macOS (arm64 or x86_64)
- Xcode Command Line Tools: `xcode-select --install`
- Qt 6: `brew install qt`
- CMake >= 3.21: `brew install cmake`
- Rust toolchain: `curl https://sh.rustup.rs -sSf | sh`
- protobuf: `brew install protobuf`

## Quick start — one-shot build (SDK + app)

`script/run.sh` clones the LiveKit C++ SDK fork, builds it, vendors the
artifacts into `third_party/livekit-sdk/`, then builds `videowall`.

```bash
cd livekit_demo
bash script/run.sh
```

Run the built binary:

```bash
./videowall/build/videowall
```

### First-time setup (auto-install Rust + protobuf)

```bash
INSTALL_DEPS=1 bash script/run.sh
```

### Environment variable reference

| Variable         | Default                                              | Purpose                                  |
|------------------|------------------------------------------------------|------------------------------------------|
| `INSTALL_DEPS`   | `0`                                                  | If `1`, auto-install `cargo` + `protoc` via brew |
| `LIVEKIT_REPO`   | `https://github.com/Shushpancheak/client-sdk-cpp.git` | SDK fork to clone                        |
| `LIVEKIT_COMMIT` | `add-remote-video-quality-controls`                  | Branch/tag/SHA to check out              |
| `BUILD_TYPE`     | `RelWithDebInfo`                                     | CMake build type for SDK and app         |
| `ARCH`           | `$(uname -m)`                                        | `CMAKE_OSX_ARCHITECTURES`                |
| `JOBS`           | `$(sysctl -n hw.logicalcpu)`                         | Parallel build jobs                      |
| `SDK_SRC_DIR`    | `.deps/client-sdk-cpp`                               | Where the SDK source is cloned           |
| `QT_PREFIX`      | *(unset)*                                            | Path to Qt install (e.g. `$(brew --prefix qt)`) |
| `CLEAN`          | `0`                                                  | If `1`, wipe SDK build/install dirs first |

### Common invocations

```bash
# Release build
BUILD_TYPE=Release bash script/run.sh

# Explicit Qt path (if cmake can't find Qt automatically)
QT_PREFIX=$(brew --prefix qt) bash script/run.sh

# Clean rebuild of SDK from scratch
CLEAN=1 bash script/run.sh

# Use a specific SDK commit
LIVEKIT_COMMIT=977ae2a bash script/run.sh
```

## App-only rebuild (SDK already vendored)

If `third_party/livekit-sdk/` already has up-to-date libs and headers:

```bash
cd videowall
cmake -S . -B build -DCMAKE_PREFIX_PATH="$(brew --prefix qt)"
cmake --build build -j
./build/videowall
```

## Runtime requirements

`videowall` is a subscriber only. You need:

- A LiveKit server reachable at a `wss://` URL
- A JWT viewer token (`canSubscribe: true`) for a room with at least one video publisher

The companion stack in `go5-livekit-test/` provides both. See
`go5-livekit-test/README.md` for setup.

---

## livekit_qt_player (legacy single-stream)

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/Qt;/path/to/livekit/install"
cmake --build build
./build/livekit_qt_player
```

On macOS/Linux, ensure runtime linker paths include both `liblivekit` and
`liblivekit_ffi` from your LiveKit SDK install if they are not in a default
system location.
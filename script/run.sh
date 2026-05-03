#!/usr/bin/env bash
set -euo pipefail

# Rebuild LiveKit C++ SDK and videowall app for profiling.
#
# Expected project layout:
#
#   livekit_demo/
#     videowall/
#     third_party/livekit-sdk/
#     scripts/run.sh
#
# Usage:
#
#   chmod +x scripts/run.sh
#   ./scripts/run.sh
#
# Optional env vars:
#
#   LIVEKIT_COMMIT=977ae2a
#   BUILD_TYPE=RelWithDebInfo
#   SDK_SRC_DIR=/path/to/client-sdk-cpp
#   QT_PREFIX=/path/to/Qt
#   CLEAN=1
#   INSTALL_DEPS=1

LIVEKIT_COMMIT=add-remote-video-quality-controls
LIVEKIT_REPO="${LIVEKIT_REPO:-git@github.com:Shushpancheak/client-sdk-cpp.git}"
BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"
ARCH="${ARCH:-$(uname -m)}"
JOBS="${JOBS:-$(sysctl -n hw.logicalcpu)}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

SDK_SRC_DIR="${SDK_SRC_DIR:-$PROJECT_ROOT/.deps/client-sdk-cpp}"
SDK_BUILD_DIR="$SDK_SRC_DIR/build"
SDK_INSTALL_DIR="$SDK_SRC_DIR/install"

VENDORED_SDK_DIR="$PROJECT_ROOT/third_party/livekit-sdk"
APP_DIR="$PROJECT_ROOT/videowall"
APP_BUILD_DIR="$APP_DIR/build"

COMMON_RELWITHDEBINFO_FLAGS="-O2 -g -DNDEBUG -fno-omit-frame-pointer"

echo "Project root:        $PROJECT_ROOT"
echo "LiveKit SDK source:  $SDK_SRC_DIR"
echo "LiveKit commit:      $LIVEKIT_COMMIT"
echo "Build type:          $BUILD_TYPE"
echo "Architecture:        $ARCH"
echo "Parallel jobs:       $JOBS"
echo

if [[ "${INSTALL_DEPS:-0}" == "1" ]]; then
  if ! command -v cargo >/dev/null 2>&1; then
    echo "Installing Rust toolchain..."
    curl https://sh.rustup.rs -sSf | sh -s -- -y
    # shellcheck disable=SC1090
    source "$HOME/.cargo/env"
  else
    echo "Updating Rust toolchain..."
    rustup update stable
  fi

  if ! command -v protoc >/dev/null 2>&1; then
    echo "Installing protobuf..."
    brew install protobuf
  fi
else
  if ! command -v cargo >/dev/null 2>&1; then
    echo "ERROR: cargo not found."
    echo "Install Rust first, or rerun with INSTALL_DEPS=1."
    echo "Manual install:"
    echo "  curl https://sh.rustup.rs | sh"
    echo "  source ~/.cargo/env"
    exit 1
  fi

  if ! command -v protoc >/dev/null 2>&1; then
    echo "ERROR: protoc not found."
    echo "Install protobuf first, or rerun with INSTALL_DEPS=1."
    echo "Manual install:"
    echo "  brew install protobuf"
    exit 1
  fi
fi

mkdir -p "$(dirname "$SDK_SRC_DIR")"
mkdir -p "$VENDORED_SDK_DIR/lib"
mkdir -p "$VENDORED_SDK_DIR/include"

if [[ ! -d "$SDK_SRC_DIR/.git" ]]; then
  echo "Cloning LiveKit C++ SDK..."
  git clone --recurse-submodules "$LIVEKIT_REPO" "$SDK_SRC_DIR"
fi

cd "$SDK_SRC_DIR"

echo "Checking out LiveKit commit $LIVEKIT_COMMIT..."
git fetch --all --tags
if ! git checkout "$LIVEKIT_COMMIT" 2>/dev/null; then
  echo "ERROR: branch/tag/commit '$LIVEKIT_COMMIT' not found in $LIVEKIT_REPO"
  echo "Set LIVEKIT_COMMIT to a valid branch, tag, or SHA and retry."
  exit 1
fi
git submodule update --init --recursive

if [[ "${CLEAN:-0}" == "1" ]]; then
  echo "Cleaning SDK build/install directories..."
  rm -rf "$SDK_BUILD_DIR" "$SDK_INSTALL_DIR"
fi

echo "Configuring LiveKit SDK..."
cmake -S "$SDK_SRC_DIR" -B "$SDK_BUILD_DIR" \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DCMAKE_INSTALL_PREFIX="$SDK_INSTALL_DIR" \
  -DCMAKE_OSX_ARCHITECTURES="$ARCH" \
  -DBUILD_SHARED_LIBS=ON \
  -DCMAKE_C_FLAGS_RELWITHDEBINFO="$COMMON_RELWITHDEBINFO_FLAGS" \
  -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="$COMMON_RELWITHDEBINFO_FLAGS"

echo "Building LiveKit SDK..."
cmake --build "$SDK_BUILD_DIR" --parallel "$JOBS"

echo "Installing LiveKit SDK..."
cmake --install "$SDK_BUILD_DIR"

echo "Copying LiveKit SDK into project third_party..."
mkdir -p "$VENDORED_SDK_DIR/lib"
mkdir -p "$VENDORED_SDK_DIR/include"

cp "$SDK_INSTALL_DIR"/lib/liblivekit*.dylib "$VENDORED_SDK_DIR/lib/"

rm -rf "$VENDORED_SDK_DIR/lib/cmake"
cp -R "$SDK_INSTALL_DIR/lib/cmake" "$VENDORED_SDK_DIR/lib/"

rm -rf "$VENDORED_SDK_DIR/include/livekit"
cp -R "$SDK_INSTALL_DIR/include/livekit" "$VENDORED_SDK_DIR/include/"

echo "Configuring videowall app..."

CMAKE_PREFIX_PATH_VALUE="$VENDORED_SDK_DIR"

if [[ -n "${QT_PREFIX:-}" ]]; then
  CMAKE_PREFIX_PATH_VALUE="$QT_PREFIX;$CMAKE_PREFIX_PATH_VALUE"
fi

cmake -S "$APP_DIR" -B "$APP_BUILD_DIR" \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DCMAKE_PREFIX_PATH="$CMAKE_PREFIX_PATH_VALUE" \
  -DCMAKE_C_FLAGS_RELWITHDEBINFO="$COMMON_RELWITHDEBINFO_FLAGS" \
  -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="$COMMON_RELWITHDEBINFO_FLAGS"

echo "Building videowall app..."
cmake --build "$APP_BUILD_DIR" --parallel "$JOBS"

echo
echo "Done."
echo "Run:"
echo "  $APP_BUILD_DIR/videowall"
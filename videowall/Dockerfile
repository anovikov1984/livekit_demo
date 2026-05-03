# Build videowall app on top of the pre-built SDK base image.
#
# First time setup (run once):
#   docker build -t livekit-sdk-base -f Dockerfile.sdk .
#
# Every subsequent app build:
#   docker build -t videowall .
#
# Run with X11 forwarding (macOS: install XQuartz first):
#   xhost +localhost
#   docker run --rm -e DISPLAY=host.docker.internal:0 videowall

ARG SDK_BASE=livekit-sdk-base

# ---------------------------------------------------------------------------
# Stage 1: build videowall on top of SDK base
# ---------------------------------------------------------------------------
FROM ${SDK_BASE} AS builder

ARG BUILD_TYPE=RelWithDebInfo

COPY . /workspace/videowall/

RUN COMMON="-O2 -g -DNDEBUG -fno-omit-frame-pointer" \
 && cmake -S videowall -B videowall/build \
      -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
      "-DCMAKE_C_FLAGS_RELWITHDEBINFO=$COMMON" \
      "-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=$COMMON" \
 && cmake --build videowall/build --parallel "$(nproc)"

# ---------------------------------------------------------------------------
# Stage 2: minimal runtime image
# ---------------------------------------------------------------------------
FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    libqt6core6t64 \
    libqt6gui6t64 \
    libqt6widgets6t64 \
    libqt6opengl6t64 \
    libqt6openglwidgets6t64 \
    libgl1 \
    libglib2.0-0t64 \
    libdbus-1-3 \
    libxcb-cursor0 \
    libxcb1 \
 && rm -rf /var/lib/apt/lists/*

COPY --from=builder /workspace/videowall/build/videowall /usr/local/bin/videowall
COPY --from=builder /workspace/third_party/livekit-sdk/lib/liblivekit*.so* /usr/local/lib/
RUN ldconfig

CMD ["videowall"]

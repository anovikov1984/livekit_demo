#pragma once

#include "livekit/video_frame.h"

#include <QMetaType>

#include <memory>

// Carries a LiveKit SDK video frame from the reader thread to the GL thread
// without copying pixel data. The shared_ptr keeps the underlying plane
// memory alive (FFI-owned in view mode, or the frame's std::vector<uint8_t>
// in owned mode) until the GL thread finishes uploading it; Qt's queued
// connection only bumps the refcount.
struct YuvFrame {
  std::shared_ptr<const livekit::VideoFrame> frame;
  int width{0};
  int height{0};

  explicit operator bool() const noexcept { return static_cast<bool>(frame); }
};

Q_DECLARE_METATYPE(YuvFrame)

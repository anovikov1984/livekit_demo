#pragma once

#include <QMetaType>

#include <cstdint>
#include <memory>
#include <vector>

// Planar I420 video frame carried across the SDK→GUI thread boundary.
// Each plane is a tightly-packed (stride == width) byte buffer owned via
// shared_ptr so Qt's queued-connection copy is a refcount bump rather than a
// deep memcpy of pixel data.
struct YuvFrame {
  using PlaneBuffer = std::shared_ptr<const std::vector<std::uint8_t>>;
  PlaneBuffer y;   // width * height bytes
  PlaneBuffer u;   // (width/2) * (height/2) bytes
  PlaneBuffer v;   // (width/2) * (height/2) bytes
  int width{0};    // luma plane width in pixels
  int height{0};   // luma plane height in pixels
};

Q_DECLARE_METATYPE(YuvFrame)

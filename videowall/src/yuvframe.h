#pragma once

#include <QImage>
#include <QMetaType>

// Planar I420 video frame carried across the SDK→GUI thread boundary.
// Each plane is a Format_Grayscale8 QImage so it benefits from QImage's
// implicit sharing (the SDK side triple-buffers these to avoid COW detach).
struct YuvFrame {
  QImage y;
  QImage u;
  QImage v;
};

Q_DECLARE_METATYPE(YuvFrame)

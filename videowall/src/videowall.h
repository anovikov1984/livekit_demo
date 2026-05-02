#pragma once

#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>
#include <QRect>

#include <vector>

#include "yuvframe.h"

class QOpenGLShaderProgram;

// Renders N I420 video streams as quads inside a single QOpenGLWidget.
// YUV→RGB conversion happens in the fragment shader (BT.601 limited).
class VideoWall : public QOpenGLWidget, protected QOpenGLFunctions {
  Q_OBJECT

public:
  explicit VideoWall(QWidget *parent = nullptr);
  ~VideoWall() override;

  void setStreamCount(int n);
  void uploadFrame(int idx, const YuvFrame &frame);
  void clearFrame(int idx);
  void clearAll();

protected:
  void initializeGL() override;
  void paintGL() override;
  void resizeGL(int w, int h) override;

private:
  struct Slot {
    GLuint texY{0};
    GLuint texU{0};
    GLuint texV{0};
    int yW{0};
    int yH{0};
    int uW{0};
    int uH{0};
    int vW{0};
    int vH{0};
    bool hasFrame{false};
    YuvFrame pending;
  };

  void reconcileSlots();
  void uploadPlane(GLuint tex, const QImage &plane, int &cachedW,
                   int &cachedH);
  QRect cellRect(int idx) const;

  QOpenGLShaderProgram *program_{nullptr};
  QOpenGLBuffer vbo_;
  QOpenGLVertexArrayObject vao_;
  std::vector<Slot> slots_;
  int desiredCount_{0};
  int fbW_{0};
  int fbH_{0};
  int locTexY_{-1};
  int locTexU_{-1};
  int locTexV_{-1};
};

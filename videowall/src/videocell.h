#pragma once

#include <QImage>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>

class QOpenGLShaderProgram;

class VideoCell : public QOpenGLWidget, protected QOpenGLFunctions {
  Q_OBJECT

public:
  explicit VideoCell(QWidget *parent = nullptr);
  ~VideoCell() override;

  void uploadFrame(const QImage &frame);
  void clearFrame();

protected:
  void initializeGL() override;
  void paintGL() override;
  void resizeGL(int w, int h) override;

private:
  QOpenGLShaderProgram *program_{nullptr};
  QOpenGLBuffer vbo_;
  QOpenGLVertexArrayObject vao_;
  GLuint textureId_{0};
  int texWidth_{0};
  int texHeight_{0};
  bool hasFrame_{false};
  QImage pendingFrame_;
};

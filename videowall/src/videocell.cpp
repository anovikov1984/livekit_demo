#include "videocell.h"

#include <QOpenGLShaderProgram>
#include <QSizePolicy>

static const char *kVertSrc = R"(
#version 150 core
in vec2 aPos;
in vec2 aUV;
out vec2 vUV;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vUV = aUV;
}
)";

static const char *kFragSrc = R"(
#version 150 core
in vec2 vUV;
out vec4 color;
uniform sampler2D uTex;
void main() {
    color = texture(uTex, vUV);
}
)";

// Triangle strip: BL, BR, TL, TR
// UV V flipped: T=0 → QImage row 0 (visual top of image)
static const GLfloat kVerts[] = {
//   x      y      u     v
    -1.0f, -1.0f,  0.0f, 1.0f,
     1.0f, -1.0f,  1.0f, 1.0f,
    -1.0f,  1.0f,  0.0f, 0.0f,
     1.0f,  1.0f,  1.0f, 0.0f,
};

VideoCell::VideoCell(QWidget *parent) : QOpenGLWidget(parent) {
  setMinimumSize(320, 180);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

VideoCell::~VideoCell() {
  makeCurrent();
  if (textureId_) {
    glDeleteTextures(1, &textureId_);
    textureId_ = 0;
  }
  vbo_.destroy();
  vao_.destroy();
  doneCurrent();
}

void VideoCell::uploadFrame(const QImage &frame) {
  pendingFrame_ = frame;
  update();
}

void VideoCell::clearFrame() {
  hasFrame_ = false;
  pendingFrame_ = QImage();
  update();
}

void VideoCell::initializeGL() {
  initializeOpenGLFunctions();

  program_ = new QOpenGLShaderProgram(this);
  program_->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertSrc);
  program_->addShaderFromSourceCode(QOpenGLShader::Fragment, kFragSrc);
  program_->link();

  vao_.create();
  vao_.bind();

  vbo_.create();
  vbo_.bind();
  vbo_.allocate(kVerts, sizeof(kVerts));

  program_->bind();
  const int posLoc = program_->attributeLocation("aPos");
  const int uvLoc  = program_->attributeLocation("aUV");
  program_->enableAttributeArray(posLoc);
  program_->enableAttributeArray(uvLoc);
  program_->setAttributeBuffer(posLoc, GL_FLOAT, 0,                    2, 4 * sizeof(GLfloat));
  program_->setAttributeBuffer(uvLoc,  GL_FLOAT, 2 * sizeof(GLfloat), 2, 4 * sizeof(GLfloat));
  program_->release();

  vao_.release();
  vbo_.release();

  glGenTextures(1, &textureId_);
  glBindTexture(GL_TEXTURE_2D, textureId_);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);

  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
}

void VideoCell::paintGL() {
  glClear(GL_COLOR_BUFFER_BIT);

  if (!pendingFrame_.isNull()) {
    glBindTexture(GL_TEXTURE_2D, textureId_);
    if (pendingFrame_.width() != texWidth_ || pendingFrame_.height() != texHeight_) {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                   pendingFrame_.width(), pendingFrame_.height(),
                   0, GL_RGBA, GL_UNSIGNED_BYTE, pendingFrame_.constBits());
      texWidth_  = pendingFrame_.width();
      texHeight_ = pendingFrame_.height();
    } else {
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                      pendingFrame_.width(), pendingFrame_.height(),
                      GL_RGBA, GL_UNSIGNED_BYTE, pendingFrame_.constBits());
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    hasFrame_ = true;
    pendingFrame_ = QImage();
  }

  if (!hasFrame_) return;

  program_->bind();
  vao_.bind();
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, textureId_);
  program_->setUniformValue("uTex", 0);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glBindTexture(GL_TEXTURE_2D, 0);
  vao_.release();
  program_->release();
}

void VideoCell::resizeGL(int w, int h) {
  glViewport(0, 0, w, h);
}


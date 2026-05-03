#include "videowall.h"

#include <QImage>
#include <QOpenGLShaderProgram>
#include <QSizePolicy>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace {

const char *kVertSrc = R"(
#version 150 core
in vec2 aPos;
in vec2 aUV;
out vec2 vUV;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vUV = aUV;
}
)";

// BT.601 limited-range YCbCr → RGB. Y in [16,235], U/V in [16,240], all
// stored in normalized [0,1] textures (so 16/255 ≈ 0.0627, 128/255 ≈ 0.502).
const char *kFragSrc = R"(
#version 150 core
in vec2 vUV;
out vec4 color;
uniform sampler2D uY;
uniform sampler2D uU;
uniform sampler2D uV;
void main() {
    float y = texture(uY, vUV).r;
    float u = texture(uU, vUV).r;
    float v = texture(uV, vUV).r;
    y = 1.164 * (y - 0.0627451);
    u = u - 0.5019608;
    v = v - 0.5019608;
    float r = y + 1.596  * v;
    float g = y - 0.391  * u - 0.813 * v;
    float b = y + 2.018  * u;
    color = vec4(r, g, b, 1.0);
}
)";

const GLfloat kVerts[] = {
    -1.0f, -1.0f, 0.0f, 1.0f,
     1.0f, -1.0f, 1.0f, 1.0f,
    -1.0f,  1.0f, 0.0f, 0.0f,
     1.0f,  1.0f, 1.0f, 0.0f,
};

QRect letterboxRect(QRect cell, int srcW, int srcH) {
  if (srcW <= 0 || srcH <= 0) return cell;
  const float cellAspect = static_cast<float>(cell.width()) / cell.height();
  const float srcAspect  = static_cast<float>(srcW) / srcH;
  int w, h;
  if (srcAspect > cellAspect) {
    w = cell.width();
    h = static_cast<int>(cell.width() / srcAspect);
  } else {
    h = cell.height();
    w = static_cast<int>(cell.height() * srcAspect);
  }
  return QRect(cell.x() + (cell.width() - w) / 2,
               cell.y() + (cell.height() - h) / 2,
               w, h);
}

} // namespace

VideoWall::VideoWall(QWidget *parent) : QOpenGLWidget(parent) {
  setMinimumSize(320, 180);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

VideoWall::~VideoWall() {
  makeCurrent();
  for (auto &s : slots_) {
    if (s.texY) glDeleteTextures(1, &s.texY);
    if (s.texU) glDeleteTextures(1, &s.texU);
    if (s.texV) glDeleteTextures(1, &s.texV);
    s.texY = s.texU = s.texV = 0;
  }
  slots_.clear();
  vbo_.destroy();
  vao_.destroy();
  doneCurrent();
}

void VideoWall::setStreamCount(int n) {
  if (n < 0) n = 0;
  desiredCount_ = n;
  if (static_cast<int>(slots_.size()) > n) {
    for (int i = n; i < static_cast<int>(slots_.size()); ++i) {
      slots_[i].pending = YuvFrame{};
      slots_[i].hasFrame = false;
    }
  }
  update();
}

void VideoWall::uploadFrame(int idx, const YuvFrame &frame) {
  if (idx < 0 || idx >= desiredCount_) return;
  if (idx >= static_cast<int>(slots_.size())) {
    slots_.resize(desiredCount_);
  }
  slots_[idx].pending = frame;
  update();
}

void VideoWall::clearFrame(int idx) {
  if (idx < 0 || idx >= static_cast<int>(slots_.size())) return;
  slots_[idx].hasFrame = false;
  slots_[idx].pending = YuvFrame{};
  update();
}

void VideoWall::clearAll() {
  for (auto &s : slots_) {
    s.hasFrame = false;
    s.pending = YuvFrame{};
  }
  update();
}

void VideoWall::setSlotMime(int idx, const std::string &mime) {
  if (idx < 0 || idx >= static_cast<int>(slots_.size())) return;
  slots_[idx].mime = mime;
}

void VideoWall::initializeGL() {
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
  const int uvLoc = program_->attributeLocation("aUV");
  program_->enableAttributeArray(posLoc);
  program_->enableAttributeArray(uvLoc);
  program_->setAttributeBuffer(posLoc, GL_FLOAT, 0, 2, 4 * sizeof(GLfloat));
  program_->setAttributeBuffer(uvLoc, GL_FLOAT, 2 * sizeof(GLfloat), 2,
                               4 * sizeof(GLfloat));
  locTexY_ = program_->uniformLocation("uY");
  locTexU_ = program_->uniformLocation("uU");
  locTexV_ = program_->uniformLocation("uV");
  program_->release();

  vao_.release();
  vbo_.release();

  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
}

void VideoWall::reconcileSlots() {
  while (static_cast<int>(slots_.size()) < desiredCount_) {
    slots_.emplace_back();
  }
  while (static_cast<int>(slots_.size()) > desiredCount_) {
    auto &s = slots_.back();
    if (s.texY) glDeleteTextures(1, &s.texY);
    if (s.texU) glDeleteTextures(1, &s.texU);
    if (s.texV) glDeleteTextures(1, &s.texV);
    slots_.pop_back();
  }
  for (auto &s : slots_) {
    auto ensure = [this](GLuint &tex) {
      if (tex != 0) return;
      glGenTextures(1, &tex);
      glBindTexture(GL_TEXTURE_2D, tex);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };
    if (s.texY == 0) {
      ensure(s.texY);
      s.yW = s.yH = 0;
    }
    if (s.texU == 0) {
      ensure(s.texU);
      s.uW = s.uH = 0;
    }
    if (s.texV == 0) {
      ensure(s.texV);
      s.vW = s.vH = 0;
    }
  }
  glBindTexture(GL_TEXTURE_2D, 0);
}

void VideoWall::uploadPlane(GLuint tex, const QImage &plane, int &cachedW,
                            int &cachedH) {
  glBindTexture(GL_TEXTURE_2D, tex);
  // Use bytesPerLine for the source row stride; QImage may pad.
  const int srcRowBytes = plane.bytesPerLine();
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, srcRowBytes);
  if (plane.width() != cachedW || plane.height() != cachedH) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, plane.width(), plane.height(), 0,
                 GL_RED, GL_UNSIGNED_BYTE, plane.constBits());
    cachedW = plane.width();
    cachedH = plane.height();
  } else {
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, plane.width(), plane.height(),
                    GL_RED, GL_UNSIGNED_BYTE, plane.constBits());
  }
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

void VideoWall::paintGL() {
  reconcileSlots();

  const qreal dpr = devicePixelRatioF();
  fbW_ = static_cast<int>(width() * dpr);
  fbH_ = static_cast<int>(height() * dpr);

  glViewport(0, 0, fbW_, fbH_);
  glClear(GL_COLOR_BUFFER_BIT);

  if (slots_.empty()) return;

  program_->bind();
  vao_.bind();
  if (locTexY_ >= 0) program_->setUniformValue(locTexY_, 0);
  if (locTexU_ >= 0) program_->setUniformValue(locTexU_, 1);
  if (locTexV_ >= 0) program_->setUniformValue(locTexV_, 2);

  for (int i = 0; i < static_cast<int>(slots_.size()); ++i) {
    auto &s = slots_[i];

    if (!s.pending.y.isNull()) {
      uploadPlane(s.texY, s.pending.y, s.yW, s.yH);
      uploadPlane(s.texU, s.pending.u, s.uW, s.uH);
      uploadPlane(s.texV, s.pending.v, s.vW, s.vH);
      s.hasFrame = true;

      const auto now = std::chrono::steady_clock::now();
      double fps = 0.0;
      if (!s.firstFrame) {
        const double dt = std::chrono::duration<double>(now - s.lastFrameTime).count();
        fps = dt > 0.0 ? 1.0 / dt : 0.0;
      }
      s.lastFrameTime = now;
      s.firstFrame = false;
      fprintf(stderr, "[stream %d] %dx%d @ %.1f fps  mime=%s\n",
              i, s.yW, s.yH, fps,
              s.mime.empty() ? "unknown" : s.mime.c_str());

      s.pending = YuvFrame{};
    }

    if (!s.hasFrame) continue;

    const QRect r = letterboxRect(cellRect(i), s.yW, s.yH);
    glViewport(r.x(), r.y(), r.width(), r.height());

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s.texY);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, s.texU);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, s.texV);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  }

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, 0);
  vao_.release();
  program_->release();
}

void VideoWall::resizeGL(int w, int h) {
  fbW_ = w;
  fbH_ = h;
}

QRect VideoWall::cellRect(int idx) const {
  const int n = static_cast<int>(slots_.size());
  if (n <= 0 || fbW_ <= 0 || fbH_ <= 0) return QRect();
  const int cols = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n))));
  const int rows = (n + cols - 1) / cols;
  const int cellW = fbW_ / cols;
  const int cellH = fbH_ / rows;
  const int row = idx / cols;
  const int col = idx % cols;
  const int x = col * cellW;
  const int y = (rows - 1 - row) * cellH;
  return QRect(x, y, cellW, cellH);
}

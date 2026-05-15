#include "videowall.h"

#include "livekitplayer.h"

#include <QFont>
#include <QHBoxLayout>
#include <QPainter>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QMessageBox>
#include <QOpenGLShaderProgram>
#include <QPushButton>
#include <QSizePolicy>
#include <QTextEdit>
#include <QVBoxLayout>

#include <chrono>
#include <cmath>
#include <cstring>
#include <numeric>

namespace {

const char *kVertSrc = R"(
#version 150 core
in vec2 aPos;
in vec2 aUV;
out vec2 vUV;
uniform vec2 uUVScale;
uniform vec2 uUVOffset;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vUV = aUV * uUVScale + uUVOffset;
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

} // namespace

// ---- VideoGrid -------------------------------------------------------------

VideoGrid::VideoGrid(QWidget *parent) : QOpenGLWidget(parent) {
  setMinimumSize(320, 180);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

VideoGrid::~VideoGrid() {
  makeCurrent();
  for (auto &s : slots_) {
    if (s.texY) glDeleteTextures(1, &s.texY);
    if (s.texU) glDeleteTextures(1, &s.texU);
    if (s.texV) glDeleteTextures(1, &s.texV);
    for (int k = 0; k < 2; ++k) {
      if (s.pboY[k]) glDeleteBuffers(1, &s.pboY[k]);
      if (s.pboU[k]) glDeleteBuffers(1, &s.pboU[k]);
      if (s.pboV[k]) glDeleteBuffers(1, &s.pboV[k]);
    }
    s.texY = s.texU = s.texV = 0;
  }
  slots_.clear();
  vbo_.destroy();
  vao_.destroy();
  doneCurrent();
}

void VideoGrid::setStreamCount(int n) {
  if (n < 0) n = 0;
  desiredCount_ = n;
  if (static_cast<int>(slots_.size()) > n) {
    for (int i = n; i < static_cast<int>(slots_.size()); ++i) {
      slots_[i].pending = YuvFrame{};
      slots_[i].hasFrame = false;
    }
  }
  slotsDirty_ = true;
  layoutDirty_ = true;
  update();
}

void VideoGrid::setActiveStream(int idx) {
  if (activeIdx_ == idx) return;
  activeIdx_ = idx;
  layoutDirty_ = true;
  update();
}

void VideoGrid::uploadFrame(int idx, const YuvFrame &frame) {
  if (idx < 0 || idx >= desiredCount_) return;
  if (idx >= static_cast<int>(slots_.size())) {
    slots_.resize(desiredCount_);
    slotsDirty_ = true;
  }
  slots_[idx].pending = frame;
  update();
}

void VideoGrid::clearAll() {
  for (auto &s : slots_) {
    s.hasFrame = false;
    s.pending = YuvFrame{};
  }
  update();
}

void VideoGrid::setSlotMime(int idx, const std::string &mime) {
  if (idx < 0 || idx >= static_cast<int>(slots_.size())) return;
  slots_[idx].mime = mime;
}

void VideoGrid::initializeGL() {
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
  locTexY_    = program_->uniformLocation("uY");
  locTexU_    = program_->uniformLocation("uU");
  locTexV_    = program_->uniformLocation("uV");
  locUVScale_ = program_->uniformLocation("uUVScale");
  locUVOffset_= program_->uniformLocation("uUVOffset");
  program_->release();

  vao_.release();
  vbo_.release();

  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
}

void VideoGrid::reconcileSlots() {
  while (static_cast<int>(slots_.size()) < desiredCount_) {
    slots_.emplace_back();
  }
  while (static_cast<int>(slots_.size()) > desiredCount_) {
    auto &s = slots_.back();
    if (s.texY) glDeleteTextures(1, &s.texY);
    if (s.texU) glDeleteTextures(1, &s.texU);
    if (s.texV) glDeleteTextures(1, &s.texV);
    for (int k = 0; k < 2; ++k) {
      if (s.pboY[k]) glDeleteBuffers(1, &s.pboY[k]);
      if (s.pboU[k]) glDeleteBuffers(1, &s.pboU[k]);
      if (s.pboV[k]) glDeleteBuffers(1, &s.pboV[k]);
    }
    slots_.pop_back();
  }
  // Keep slotAspects_ in sync with slots_; new entries default to 16:9.
  const int prevAspects = static_cast<int>(slotAspects_.size());
  slotAspects_.resize(slots_.size());
  for (int i = prevAspects; i < static_cast<int>(slotAspects_.size()); ++i)
      slotAspects_[i] = 16.f / 9.f;
  for (auto &s : slots_) {
    auto ensureTex = [this](GLuint &tex) {
      if (tex != 0) return;
      glGenTextures(1, &tex);
      glBindTexture(GL_TEXTURE_2D, tex);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };
    auto ensurePbos = [this](GLuint pbo[2]) {
      for (int k = 0; k < 2; ++k) {
        if (pbo[k] == 0) glGenBuffers(1, &pbo[k]);
      }
    };
    if (s.texY == 0) {
      ensureTex(s.texY);
      s.yW = s.yH = 0;
    }
    if (s.texU == 0) {
      ensureTex(s.texU);
      s.uW = s.uH = 0;
    }
    if (s.texV == 0) {
      ensureTex(s.texV);
      s.vW = s.vH = 0;
    }
    ensurePbos(s.pboY);
    ensurePbos(s.pboU);
    ensurePbos(s.pboV);
  }
  glBindTexture(GL_TEXTURE_2D, 0);
}

void VideoGrid::uploadPlane(GLuint tex, GLuint pbo[2], int pboSize[2],
                            int pboIdx, const std::uint8_t *data, int w, int h,
                            int &cachedW, int &cachedH) {
  const int bytes = w * h;
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo[pboIdx]);
  if (pboSize[pboIdx] != bytes) {
    glBufferData(GL_PIXEL_UNPACK_BUFFER, bytes, nullptr, GL_STREAM_DRAW);
    pboSize[pboIdx] = bytes;
  }
  void *p = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, bytes,
                             GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT |
                                 GL_MAP_UNSYNCHRONIZED_BIT);
  if (p) {
    std::memcpy(p, data, bytes);
    glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
  }
  glBindTexture(GL_TEXTURE_2D, tex);
  if (w != cachedW || h != cachedH) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE,
                 nullptr);
    cachedW = w;
    cachedH = h;
  }
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RED, GL_UNSIGNED_BYTE,
                  nullptr);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
}

void VideoGrid::paintGL() {
  if (slotsDirty_) {
    reconcileSlots();
    slotsDirty_ = false;
    layoutDirty_ = true;
  }

  const qreal dpr = devicePixelRatioF();
  fbW_ = static_cast<int>(width() * dpr);
  fbH_ = static_cast<int>(height() * dpr);

  if (layoutDirty_) recomputeLayout();

  QPainter painter(this);
  painter.beginNativePainting();
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);  // YUV planes are tightly packed

  glViewport(0, 0, fbW_, fbH_);
  glClear(GL_COLOR_BUFFER_BIT);

  if (!slots_.empty()) {
    program_->bind();
    vao_.bind();
    if (locTexY_ >= 0) program_->setUniformValue(locTexY_, 0);
    if (locTexU_ >= 0) program_->setUniformValue(locTexU_, 1);
    if (locTexV_ >= 0) program_->setUniformValue(locTexV_, 2);

    for (int i = 0; i < static_cast<int>(slots_.size()); ++i) {
      auto &s = slots_[i];

      if (s.pending.y) {
        const int w = s.pending.width;
        const int h = s.pending.height;
        const int cw = w / 2;
        const int ch = h / 2;
        const int idx = s.pboIndex;
        // Detect first frame or resolution change before uploadPlane overwrites s.yW/s.yH.
        if ((w != s.yW || h != s.yH) && w > 0 && h > 0
                && i < static_cast<int>(slotAspects_.size())) {
          slotAspects_[i] = static_cast<float>(w) / static_cast<float>(h);
          layoutDirty_ = true;
        }
        uploadPlane(s.texY, s.pboY, s.pboYSize, idx, s.pending.y->data(), w,  h,  s.yW, s.yH);
        uploadPlane(s.texU, s.pboU, s.pboUSize, idx, s.pending.u->data(), cw, ch, s.uW, s.uH);
        uploadPlane(s.texV, s.pboV, s.pboVSize, idx, s.pending.v->data(), cw, ch, s.vW, s.vH);
        s.pboIndex ^= 1;
        s.hasFrame = true;
        s.pending = YuvFrame{};
      }

      if (!s.hasFrame) continue;

      // layout_ uses top-left origin; glViewport needs bottom-left origin.
      const QRect cell = (i < static_cast<int>(layout_.size())) ? layout_[i] : QRect();
      if (cell.isEmpty()) continue;
      const int glY = fbH_ - cell.y() - cell.height();
      const QRect glCell(cell.x(), glY, cell.width(), cell.height());
      glViewport(glCell.x(), glCell.y(), glCell.width(), glCell.height());

      // UV cover crop: zoom into the video so it fills the cell with no black bars.
      float uvSx = 1.f, uvSy = 1.f, uvOx = 0.f, uvOy = 0.f;
      if (s.yW > 0 && s.yH > 0) {
        const float Ca = static_cast<float>(glCell.width())  / static_cast<float>(glCell.height());
        const float Va = static_cast<float>(s.yW) / static_cast<float>(s.yH);
        if (Ca > Va + 1e-4f) {
          // Cell is wider than video → crop top/bottom
          uvSy = Va / Ca;
          uvOy = (1.f - uvSy) * 0.5f;
        } else if (Va > Ca + 1e-4f) {
          // Cell is taller than video → crop left/right
          uvSx = Ca / Va;
          uvOx = (1.f - uvSx) * 0.5f;
        }
      }
      if (locUVScale_  >= 0) program_->setUniformValue(locUVScale_,  uvSx, uvSy);
      if (locUVOffset_ >= 0) program_->setUniformValue(locUVOffset_, uvOx, uvOy);

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

  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);  // restore default so QPainter glyph uploads work
  painter.endNativePainting();

  // Number overlay — QPainter coords are logical pixels; layout_ is device pixels.
  if (!slots_.empty() && !visualNumbers_.empty()) {
    QFont font = painter.font();
    font.setPixelSize(18);
    font.setBold(true);
    painter.setFont(font);
    constexpr int kMargin = 6;

    for (int i = 0; i < static_cast<int>(slots_.size()); ++i) {
      if (i >= static_cast<int>(layout_.size())) continue;
      const QRect &cellDev = layout_[i];
      if (cellDev.isEmpty()) continue;

      const QRectF cellLog(cellDev.x() / dpr, cellDev.y() / dpr,
                           cellDev.width() / dpr, cellDev.height() / dpr);
      const QString label = QString::number(visualNumbers_[i]);
      const QRectF textRect(cellLog.x() + kMargin, cellLog.y() + kMargin,
                            cellLog.width() - kMargin, cellLog.height() - kMargin);

      painter.setPen(Qt::black);
      painter.drawText(textRect.translated(1.0, 1.0), Qt::AlignTop | Qt::AlignLeft, label);
      painter.setPen(Qt::white);
      painter.drawText(textRect, Qt::AlignTop | Qt::AlignLeft, label);
    }
  }
}

void VideoGrid::resizeGL(int w, int h) {
  fbW_ = w;
  fbH_ = h;
  layoutDirty_ = true;
}

void VideoGrid::recomputeLayout() {
  const int n = static_cast<int>(slots_.size());
  const LayoutEngine::JustifiedConfig cfg{ layoutGap_, minRowH_, maxRowH_ };
  layout_ = layoutEngine_.compute(n, fbW_, fbH_, activeIdx_, slotAspects_, cfg);
  computeVisualNumbers();
  layoutDirty_ = false;
}

void VideoGrid::computeVisualNumbers() {
  const int n = static_cast<int>(slots_.size());
  visualNumbers_.assign(n, 0);
  if (n == 0) return;

  std::vector<int> order(n);
  std::iota(order.begin(), order.end(), 0);
  std::sort(order.begin(), order.end(), [this](int a, int b) {
    if (layout_[a].y() != layout_[b].y()) return layout_[a].y() < layout_[b].y();
    return layout_[a].x() < layout_[b].x();
  });

  if (activeIdx_ >= 0 && activeIdx_ < n) {
    visualNumbers_[activeIdx_] = 1;
    int counter = 2;
    for (int idx : order) {
      if (idx != activeIdx_) visualNumbers_[idx] = counter++;
    }
  } else {
    for (int rank = 0; rank < n; ++rank)
      visualNumbers_[order[rank]] = rank + 1;
  }
}

// ---- VideoWall -------------------------------------------------------------

VideoWall::VideoWall(QWidget *parent) : QWidget(parent) {
  createUi();
}

VideoWall::~VideoWall() {
  shutdownAllPlayers();
}

void VideoWall::createUi() {
  auto *mainLayout = new QVBoxLayout(this);

  auto *bearerLabel = new QLabel(QStringLiteral("Bearer JWT:"), this);
  bearerEdit_ = new QTextEdit(this);
  bearerEdit_->setMaximumHeight(50);
  QFile tokenFile(QStringLiteral("token"));

  if (tokenFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
    QTextStream in(&tokenFile);
    bearerEdit_->setPlainText(in.readAll().trimmed());
    tokenFile.close();
  } else {
    bearerEdit_->setPlaceholderText(QStringLiteral("Failed to load ../token"));
  }

  QFont monoFont = bearerEdit_->font();
  monoFont.setStyleHint(QFont::Monospace);
  monoFont.setFamily(QStringLiteral("Menlo"));
  bearerEdit_->setFont(monoFont);
  bearerEdit_->setLineWrapMode(QTextEdit::WidgetWidth);

  auto *jsonLabel =
      new QLabel(QStringLiteral("Cameras JSON (array of descriptors):"), this);
  jsonEdit_ = new QTextEdit(this);
  jsonEdit_->setMinimumHeight(80);
  jsonEdit_->setMaximumHeight(120);
  jsonEdit_->setPlainText(QStringLiteral(
    "[\n"
    "  {\n"
    "    \"participantName\": \"auth0|6363bdb451bcdda4f909db23-415bf0fd-f5dc-4b3a-b543-b5cedec53a04\",\n"
    "    \"edgeId\": \"6953cff92a13ade0364679ec\",\n"
    "    \"cameraId\": \"6953ebd855947949135d3dfd\",\n"
    "    \"resolution\": 3,\n"
    "    \"errorCounter\": 0\n"
    "  }\n"
    "]"));

  auto *buttonsLayout = new QHBoxLayout();
  playButton_ = new QPushButton(QStringLiteral("Play All"), this);
  stopButton_ = new QPushButton(QStringLiteral("Stop All"), this);
  stopButton_->setEnabled(false);
  buttonsLayout->addWidget(playButton_);
  buttonsLayout->addWidget(stopButton_);
  buttonsLayout->addStretch();

  connect(playButton_, &QPushButton::clicked, this, &VideoWall::startPlayback);
  connect(stopButton_, &QPushButton::clicked, this, &VideoWall::stopPlayback);

  grid_ = new VideoGrid(this);
  statusLabel_ = new QLabel(QStringLiteral("Idle"), this);

  fieldsWidget_ = new QWidget(this);
  auto *fieldsLayout = new QVBoxLayout(fieldsWidget_);
  fieldsLayout->setContentsMargins(0, 0, 0, 0);
  fieldsLayout->setSpacing(mainLayout->spacing());
  fieldsLayout->addWidget(bearerLabel);
  fieldsLayout->addWidget(bearerEdit_);
  fieldsLayout->addWidget(jsonLabel);
  fieldsLayout->addWidget(jsonEdit_);

  mainLayout->addWidget(fieldsWidget_);
  mainLayout->addLayout(buttonsLayout);
  mainLayout->addWidget(grid_, 1);
  mainLayout->addWidget(statusLabel_);
}

bool VideoWall::parseJsonInput() {
  cameras_.clear();
  bearerJwt_.clear();

  const QString jwt = bearerEdit_->toPlainText().trimmed();
  if (jwt.isEmpty()) {
    QMessageBox::warning(this, QStringLiteral("Missing Bearer JWT"),
                         QStringLiteral("Please paste the Bearer JWT."));
    return false;
  }

  const QByteArray jsonData = jsonEdit_->toPlainText().toUtf8();
  QJsonParseError perr{};
  const QJsonDocument jsonDoc = QJsonDocument::fromJson(jsonData, &perr);

  if (perr.error != QJsonParseError::NoError || !jsonDoc.isArray()) {
    QMessageBox::warning(
        this, QStringLiteral("Invalid JSON"),
        QStringLiteral("Please enter a JSON array of camera descriptors. %1")
            .arg(perr.errorString()));
    return false;
  }

  const QJsonArray arr = jsonDoc.array();
  if (arr.isEmpty()) {
    QMessageBox::warning(this, QStringLiteral("Empty JSON"),
                         QStringLiteral("The camera array is empty."));
    return false;
  }

  QVector<QJsonObject> parsed;
  parsed.reserve(arr.size());
  for (int i = 0; i < arr.size(); ++i) {
    if (!arr[i].isObject()) {
      QMessageBox::warning(
          this, QStringLiteral("Invalid Entry"),
          QStringLiteral("Entry %1 is not a JSON object.").arg(i));
      return false;
    }
    const QJsonObject obj = arr[i].toObject();
    const QString participantName =
        obj.value(QStringLiteral("participantName")).toString();
    const QString edgeId = obj.value(QStringLiteral("edgeId")).toString();
    const QString cameraId = obj.value(QStringLiteral("cameraId")).toString();
    const QJsonValue resolution = obj.value(QStringLiteral("resolution"));
    if (participantName.isEmpty() || edgeId.isEmpty() || cameraId.isEmpty()) {
      QMessageBox::warning(
          this, QStringLiteral("Missing Fields"),
          QStringLiteral("Entry %1 is missing 'participantName', 'edgeId', "
                         "or 'cameraId'.")
              .arg(i));
      return false;
    }
    if (!resolution.isDouble()) {
      QMessageBox::warning(
          this, QStringLiteral("Missing Fields"),
          QStringLiteral("Entry %1 is missing numeric 'resolution'.").arg(i));
      return false;
    }
    parsed.append(obj);
  }

  cameras_ = parsed;
  bearerJwt_ = jwt;
  statusLabel_->setText(
      QStringLiteral("Parsed %1 camera(s).").arg(cameras_.size()));
  return true;
}

void VideoWall::startPlayback() {
  if (!parseJsonInput() || cameras_.isEmpty()) {
    return;
  }
  setButtonStates(true);

  pauseAllPlayers();
  grid_->setStreamCount(cameras_.size());

  ensurePlayers(cameras_.size());
  for (int i = 0; i < cameras_.size(); ++i) {
    connectPlayerSignals(i);
    players_[i]->startPlayback(bearerJwt_, cameras_[i]);
  }
}

void VideoWall::stopPlayback() {
  pauseAllPlayers();
  grid_->clearAll();
  setButtonStates(false);
  statusLabel_->setText(QStringLiteral("Stopped"));
}

void VideoWall::pauseAllPlayers() {
  for (auto *player : players_) {
    disconnect(player, nullptr, this, nullptr);
    player->pauseReceiving();
  }
}

void VideoWall::shutdownAllPlayers() {
  for (auto *player : players_) {
    disconnect(player, nullptr, this, nullptr);
    player->shutdownPlayback();
    delete player;
  }
  players_.clear();
}

void VideoWall::ensurePlayers(int streamCount) {
  while (players_.size() < streamCount) {
    players_.append(new LiveKitPlayer(this));
  }
  while (players_.size() > streamCount) {
    auto *player = players_.takeLast();
    disconnect(player, nullptr, this, nullptr);
    player->shutdownPlayback();
    delete player;
  }
}

void VideoWall::connectPlayerSignals(int slotIndex) {
  LiveKitPlayer *player = players_[slotIndex];
  disconnect(player, nullptr, this, nullptr);

  connect(player, &LiveKitPlayer::frameReady, this,
          [this, player, slotIndex](const YuvFrame &frame) {
            player->clearFrameInFlight();
            grid_->uploadFrame(slotIndex, frame);
          });
  connect(player, &LiveKitPlayer::mimeTypeReceived, this,
          [this, slotIndex](const std::string &mime) {
            grid_->setSlotMime(slotIndex, mime);
          });
  connect(player, &LiveKitPlayer::statusChanged, this,
          [this, slotIndex](const QString &s) {
            onSlotStatusChanged(slotIndex, s);
          });
  connect(player, &LiveKitPlayer::errorOccurred, this,
          [this, slotIndex](const QString &e) {
            onSlotError(slotIndex, e);
          });
}

void VideoWall::onSlotStatusChanged(int slotIndex, const QString &status) {
  statusLabel_->setText(
      QStringLiteral("Slot %1: %2").arg(slotIndex).arg(status));
}

void VideoWall::onSlotError(int slotIndex, const QString &errorMessage) {
  statusLabel_->setText(
      QStringLiteral("Slot %1 error: %2").arg(slotIndex).arg(errorMessage));
}

void VideoWall::setButtonStates(bool isPlaying) {
  fieldsWidget_->setVisible(!isPlaying);
  playButton_->setEnabled(!isPlaying);
  stopButton_->setEnabled(isPlaying);
}

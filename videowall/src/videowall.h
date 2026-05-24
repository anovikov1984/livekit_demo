#pragma once

#include <QJsonObject>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>
#include <QRect>
#include <QString>
#include <QVector>
#include <QWidget>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "layoutengine.h"
#include "yuvframe.h"

class QLabel;
class QOpenGLShaderProgram;
class QPushButton;
class QTextEdit;
class QTimer;
class LiveKitPlayer;

// OpenGL grid renderer: N I420 streams as quads in a single QOpenGLWidget.
// YUV→RGB conversion happens in the fragment shader (BT.601 limited).
class VideoGrid : public QOpenGLWidget, protected QOpenGLFunctions {
  Q_OBJECT

public:
  explicit VideoGrid(QWidget *parent = nullptr);
  ~VideoGrid() override;

  void setStreamCount(int n);
  void setActiveStream(int idx);  // idx < 0 clears dominant mode
  void uploadFrame(int idx, const YuvFrame &frame);
  void setSlotMime(int idx, const std::string &mime);
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
    GLuint pboY[2]{0, 0};
    GLuint pboU[2]{0, 0};
    GLuint pboV[2]{0, 0};
    int pboYSize[2]{0, 0};
    int pboUSize[2]{0, 0};
    int pboVSize[2]{0, 0};
    int pboIndex{0};
    int yW{0};
    int yH{0};
    int uW{0};
    int uH{0};
    int vW{0};
    int vH{0};
    bool hasFrame{false};
    YuvFrame pending;
    std::string mime;
  };

  void reconcileSlots();
  void uploadPlane(GLuint tex, GLuint pbo[2], int pboSize[2], int pboIdx,
                   const std::uint8_t *src, int srcStride, int w, int h,
                   int &cachedW, int &cachedH);
  void recomputeLayout();
  void computeVisualNumbers();

  QOpenGLShaderProgram *program_{nullptr};
  QOpenGLBuffer vbo_;
  QOpenGLVertexArrayObject vao_;
  std::vector<Slot> slots_;
  std::vector<QRect> layout_;   // one rect per slot, framebuffer coords
  std::vector<int> visualNumbers_; // 1-based visual number per slot
  int desiredCount_{0};
  bool slotsDirty_{true};
  bool layoutDirty_{true};
  int activeIdx_{-1};
  int fbW_{0};
  int fbH_{0};
  int locTexY_{-1};
  int locTexU_{-1};
  int locTexV_{-1};
  int locUVScale_{-1};
  int locUVOffset_{-1};
  LayoutEngine layoutEngine_;
  std::vector<float> slotAspects_;   // one per slot; default 16/9
  int   layoutGap_{4};               // device-px gap between tiles
  float minRowH_{80.0f};             // device-px min row height
  float maxRowH_{2000.0f};           // device-px max row height
};

// Top-level composite widget: bearer JWT + camera-descriptor JSON inputs,
// Play/Stop buttons, status label, and an inner VideoGrid. Owns the
// per-stream LiveKitPlayer instances.
class VideoWall : public QWidget {
  Q_OBJECT

public:
  explicit VideoWall(QWidget *parent = nullptr);
  ~VideoWall() override;

private slots:
  void startPlayback();
  void stopPlayback();
  void onSlotStatusChanged(int slotIndex, const QString &status);
  void onSlotError(int slotIndex, const QString &errorMessage);
  void checkSlotStalls();

private:
  void createUi();
  void setButtonStates(bool isPlaying);
  bool parseJsonInput();
  void pauseAllPlayers();
  void shutdownAllPlayers();
  void ensurePlayers(int streamCount);
  void connectPlayerSignals(int slotIndex);

  QWidget *fieldsWidget_{nullptr};
  QTextEdit *bearerEdit_{nullptr};
  QTextEdit *jsonEdit_{nullptr};
  QLabel *statusLabel_{nullptr};
  QPushButton *playButton_{nullptr};
  QPushButton *stopButton_{nullptr};
  VideoGrid *grid_{nullptr};

  QString bearerJwt_;
  QVector<QJsonObject> cameras_;
  QVector<LiveKitPlayer *> players_;
  QVector<bool> slotFirstFrameLogged_;
  QVector<qint64> slotLastFrameMs_;
  QVector<bool> slotStalled_;
  QTimer *watchdogTimer_{nullptr};
};

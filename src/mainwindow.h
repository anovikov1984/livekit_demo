#pragma once

#include <QMainWindow>

class QLabel;
class QLineEdit;
class QPushButton;
class QTextEdit;
class QImage;

class LiveKitPlayer;

class MainWindow : public QMainWindow {
  Q_OBJECT

public:
  explicit MainWindow(QWidget *parent = nullptr);
  ~MainWindow() override;

private slots:
  void parseJsonInput();
  void playFromFields();
  void stopPlayback();
  void onFrameReady(const QImage &frame);
  void onStatusChanged(const QString &status);
  void onError(const QString &errorMessage);

private:
  void createUi();
  void setButtonStates(bool isPlaying);

  QTextEdit *jsonEdit_{nullptr};
  QLineEdit *tokenEdit_{nullptr};
  QLineEdit *apiUrlEdit_{nullptr};
  QLabel *videoLabel_{nullptr};
  QLabel *statusLabel_{nullptr};
  QPushButton *playButton_{nullptr};
  QPushButton *stopButton_{nullptr};
  LiveKitPlayer *player_{nullptr};
};

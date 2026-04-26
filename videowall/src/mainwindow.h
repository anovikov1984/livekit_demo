#pragma once

#include <QMainWindow>
#include <QVector>
#include <QPair>
#include <QString>

class QGridLayout;
class QLabel;
class QPushButton;
class QTextEdit;
class QWidget;
class QImage;

class LiveKitPlayer;

class MainWindow : public QMainWindow {
  Q_OBJECT

public:
  explicit MainWindow(QWidget *parent = nullptr);
  ~MainWindow() override;

private slots:
  void parseJsonInput();
  void startPlayback();
  void stopPlayback();
  void onFrameReady(int index, const QImage &frame);
  void onStatusChanged(const QString &status);
  void onError(const QString &errorMessage);

private:
  void createUi();
  void setButtonStates(bool isPlaying);
  void clearPlayers();
  void rebuildGrid();

  QTextEdit *jsonEdit_{nullptr};
  QLabel *statusLabel_{nullptr};
  QPushButton *playButton_{nullptr};
  QPushButton *stopButton_{nullptr};

  QWidget *videoGridWidget_{nullptr};
  QGridLayout *videoGrid_{nullptr};
  QVector<QLabel*> videoLabels_;
  QVector<LiveKitPlayer*> players_;
  QVector<QPair<QString, QString>> streams_; // {token, apiUrl}
};

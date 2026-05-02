#pragma once

#include <QMainWindow>
#include <QVector>
#include <QPair>
#include <QString>

class QLabel;
class QPushButton;
class QTextEdit;
class QWidget;

class LiveKitPlayer;
class VideoWall;

class MainWindow : public QMainWindow {
  Q_OBJECT

public:
  explicit MainWindow(QWidget *parent = nullptr);
  ~MainWindow() override;

private slots:
  void parseJsonInput();
  void startPlayback();
  void stopPlayback();
  void onStatusChanged(const QString &status);
  void onError(const QString &errorMessage);

private:
  void createUi();
  void setButtonStates(bool isPlaying);
  void clearPlayers();

  QTextEdit *jsonEdit_{nullptr};
  QLabel *statusLabel_{nullptr};
  QPushButton *playButton_{nullptr};
  QPushButton *stopButton_{nullptr};

  VideoWall *videoWall_{nullptr};
  QVector<LiveKitPlayer*> players_;
  QVector<QPair<QString, QString>> streams_; // {token, apiUrl}
};

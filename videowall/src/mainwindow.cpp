#include "mainwindow.h"
#include "livekitplayer.h"
#include "videowall.h"

#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QWidget>

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
  createUi();
}

MainWindow::~MainWindow() {
  clearPlayers();
}

void MainWindow::parseJsonInput() {
  streams_.clear();
  const QByteArray jsonData = jsonEdit_->toPlainText().toUtf8();
  const QJsonDocument jsonDoc = QJsonDocument::fromJson(jsonData);

  if (!jsonDoc.isArray()) {
    QMessageBox::warning(this, QStringLiteral("Invalid JSON"),
                         QStringLiteral("Please enter a JSON array of stream objects."));
    return;
  }

  const QJsonArray arr = jsonDoc.array();
  if (arr.isEmpty()) {
    QMessageBox::warning(this, QStringLiteral("Empty JSON"),
                         QStringLiteral("The stream array is empty."));
    return;
  }

  QVector<QPair<QString, QString>> parsed;
  for (int i = 0; i < arr.size(); ++i) {
    const QJsonObject obj = arr[i].toObject();
    const QString token = obj.value(QStringLiteral("token")).toString();
    const QString apiUrl = obj.value(QStringLiteral("apiUrl")).toString();
    if (token.isEmpty() || apiUrl.isEmpty()) {
      QMessageBox::warning(this, QStringLiteral("Missing Fields"),
                           QStringLiteral("Stream %1 is missing 'token' or 'apiUrl'.").arg(i + 1));
      return;
    }
    parsed.append({token, apiUrl});
  }

  streams_ = parsed;
  statusLabel_->setText(QStringLiteral("Parsed %1 stream(s).").arg(streams_.size()));
}

void MainWindow::startPlayback() {
  parseJsonInput();
  if (streams_.isEmpty()) {
    return;
  }

  clearPlayers();
  videoWall_->setStreamCount(streams_.size());
  setButtonStates(true);

  for (int i = 0; i < streams_.size(); ++i) {
    auto *player = new LiveKitPlayer(this);
    VideoWall *wall = videoWall_;

    connect(player, &LiveKitPlayer::frameReady, this,
            [player, wall, i](const YuvFrame &frame) {
              player->clearFrameInFlight();
              wall->uploadFrame(i, frame);
            });
    connect(player, &LiveKitPlayer::statusChanged, this, &MainWindow::onStatusChanged);
    connect(player, &LiveKitPlayer::errorOccurred, this, &MainWindow::onError);

    players_.append(player);
    player->startPlayback(streams_[i].second, streams_[i].first);
  }
}

void MainWindow::stopPlayback() {
  clearPlayers();
  setButtonStates(false);
  statusLabel_->setText(QStringLiteral("Stopped"));
  videoWall_->clearAll();
}

void MainWindow::onStatusChanged(const QString &status) {
  statusLabel_->setText(status);
}

void MainWindow::onError(const QString &errorMessage) {
  QMessageBox::critical(this, QStringLiteral("LiveKit Error"), errorMessage);
}

void MainWindow::clearPlayers() {
  for (auto *player : players_) {
    player->disconnect();
    player->stopPlayback();
    delete player;
  }
  players_.clear();
}

void MainWindow::createUi() {
  auto *centralWidget = new QWidget(this);
  auto *mainLayout = new QVBoxLayout(centralWidget);

  auto *jsonLabel = new QLabel(QStringLiteral("LiveKit streams JSON (array):"), centralWidget);
  jsonEdit_ = new QTextEdit(centralWidget);
  jsonEdit_->setMinimumHeight(120);
  jsonEdit_->setMaximumHeight(160);
  jsonEdit_->setPlainText(QStringLiteral(
      "[\n"
      "  { \"token\": \"\", \"apiUrl\": \"wss://your-livekit-url\" },\n"
      "  { \"token\": \"\", \"apiUrl\": \"wss://your-livekit-url\" }\n"
      "]"));

  auto *buttonsLayout = new QHBoxLayout();
  playButton_ = new QPushButton(QStringLiteral("Play All"), centralWidget);
  stopButton_ = new QPushButton(QStringLiteral("Stop All"), centralWidget);
  stopButton_->setEnabled(false);
  buttonsLayout->addWidget(playButton_);
  buttonsLayout->addWidget(stopButton_);
  buttonsLayout->addStretch();

  connect(playButton_, &QPushButton::clicked, this, &MainWindow::startPlayback);
  connect(stopButton_, &QPushButton::clicked, this, &MainWindow::stopPlayback);

  videoWall_ = new VideoWall(centralWidget);
  videoWall_->setStreamCount(1);

  statusLabel_ = new QLabel(QStringLiteral("Idle"), centralWidget);

  mainLayout->addWidget(jsonLabel);
  mainLayout->addWidget(jsonEdit_);
  mainLayout->addLayout(buttonsLayout);
  mainLayout->addWidget(videoWall_, 1);
  mainLayout->addWidget(statusLabel_);

  setCentralWidget(centralWidget);
  resize(1280, 960);
  setWindowTitle(QStringLiteral("LiveKit Videowall"));
}

void MainWindow::setButtonStates(bool isPlaying) {
  playButton_->setEnabled(!isPlaying);
  stopButton_->setEnabled(isPlaying);
}

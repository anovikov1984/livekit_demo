#include "mainwindow.h"
#include "livekitplayer.h"
#include "videocell.h"

#include <QGridLayout>
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
#include <cmath>

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
  rebuildGrid();
  setButtonStates(true);

  for (int i = 0; i < streams_.size(); ++i) {
    auto *player = new LiveKitPlayer(this);
    VideoCell *cell = videoCells_[i];

    connect(player, &LiveKitPlayer::frameReady, this, [player, cell](const QImage &frame) {
      player->clearFrameInFlight();
      cell->uploadFrame(frame);
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
  for (auto *cell : videoCells_) {
    cell->clearFrame();
  }
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

void MainWindow::rebuildGrid() {
  for (auto *cell : videoCells_) {
    videoGrid_->removeWidget(cell);
    delete cell;
  }
  videoCells_.clear();

  const int n = streams_.size();
  const int cols = static_cast<int>(std::ceil(std::sqrt(n)));

  for (int i = 0; i < n; ++i) {
    auto *cell = new VideoCell(videoGridWidget_);
    videoGrid_->addWidget(cell, i / cols, i % cols);
    videoCells_.append(cell);
  }
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

  videoGridWidget_ = new QWidget(centralWidget);
  videoGridWidget_->setStyleSheet(QStringLiteral("background-color: black;"));
  videoGrid_ = new QGridLayout(videoGridWidget_);
  videoGrid_->setSpacing(4);
  videoGrid_->setContentsMargins(0, 0, 0, 0);

  // Placeholder cell until Play is pressed
  auto *placeholder = new VideoCell(videoGridWidget_);
  videoGrid_->addWidget(placeholder, 0, 0);
  videoCells_.append(placeholder);

  statusLabel_ = new QLabel(QStringLiteral("Idle"), centralWidget);

  mainLayout->addWidget(jsonLabel);
  mainLayout->addWidget(jsonEdit_);
  mainLayout->addLayout(buttonsLayout);
  mainLayout->addWidget(videoGridWidget_, 1);
  mainLayout->addWidget(statusLabel_);

  setCentralWidget(centralWidget);
  resize(1280, 960);
  setWindowTitle(QStringLiteral("LiveKit Videowall"));
}

void MainWindow::setButtonStates(bool isPlaying) {
  playButton_->setEnabled(!isPlaying);
  stopButton_->setEnabled(isPlaying);
}

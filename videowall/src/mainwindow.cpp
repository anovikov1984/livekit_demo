#include "mainwindow.h"
#include "livekitplayer.h"

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

    connect(player, &LiveKitPlayer::frameReady, this, [this, i](const QImage &frame) {
      onFrameReady(i, frame);
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

  for (auto *label : videoLabels_) {
    label->setText(QStringLiteral("No video"));
    label->setPixmap(QPixmap());
  }
}

void MainWindow::onFrameReady(int index, const QImage &frame) {
  if (index < 0 || index >= videoLabels_.size()) {
    return;
  }
  QLabel *label = videoLabels_[index];
  const QPixmap pixmap = QPixmap::fromImage(frame).scaled(
      label->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
  label->setPixmap(pixmap);
}

void MainWindow::onStatusChanged(const QString &status) {
  statusLabel_->setText(status);
}

void MainWindow::onError(const QString &errorMessage) {
  QMessageBox::critical(this, QStringLiteral("LiveKit Error"), errorMessage);
}

void MainWindow::clearPlayers() {
  for (auto *player : players_) {
    player->disconnect(); // prevent queued signals hitting a deleted object
    player->stopPlayback();
    delete player;
  }
  players_.clear();
}

void MainWindow::rebuildGrid() {
  // Remove existing labels from grid
  for (auto *label : videoLabels_) {
    videoGrid_->removeWidget(label);
    delete label;
  }
  videoLabels_.clear();

  const int n = streams_.size();
  const int cols = static_cast<int>(std::ceil(std::sqrt(n)));
  const int rows = (n + cols - 1) / cols;

  for (int i = 0; i < n; ++i) {
    auto *label = new QLabel(QStringLiteral("Connecting..."), videoGridWidget_);
    label->setMinimumSize(320, 180);
    label->setStyleSheet(QStringLiteral("background-color: black; color: #cccccc;"));
    label->setAlignment(Qt::AlignCenter);
    label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    videoGrid_->addWidget(label, i / cols, i % cols);
    videoLabels_.append(label);
  }

  Q_UNUSED(rows);
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
  videoGrid_ = new QGridLayout(videoGridWidget_);
  videoGrid_->setSpacing(4);

  // Placeholder until Play is pressed
  auto *placeholderLabel = new QLabel(QStringLiteral("No video"), videoGridWidget_);
  placeholderLabel->setMinimumSize(640, 360);
  placeholderLabel->setStyleSheet(QStringLiteral("background-color: black; color: #cccccc;"));
  placeholderLabel->setAlignment(Qt::AlignCenter);
  videoGrid_->addWidget(placeholderLabel, 0, 0);
  videoLabels_.append(placeholderLabel);

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

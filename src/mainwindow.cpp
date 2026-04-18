#include "mainwindow.h"

#include "livekitplayer.h"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QWidget>

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
  createUi();

  player_ = new LiveKitPlayer(this);
  connect(player_, &LiveKitPlayer::frameReady, this, &MainWindow::onFrameReady);
  connect(player_, &LiveKitPlayer::statusChanged, this,
          &MainWindow::onStatusChanged);
  connect(player_, &LiveKitPlayer::errorOccurred, this, &MainWindow::onError);
}

MainWindow::~MainWindow() = default;

void MainWindow::parseJsonInput() {
  const QByteArray jsonData = jsonEdit_->toPlainText().toUtf8();
  const QJsonDocument jsonDoc = QJsonDocument::fromJson(jsonData);
  if (!jsonDoc.isObject()) {
    QMessageBox::warning(this, QStringLiteral("Invalid JSON"),
                         QStringLiteral("Please enter a JSON object."));
    return;
  }

  const QJsonObject root = jsonDoc.object();
  const QString token = root.value(QStringLiteral("token")).toString();
  const QString apiUrl = root.value(QStringLiteral("apiUrl")).toString();

  if (token.isEmpty() || apiUrl.isEmpty()) {
    QMessageBox::warning(this, QStringLiteral("Missing Fields"),
                         QStringLiteral("JSON must contain 'token' and 'apiUrl'."));
    return;
  }

  tokenEdit_->setText(token);
  apiUrlEdit_->setText(apiUrl);
  statusLabel_->setText(QStringLiteral("JSON parsed successfully."));
}

void MainWindow::playFromFields() {
  if (tokenEdit_->text().trimmed().isEmpty() || apiUrlEdit_->text().trimmed().isEmpty()) {
    parseJsonInput();
  }

  const QString token = tokenEdit_->text().trimmed();
  const QString apiUrl = apiUrlEdit_->text().trimmed();
  if (token.isEmpty() || apiUrl.isEmpty()) {
    return;
  }

  setButtonStates(true);
  videoLabel_->setText(QStringLiteral("Connecting..."));
  player_->startPlayback(apiUrl, token);
}

void MainWindow::stopPlayback() {
  player_->stopPlayback();
  setButtonStates(false);
  statusLabel_->setText(QStringLiteral("Stopped"));
  videoLabel_->setText(QStringLiteral("No video"));
}

void MainWindow::onFrameReady(const QImage &frame) {
  const QPixmap pixmap = QPixmap::fromImage(frame).scaled(
      videoLabel_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
  videoLabel_->setPixmap(pixmap);
}

void MainWindow::onStatusChanged(const QString &status) { statusLabel_->setText(status); }

void MainWindow::onError(const QString &errorMessage) {
  setButtonStates(false);
  QMessageBox::critical(this, QStringLiteral("LiveKit Error"), errorMessage);
}

void MainWindow::createUi() {
  auto *centralWidget = new QWidget(this);
  auto *mainLayout = new QVBoxLayout(centralWidget);

  auto *jsonLabel = new QLabel(QStringLiteral("LiveKit credentials JSON:"), centralWidget);
  jsonEdit_ = new QTextEdit(centralWidget);
  jsonEdit_->setMinimumHeight(180);
  jsonEdit_->setPlainText(QStringLiteral(
      "{\n"
      "  \"token\": \"\",\n"
      "  \"apiUrl\": \"wss://your-livekit-url\"\n"
      "}"));

  auto *parseButton = new QPushButton(QStringLiteral("Parse JSON"), centralWidget);
  connect(parseButton, &QPushButton::clicked, this, &MainWindow::parseJsonInput);

  tokenEdit_ = new QLineEdit(centralWidget);
  tokenEdit_->setPlaceholderText(QStringLiteral("JWT token"));
  apiUrlEdit_ = new QLineEdit(centralWidget);
  apiUrlEdit_->setPlaceholderText(QStringLiteral("wss://your-livekit-server"));

  auto *formLayout = new QFormLayout();
  formLayout->addRow(QStringLiteral("Token:"), tokenEdit_);
  formLayout->addRow(QStringLiteral("API URL:"), apiUrlEdit_);

  auto *buttonsLayout = new QHBoxLayout();
  playButton_ = new QPushButton(QStringLiteral("Play"), centralWidget);
  stopButton_ = new QPushButton(QStringLiteral("Stop"), centralWidget);
  stopButton_->setEnabled(false);
  buttonsLayout->addWidget(playButton_);
  buttonsLayout->addWidget(stopButton_);
  buttonsLayout->addStretch();

  connect(playButton_, &QPushButton::clicked, this, &MainWindow::playFromFields);
  connect(stopButton_, &QPushButton::clicked, this, &MainWindow::stopPlayback);

  videoLabel_ = new QLabel(QStringLiteral("No video"), centralWidget);
  videoLabel_->setMinimumSize(960, 540);
  videoLabel_->setStyleSheet(QStringLiteral("background-color: black; color: #cccccc;"));
  videoLabel_->setAlignment(Qt::AlignCenter);

  statusLabel_ = new QLabel(QStringLiteral("Idle"), centralWidget);

  mainLayout->addWidget(jsonLabel);
  mainLayout->addWidget(jsonEdit_);
  mainLayout->addWidget(parseButton);
  mainLayout->addLayout(formLayout);
  mainLayout->addLayout(buttonsLayout);
  mainLayout->addWidget(videoLabel_);
  mainLayout->addWidget(statusLabel_);

  setCentralWidget(centralWidget);
  resize(1100, 900);
  setWindowTitle(QStringLiteral("LiveKit Native Qt Player"));
}

void MainWindow::setButtonStates(bool isPlaying) {
  playButton_->setEnabled(!isPlaying);
  stopButton_->setEnabled(isPlaying);
}

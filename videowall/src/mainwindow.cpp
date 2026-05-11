#include "mainwindow.h"

#include "videowall.h"

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
  setCentralWidget(new VideoWall(this));
  resize(1280, 960);
  setWindowTitle(QStringLiteral("LiveKit Videowall"));
}

MainWindow::~MainWindow() = default;

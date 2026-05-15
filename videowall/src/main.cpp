#include "livekitplayer.h"
#include "mainwindow.h"

#include <QApplication>
#include <QByteArray>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QSurfaceFormat>

#include <cstdlib>

int main(int argc, char *argv[]) {
    QSurfaceFormat fmt;
    fmt.setVersion(3, 2);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    QSurfaceFormat::setDefaultFormat(fmt);

    QApplication app(argc, argv);

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("LiveKit videowall (multi-stream subscriber)"));
    parser.addHelpOption();
    QCommandLineOption widthOpt(
        QStringList{QStringLiteral("width"), QStringLiteral("W")},
        QStringLiteral("Requested video width (also: LIVEKIT_WIDTH)"),
        QStringLiteral("pixels"));
    QCommandLineOption heightOpt(
        QStringList{QStringLiteral("height"), QStringLiteral("H")},
        QStringLiteral("Requested video height (also: LIVEKIT_HEIGHT)"),
        QStringLiteral("pixels"));
    parser.addOption(widthOpt);
    parser.addOption(heightOpt);
    parser.process(app);

    int reqW = 0, reqH = 0;
    if (parser.isSet(widthOpt)) reqW = parser.value(widthOpt).toInt();
    if (parser.isSet(heightOpt)) reqH = parser.value(heightOpt).toInt();
    if (reqW <= 0) {
        if (const char *e = std::getenv("LIVEKIT_WIDTH")) reqW = std::atoi(e);
    }
    if (reqH <= 0) {
        if (const char *e = std::getenv("LIVEKIT_HEIGHT")) reqH = std::atoi(e);
    }
    LiveKitPlayer::setRequestedDimensions(reqW, reqH);

    MainWindow window;
    window.show();

    return app.exec();
}

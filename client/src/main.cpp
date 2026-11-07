#include <QFileInfo>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>

#include <clocale>

int main(int argc, char *argv[])
{
    // libmpv's render API is driven through OpenGL; must be chosen before any window exists.
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);

    QGuiApplication app(argc, argv);
    // Used for QStandardPaths::AppDataLocation (watch-later positions live there).
    QCoreApplication::setOrganizationName(QStringLiteral("CouchSync"));
    QCoreApplication::setApplicationName(QStringLiteral("CouchSync"));
    QQuickStyle::setStyle(QStringLiteral("Basic")); // restyled dark in QML (see Theme.qml)
    // Qt resets the locale on startup; libmpv requires the C numeric locale.
    std::setlocale(LC_NUMERIC, "C");

    QString startupFile;
    const QStringList args = app.arguments();
    if (args.size() > 1) {
        startupFile = QFileInfo(args.at(1)).absoluteFilePath();
    }

    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(-1); }, Qt::QueuedConnection);
    engine.setInitialProperties({{QStringLiteral("startupFile"), startupFile}});
    engine.loadFromModule("CouchSync", "Main");

    return app.exec();
}

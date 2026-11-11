#include <QFileInfo>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSslSocket>
#include <QTextStream>

#include <clocale>

int main(int argc, char *argv[])
{
    // libmpv's render API is driven through OpenGL; must be chosen before any window exists.
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);

    QGuiApplication app(argc, argv);
    // Used for QStandardPaths::AppDataLocation (watch-later positions live there).
    QCoreApplication::setOrganizationName(QStringLiteral("CouchSync"));
    QCoreApplication::setApplicationName(QStringLiteral("CouchSync"));
    QCoreApplication::setApplicationVersion(QStringLiteral(COUCHSYNC_VERSION_STR));

    // --check-tls: print the TLS backend and exit 0 if wss:// can work, 1 otherwise (release/support check).
    if (app.arguments().contains(QLatin1String("--check-tls"))) {
        const bool ok = QSslSocket::supportsSsl();
        QTextStream out(stdout);
        out << "CouchSync " << QCoreApplication::applicationVersion() << "\n"
            << "TLS active backend: " << QSslSocket::activeBackend() << "\n"
            << "TLS available backends: " << QSslSocket::availableBackends().join(QStringLiteral(", ")) << "\n"
            << "supportsSsl: " << (ok ? "true" : "false") << "\n"
            << "TLS library: " << QSslSocket::sslLibraryVersionString() << "\n";
        out.flush();
        return ok ? 0 : 1;
    }

    QQuickStyle::setStyle(QStringLiteral("Basic")); // restyled dark in QML (see Theme.qml)
    // Qt resets the locale on startup; libmpv requires the C numeric locale.
    std::setlocale(LC_NUMERIC, "C");

    // Command line: [file] [--server URL] [--create NAME | --join CODE NAME] [--password PW] [--log-sync] | --check-tls
    // (the room flags exist for test automation and dev convenience).
    QString startupFile;
    QString cliServer, cliCreateName, cliJoinCode, cliJoinName, cliPassword;
    bool logSync = false;
    const QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        const QString &a = args.at(i);
        auto next = [&]() { return i + 1 < args.size() ? args.at(++i) : QString(); };
        if (a == QLatin1String("--server"))
            cliServer = next();
        else if (a == QLatin1String("--create"))
            cliCreateName = next();
        else if (a == QLatin1String("--join")) {
            cliJoinCode = next();
            cliJoinName = next();
        } else if (a == QLatin1String("--password"))
            cliPassword = next();
        else if (a == QLatin1String("--log-sync"))
            logSync = true;
        else if (!a.startsWith(QLatin1String("--")) && startupFile.isEmpty())
            startupFile = QFileInfo(a).absoluteFilePath();
    }

    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(-1); }, Qt::QueuedConnection);
    engine.setInitialProperties({
        {QStringLiteral("startupFile"), startupFile},
        {QStringLiteral("cliServer"), cliServer},
        {QStringLiteral("cliCreateName"), cliCreateName},
        {QStringLiteral("cliJoinCode"), cliJoinCode},
        {QStringLiteral("cliJoinName"), cliJoinName},
        {QStringLiteral("cliPassword"), cliPassword},
        {QStringLiteral("logSync"), logSync},
    });
    engine.loadFromModule("CouchSync", "Main");

    return app.exec();
}

#include "listener/headless_listener.hpp"
#include "viewer/external_monitor_window.hpp"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>

#ifndef ECOSYSTEM_PROJECT_VERSION
#define ECOSYSTEM_PROJECT_VERSION "0.1.0"
#endif

namespace {

bool headless_mode_requested(int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        const QString argument = QString::fromLocal8Bit(argv[index]);
        if (argument == QStringLiteral("--listen")
            || argument == QStringLiteral("--headless")) {
            return true;
        }
    }
    return false;
}

QString endpoint_from_parser(const QCommandLineParser& parser) {
    QString endpoint = parser.value(QStringLiteral("endpoint")).trimmed();
    if (endpoint.isEmpty()) {
        endpoint = qEnvironmentVariable("MONITOR_ENDPOINT").trimmed();
    }
    if (endpoint.isEmpty()) {
        endpoint = qEnvironmentVariable("CPPR_DEBUG_ENDPOINT").trimmed();
    }
    return endpoint;
}

int run_monitor_window(QApplication& application) {
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Debug monitor for local application telemetry")
    );
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption(QCommandLineOption(
        QStringList() << QStringLiteral("e") << QStringLiteral("endpoint"),
        QStringLiteral(
            "Absolute local socket path or kcuckoounter endpoint name."
        ),
        QStringLiteral("path-or-name")
    ));
    parser.process(application);

    external_monitor_window window;
    window.resize(1100, 760);
    window.set_initial_endpoint(endpoint_from_parser(parser));
    window.show();
    return QApplication::exec();
}

} // namespace

int main(int argc, char** argv) {
    if (headless_mode_requested(argc, argv)) {
        QCoreApplication application(argc, argv);
        QCoreApplication::setApplicationName(QStringLiteral("monitor"));
        QCoreApplication::setApplicationVersion(
            QString::fromLatin1(ECOSYSTEM_PROJECT_VERSION)
        );
        return run_monitor_headless_listener(application);
    }

    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("monitor"));
    QCoreApplication::setApplicationVersion(
        QString::fromLatin1(ECOSYSTEM_PROJECT_VERSION)
    );
    return run_monitor_window(application);
}

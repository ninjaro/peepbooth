#include "listener/headless_listener.hpp"

#include <QCoreApplication>

#ifndef ECOSYSTEM_PROJECT_VERSION
#define ECOSYSTEM_PROJECT_VERSION "0.1.0"
#endif

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("monitor_listener"));
    QCoreApplication::setApplicationVersion(
        QString::fromLatin1(ECOSYSTEM_PROJECT_VERSION)
    );
    return run_monitor_headless_listener(application);
}

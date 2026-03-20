#include <QApplication>
#include <QtTest/QtTest>

#include "include/parity/parity_checker_tests.hpp"
#include "include/telemetry/debug_probe_core_tests.hpp"
#include "include/viewer/external_monitor_window_tests.hpp"
#include "include/viewer/visual_widgets_tests.hpp"

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    int status = 0;

    {
        debug_probe_core_tests t;
        status |= QTest::qExec(&t, argc, argv);
    }
    {
        monitor_visual_widgets_tests t;
        status |= QTest::qExec(&t, argc, argv);
    }
    {
        external_monitor_window_tests t;
        status |= QTest::qExec(&t, argc, argv);
    }
    {
        monitor_parity_checker_tests t;
        status |= QTest::qExec(&t, argc, argv);
    }

    return status;
}

#include "monitor/client.hpp"
#include "monitor/platform/shared_memory.hpp"
#include "monitor/qt/gui_heartbeat.hpp"
#include "monitor/watchdog.hpp"

#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <QMetaObject>
#include <QPointer>
#include <QTimer>

#include <cstdint>
#include <iostream>
#include <memory>

#if defined(__linux__) && !defined(__ANDROID__)
#include <sys/resource.h>
#endif

namespace {
int failures {};

void check(const bool value, const char* message) {
    if (!value) {
        ++failures;
        std::cerr << message << '\n';
    }
}

void check_inactive_adapter(QObject& application) {
    monitor::qt::gui_heartbeat adapter(&application, 1);
    const auto* timer = adapter.findChild<QTimer*>();
    check(
        timer != nullptr && !timer->isActive(),
        "unavailable producer has no active Qt timer"
    );
}

void check_lifecycle(QObject& application) {
    auto& producer = monitor::client::process();
    producer.stop();
    check_inactive_adapter(application);
    if constexpr (!monitor::instrumentation_enabled) {
        check(
            !monitor::start_process("qt-disabled"),
            "disabled startup remains unavailable"
        );
        check_inactive_adapter(application);
        return;
    }
    const auto identity = monitor::platform::read_process_identity(
        monitor::platform::current_process_id()
    );
    check(identity.has_value(), "actual Qt producer identity is available");
    if (!identity)
        return;

#if defined(__linux__) && !defined(__ANDROID__)
    // A real, process-local failure; no second producer or transport mock.
    // Restore the descriptor limit before Qt/event-loop operations resume.
    rlimit original {};
    const bool have_limit = ::getrlimit(RLIMIT_NOFILE, &original) == 0;
    check(have_limit, "read test process descriptor limit");
    if (!have_limit)
        return;
    auto unavailable = original;
    unavailable.rlim_cur = 0;
    const bool restricted = ::setrlimit(RLIMIT_NOFILE, &unavailable) == 0;
    check(restricted, "temporarily restrict only this test process");
    if (!restricted)
        return;
    const bool started = producer.start("must-fail");
    const bool restored = ::setrlimit(RLIMIT_NOFILE, &original) == 0;
    check(restored, "restore the test process descriptor limit");
    check(
        !started && !producer.available(),
        "failed startup leaves the client unavailable"
    );
    if (!restored)
        return;
    check_inactive_adapter(application);
#endif

    for (int cycle = 0; cycle < 3; ++cycle) {
        check(
            producer.start("qt-lifecycle"),
            "producer recovers after failure or shutdown"
        );
        auto watcher = monitor::watchdog_session::attach(*identity);
        auto retained = monitor::platform::open_process_mapping(identity->pid);
        check(
            watcher.has_value() && retained.has_value(),
            "actual observer retains the Qt producer"
        );
        if (!watcher || !retained)
            return;
        producer.set_channel_active(monitor::channel::worker, true);
        auto adapter
            = std::make_unique<monitor::qt::gui_heartbeat>(&application, 1);
        const QPointer<QTimer> timer = adapter->findChild<QTimer*>();
        check(
            timer != nullptr && timer->isActive(),
            "available producer starts its actual Qt timer"
        );
        if (!timer)
            return;
        {
            monitor::qt::gui_heartbeat duplicate(&application, 1);
            const auto* other_timer = duplicate.findChild<QTimer*>();
            check(
                other_timer != nullptr && !other_timer->isActive(),
                "a second GUI owner is rejected without starting another timer"
            );
        }
        monitor::heartbeat(monitor::channel::gui);
        check(
            retained->get()->heartbeats[1].active.load() == 1
                && retained->get()->heartbeats[1].sequence.load() == 0,
            "a rejected adapter cannot retire the owner or fabricate GUI "
            "progress"
        );
        QEventLoop loop;
        QTimer deadline;
        deadline.setSingleShot(true);
        QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
        int ticks = 0;
        QObject::connect(timer, &QTimer::timeout, &loop, [&] {
            ++ticks;
            loop.quit();
        });
        deadline.start(5000);
        loop.exec();
        check(
            ticks == 1,
            "actual timer delivers before teardown without scheduler sleeps"
        );
        check(
            retained->get()->heartbeats[1].active.load() == 1,
            "GUI channel is active while the adapter lives"
        );
        check(
            retained->get()->heartbeats[1].sequence.load() != 0,
            "the actual owner timer publishes GUI progress"
        );
        int late_calls = 0;
        check(
            QMetaObject::invokeMethod(
                adapter.get(),
                [&] {
                    ++late_calls;
                    monitor::heartbeat(monitor::channel::gui);
                },
                Qt::QueuedConnection
            ),
            "enqueue a context-bound callback before destruction"
        );

        // The real host sequence: window/workers are retired, then this GUI
        // adapter is destroyed, then the process client may unmap its endpoint.
        adapter.reset();
        check(timer.isNull(), "adapter destruction removes the actual timer");
        check(
            retained->get()->heartbeats[1].active.load() == 0,
            "teardown retires GUI publication"
        );
        check(
            retained->get()->heartbeats[3].active.load() == 1,
            "GUI teardown does not retire another producer channel"
        );
        producer.stop();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        check(
            late_calls == 0,
            "destroyed receiver cancels queued publication before client "
            "teardown"
        );
        check(
            watcher->snapshot().process_lifecycle
                == monitor::lifecycle::clean_exit,
            "observer retains the final clean lifecycle"
        );
        check(
            !monitor::platform::open_process_mapping(identity->pid),
            "only the producer removes its endpoint"
        );
    }
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    check_lifecycle(application);
    return failures == 0 ? 0 : 1;
}

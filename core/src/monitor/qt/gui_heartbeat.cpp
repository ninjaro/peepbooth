#include "monitor/qt/gui_heartbeat.hpp"

#include "monitor/client.hpp"

namespace monitor::qt {

gui_heartbeat::gui_heartbeat(QObject* parent, const int interval_ms)
    : QObject(parent)
    , timer_(this) {
    if (!process_available()) {
        return;
    }
    progress_ = expect_progress(channel::gui);
    if (!progress_.valid()) {
        return;
    }
    timer_.setInterval(interval_ms);
    timer_.setTimerType(Qt::CoarseTimer);
    connect(&timer_, &QTimer::timeout, this, [this] { progress_.heartbeat(); });
    timer_.start();
}

gui_heartbeat::~gui_heartbeat() {
    timer_.stop();
    progress_.reset();
}

} // namespace monitor::qt

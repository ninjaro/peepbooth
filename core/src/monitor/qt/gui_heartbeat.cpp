#include "monitor/qt/gui_heartbeat.hpp"

#include "monitor/client.hpp"

namespace monitor::qt {

gui_heartbeat::gui_heartbeat(QObject* parent, const int interval_ms)
    : QObject(parent)
    , timer_(this) {
    if (!process_available()) {
        return;
    }
    set_channel_active(channel::gui, true);
    timer_.setInterval(interval_ms);
    timer_.setTimerType(Qt::CoarseTimer);
    connect(&timer_, &QTimer::timeout, this, [] { heartbeat(channel::gui); });
    timer_.start();
}

gui_heartbeat::~gui_heartbeat() {
    if (timer_.isActive()) {
        set_channel_active(channel::gui, false);
    }
}

} // namespace monitor::qt

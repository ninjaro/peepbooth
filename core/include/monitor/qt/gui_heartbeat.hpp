#ifndef CPPR_MONITOR_QT_GUI_HEARTBEAT_HPP
#define CPPR_MONITOR_QT_GUI_HEARTBEAT_HPP

#include <QObject>
#include <QTimer>

namespace monitor {
namespace qt {

    class gui_heartbeat final : public QObject {
    public:
        explicit gui_heartbeat(
            QObject* parent = nullptr, int interval_ms = 500
        );
        ~gui_heartbeat() override;

        gui_heartbeat(const gui_heartbeat&) = delete;
        gui_heartbeat& operator=(const gui_heartbeat&) = delete;

    private:
        QTimer timer_;
    };

} // namespace qt
} // namespace monitor

#endif // CPPR_MONITOR_QT_GUI_HEARTBEAT_HPP

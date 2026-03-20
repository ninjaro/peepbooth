#pragma once

#include <QObject>

class external_monitor_window_tests : public QObject {
    Q_OBJECT

private slots:
    void generic_catalog_drives_primary_metric_selection();
    void generic_snapshot_and_event_batch_update_monitor_state();
};

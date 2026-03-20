#pragma once

#include <QObject>

class monitor_parity_checker_tests : public QObject {
    Q_OBJECT

private slots:
    void aligned_embedded_and_external_payloads_have_no_warnings();
    void drifting_payloads_surface_warnings();
    void generic_aligned_payloads_compare_on_overlapping_metrics_only();
    void legacy_and_generic_payloads_compare_only_shared_metric();
};

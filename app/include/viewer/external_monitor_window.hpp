#pragma once

#include "telemetry/debug_probe_core.hpp"
#include "viewer/visual_widgets.hpp"

#include <QHash>
#include <QJsonObject>
#include <QMainWindow>
#include <QSet>
#include <QString>
#include <QVector>

class QLineEdit;
class QLabel;
class QPushButton;
class QPlainTextEdit;
class QLocalSocket;
class QTimer;
class external_monitor_window_tests;

class external_monitor_window : public QMainWindow {
    Q_OBJECT

    friend class external_monitor_window_tests;

public:
    explicit external_monitor_window(QWidget* parent = nullptr);
    ~external_monitor_window() override;

    void set_initial_endpoint(const QString& endpoint_path);

private slots:
    void on_connect_clicked();
    void on_disconnect_clicked();
    void on_export_charts_clicked();
    void on_socket_connected();
    void on_socket_disconnected();
    void on_socket_ready_read();
    void on_socket_error();
    void on_connect_timeout();

private:
    using metric_point = debug_probe_core::metric_point_v1;

    struct marker_checkpoint {
        QString label;
        qint64 monotonic_timestamp_ms = 0;
        metric_point point;
    };

    QLineEdit* endpoint_input;
    QLabel* connection_status_label;
    QLabel* session_status_label;
    QLabel* leak_status_label;
    QLabel* log_path_label;
    QPushButton* connect_button;
    QPushButton* disconnect_button;
    QPushButton* export_charts_button;
    monitor_line_chart_widget* primary_memory_chart;
    monitor_line_chart_widget* leak_signal_chart;
    QPlainTextEdit* events_text;
    QPlainTextEdit* snapshot_text;
    QPlainTextEdit* warnings_text;
    QTimer* connect_timeout_timer;

    QLocalSocket* socket;
    QByteArray pending_read_buffer;
    QString current_endpoint_path;
    QString history_log_path;
    qint64 line_counter;
    qint64 warning_counter;
    qint64 marker_counter;
    qint64 snapshot_counter;
    bool monotonic_growth_suspicion;
    QString current_app_name;
    bool legacy_memory_view_active;
    metric_point latest_metric_point;
    metric_point high_water_point;
    metric_point settle_baseline_point;
    bool settle_baseline_valid;
    QHash<QString, QJsonObject> metric_hints_by_id;
    QVector<QString> metric_catalog_ids_in_order;
    QVector<QString> generic_primary_metric_ids;
    QHash<QString, qint64> latest_numeric_metrics_by_id;
    QHash<QString, QVector<double>> generic_series_by_id;
    QString generic_primary_display_unit;
    QSet<QString> reported_metric_hint_warnings;
    QVector<marker_checkpoint> marker_history;
    QVector<qint64> marker_cache_diffs;
    QVector<double> series_cache_mib;
    QVector<double> series_widget_mib;
    QVector<double> series_rss_mib;
    QVector<double> series_gap_mib;
    QVector<double> series_high_water_cache_mib;
    QVector<double> series_baseline_delta_mib;

    void connect_to_endpoint(const QString& endpoint_path);
    void disconnect_from_endpoint();
    void append_raw_line_to_history(const QByteArray& compact_json_line);
    void ensure_history_log_path(const QString& session_hint);
    void process_incoming_line(const QByteArray& compact_json_line);
    void handle_protocol_message(const QJsonObject& message);
    void handle_hello(const QJsonObject& message);
    void handle_capabilities(const QJsonObject& message);
    void handle_sample_batch(const QJsonObject& message);
    void handle_event_batch(const QJsonObject& message);
    void handle_snapshot(const QJsonObject& message);
    void handle_marker(const QJsonObject& message);
    void handle_warning(const QJsonObject& message);
    void update_primary_memory_chart();
    void update_leak_signal_chart();
    void update_status_labels();
    void rebuild_generic_metric_selection();
    bool has_legacy_memory_metrics() const;
    bool use_legacy_memory_view() const;
    QString metric_display_label(const QString& metric_id) const;
    QString primary_metric_labels_text() const;
    QString metric_unit(const QString& metric_id) const;
    QString chart_unit_label_for_metric_unit(const QString& unit) const;
    double chart_value_for_metric(const QString& metric_id, qint64 value) const;
    void append_generic_chart_sample();
    void merge_numeric_snapshot(const QJsonObject& snapshot);
    static bool is_numeric_json_value(const QJsonValue& value);
    static bool is_legacy_memory_metric_id(const QString& metric_id);
    void append_event_line(const QString& line);
    void append_warning_line(const QString& line);
    void append_parity_warning(const QString& code, const QString& details);
    static QString message_family(const QJsonObject& message);
    static QJsonObject protocol_identity(const QJsonObject& message);
    static qint64 integer_like_value(const QJsonValue& value);
};

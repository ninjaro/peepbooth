#include "viewer/external_monitor_window.hpp"

#include "telemetry/debug_probe_core.hpp"
#include "telemetry/shared_helpers.hpp"

#include <QDateTime>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>

namespace external_monitor_window_support {

constexpr int chart_history_limit = 1024;
constexpr int marker_diff_history_limit = 16;

const QVector<QColor>& generic_chart_palette() {
    static const QVector<QColor> colors {
        monitor_palette::blue(),   monitor_palette::orange(),
        monitor_palette::green(),  monitor_palette::red(),
        monitor_palette::purple(), monitor_palette::sky_blue(),
        monitor_palette::gray(),
    };
    return colors;
}

QString
connection_status_label_for_state(telemetry_session::connection_state state) {
    switch (state) {
    case telemetry_session::connection_state::connected:
        return QStringLiteral("Connected");
    case telemetry_session::connection_state::connecting:
        return QStringLiteral("Connecting");
    case telemetry_session::connection_state::disconnected:
    default:
        return QStringLiteral("Disconnected");
    }
}

QSize size_from_json(const QJsonValue& value) {
    const QJsonObject object = value.toObject();
    return { object.value(QStringLiteral("width")).toInt(),
             object.value(QStringLiteral("height")).toInt() };
}

geometry_debug_snapshot geometry_from_json(const QJsonObject& object) {
    return geometry_debug_snapshot {
        .timestamp_ms
        = object.value(QStringLiteral("timestamp_ms")).toInteger(),
        .slot_count = object.value(QStringLiteral("slot_count")).toInt(),
        .visible_slot_count
        = object.value(QStringLiteral("visible_slot_count")).toInt(),
        .window_size
        = size_from_json(object.value(QStringLiteral("window_size"))),
        .layout_size
        = size_from_json(object.value(QStringLiteral("layout_size"))),
        .display_card_size
        = size_from_json(object.value(QStringLiteral("display_card_size"))),
        .display_card_need_short_px
        = object.value(QStringLiteral("display_card_need_short_px")).toInt(),
        .device_pixel_ratio
        = object.value(QStringLiteral("device_pixel_ratio")).toDouble(1.0),
        .active_bucket_px
        = object.value(QStringLiteral("active_bucket_px")).toInt(),
        .warming_bucket_px
        = object.value(QStringLiteral("warming_bucket_px")).toInt(),
        .cache_window_minimum_need_px
        = object.value(QStringLiteral("cache_window_minimum_need_px")).toInt(),
        .cache_window_maximum_need_px
        = object.value(QStringLiteral("cache_window_maximum_need_px")).toInt(),
        .requested_target_bucket_px
        = object.value(QStringLiteral("requested_target_bucket_px")).toInt(),
        .cache_decision
        = object.value(QStringLiteral("cache_decision")).toString(),
        .cache_trigger
        = object.value(QStringLiteral("cache_trigger")).toString(),
        .cache_raster_size
        = size_from_json(object.value(QStringLiteral("cache_raster_size"))),
        .preloaded_raster_size
        = size_from_json(object.value(QStringLiteral("preloaded_raster_size"))),
        .coverage_percent
        = object.value(QStringLiteral("coverage_percent")).toInt(),
        .coverage_window_ms
        = object.value(QStringLiteral("coverage_window_ms")).toInteger(),
        .unique_size_buckets
        = object.value(QStringLiteral("unique_size_buckets")).toInt(),
        .prewarm_in_flight
        = object.value(QStringLiteral("prewarm_in_flight")).toBool(),
        .active_generation_id
        = object.value(QStringLiteral("active_generation_id")).toInteger(),
        .warming_generation_id
        = object.value(QStringLiteral("warming_generation_id")).toInteger(),
    };
}

void trim_series_to_limit(QVector<double>* series) {
    monitor_shared::trim_series(series, chart_history_limit);
}

} // namespace external_monitor_window_support

using external_monitor_window_support::connection_status_label_for_state;
using external_monitor_window_support::generic_chart_palette;
using external_monitor_window_support::geometry_from_json;
using external_monitor_window_support::marker_diff_history_limit;
using external_monitor_window_support::size_from_json;
using external_monitor_window_support::trim_series_to_limit;

external_monitor_window::external_monitor_window(QWidget* parent)
    : QMainWindow(parent)
    , endpoint_input(nullptr)
    , connection_status_label(nullptr)
    , session_status_label(nullptr)
    , leak_status_label(nullptr)
    , log_path_label(nullptr)
    , connect_button(nullptr)
    , disconnect_button(nullptr)
    , export_charts_button(nullptr)
    , primary_memory_chart(nullptr)
    , leak_signal_chart(nullptr)
    , cache_entry_chart(nullptr)
    , geometry_schematic(nullptr)
    , resize_history_chart(nullptr)
    , events_text(nullptr)
    , snapshot_text(nullptr)
    , warnings_text(nullptr)
    , render_timer(new QTimer(this))
    , session(new telemetry_session(this))
    , current_endpoint_path()
    , history_log_path()
    , line_counter(0)
    , warning_counter(0)
    , marker_counter(0)
    , snapshot_counter(0)
    , monotonic_growth_suspicion(false)
    , current_app_name()
    , legacy_memory_view_active(false)
    , latest_metric_point()
    , high_water_point()
    , settle_baseline_point()
    , settle_baseline_valid(false)
    , metric_hints_by_id()
    , metric_catalog_ids_in_order()
    , generic_primary_metric_ids()
    , latest_numeric_metrics_by_id()
    , generic_series_by_id()
    , generic_primary_display_unit()
    , reported_metric_hint_warnings()
    , marker_history()
    , marker_cache_diffs()
    , series_cache_mib()
    , series_widget_mib()
    , series_rss_mib()
    , series_gap_mib()
    , series_high_water_cache_mib()
    , series_baseline_delta_mib()
    , resize_entries() {
    setWindowTitle(QStringLiteral("Monitor"));

    auto* root = new QWidget(this);
    auto* root_layout = new QVBoxLayout(root);
    root_layout->setContentsMargins(8, 8, 8, 8);
    root_layout->setSpacing(8);

    auto* controls_row = new QHBoxLayout;
    controls_row->setSpacing(6);

    auto* endpoint_label = new QLabel(QStringLiteral("Endpoint:"), root);
    endpoint_input = new QLineEdit(root);
    endpoint_input->setPlaceholderText(QStringLiteral("/tmp/monitor_...sock"));
    endpoint_input->setAccessibleName(QStringLiteral("Telemetry endpoint"));
    endpoint_label->setBuddy(endpoint_input);
    connect_button = new QPushButton(QStringLiteral("Connect"), root);
    disconnect_button = new QPushButton(QStringLiteral("Disconnect"), root);
    export_charts_button
        = new QPushButton(QStringLiteral("Save chart image"), root);
    connect_button->setAccessibleDescription(
        QStringLiteral("Connect to the configured local telemetry endpoint")
    );
    disconnect_button->setAccessibleDescription(
        QStringLiteral("Disconnect from the current telemetry session")
    );
    export_charts_button->setAccessibleDescription(
        QStringLiteral("Save the two current time-series charts as a PNG")
    );
    disconnect_button->setEnabled(false);

    controls_row->addWidget(endpoint_label);
    controls_row->addWidget(endpoint_input, 1);
    controls_row->addWidget(connect_button);
    controls_row->addWidget(disconnect_button);
    controls_row->addWidget(export_charts_button);
    root_layout->addLayout(controls_row);

    auto* status_row = new QHBoxLayout;
    status_row->setSpacing(10);
    connection_status_label = new QLabel(root);
    session_status_label = new QLabel(root);
    leak_status_label = new QLabel(root);
    log_path_label = new QLabel(root);
    status_row->addWidget(connection_status_label, 1);
    status_row->addWidget(session_status_label, 2);
    status_row->addWidget(leak_status_label, 2);
    root_layout->addLayout(status_row);
    root_layout->addWidget(log_path_label);

    auto* semantics_glossary = new QLabel(
        QStringLiteral(
            "Glossary: measured = OS source, accounted = owned object bytes, "
            "estimated = display heuristic, derived = diagnostics."
        ),
        root
    );
    semantics_glossary->setWordWrap(true);
    semantics_glossary->setToolTip(QStringLiteral(
        "Derived lines are leak-analysis aids, not direct ownership classes."
    ));
    root_layout->addWidget(semantics_glossary);
    auto* series_legend = new QLabel(
        QStringLiteral(
            "Color legend: "
            "<span style='color:#0072B2'>■</span> cache-accounted, "
            "<span style='color:#E69F00'>■</span> widget-local estimated, "
            "<span style='color:#009E73'>■</span> process RSS measured, "
            "<span style='color:#D55E00'>■</span> measured-accounted gap "
            "derived, "
            "<span style='color:#CC79A7'>■</span> high-water/baseline derived."
        ),
        root
    );
    series_legend->setWordWrap(true);
    series_legend->setToolTip(QStringLiteral(
        "Legend remains visible even when historical series are decimated."
    ));
    root_layout->addWidget(series_legend);

    auto* tabs = new QTabWidget(root);
    auto* dashboard_tab = new QWidget(tabs);
    auto* dashboard_layout = new QVBoxLayout(dashboard_tab);
    dashboard_layout->setContentsMargins(4, 4, 4, 4);
    dashboard_layout->setSpacing(8);

    primary_memory_chart = new monitor_line_chart_widget(dashboard_tab);
    primary_memory_chart->set_title(QStringLiteral(
        "Primary memory lines (measured/accounted/estimated/derived)"
    ));
    primary_memory_chart->set_unit_label(QStringLiteral("MiB"));
    primary_memory_chart->set_x_axis_label(
        QStringLiteral("sample index (oldest -> newest)")
    );
    primary_memory_chart->setToolTip(
        QStringLiteral("Hover a line point for value and sample index details.")
    );
    primary_memory_chart->setAccessibleName(
        QStringLiteral("Primary telemetry time series")
    );
    primary_memory_chart->setAccessibleDescription(QStringLiteral(
        "Measured process memory, accounted cache memory, estimated display "
        "memory, and derived comparison values."
    ));
    dashboard_layout->addWidget(primary_memory_chart, 3);

    leak_signal_chart = new monitor_line_chart_widget(dashboard_tab);
    leak_signal_chart->set_title(QStringLiteral(
        "Leak-oriented derived lines (high-water / baseline delta)"
    ));
    leak_signal_chart->set_unit_label(QStringLiteral("MiB"));
    leak_signal_chart->set_x_axis_label(
        QStringLiteral("sample index (oldest -> newest)")
    );
    leak_signal_chart->setToolTip(QStringLiteral(
        "Derived leak diagnostics across markers and settle baselines."
    ));
    leak_signal_chart->setAccessibleName(
        QStringLiteral("Leak-oriented diagnostic time series")
    );
    leak_signal_chart->setAccessibleDescription(QStringLiteral(
        "Derived cache high-water and post-settle baseline values."
    ));
    dashboard_layout->addWidget(leak_signal_chart, 2);
    tabs->addTab(dashboard_tab, QStringLiteral("Dashboard"));

    auto* cache_tab = new QWidget(tabs);
    auto* cache_layout = new QVBoxLayout(cache_tab);
    cache_layout->setContentsMargins(4, 4, 4, 4);
    cache_layout->setSpacing(8);
    auto* cache_top_splitter = new QSplitter(Qt::Horizontal, cache_tab);
    cache_entry_chart = new monitor_pie_chart_widget(cache_top_splitter);
    cache_entry_chart->set_title(
        QStringLiteral("Ready cache entry composition")
    );
    cache_entry_chart->setAccessibleName(
        QStringLiteral("Displayed and cached-only ready entries")
    );
    geometry_schematic
        = new monitor_geometry_schematic_widget(cache_top_splitter);
    geometry_schematic->setAccessibleName(
        QStringLiteral("Card display and SVG cache geometry")
    );
    cache_top_splitter->addWidget(cache_entry_chart);
    cache_top_splitter->addWidget(geometry_schematic);
    cache_layout->addWidget(cache_top_splitter, 3);
    resize_history_chart = new monitor_resize_history_widget(cache_tab);
    resize_history_chart->setAccessibleName(
        QStringLiteral("Window and cache resize history")
    );
    cache_layout->addWidget(resize_history_chart, 2);
    tabs->addTab(cache_tab, QStringLiteral("SVG cache"));

    auto* events_tab = new QWidget(tabs);
    auto* events_layout = new QVBoxLayout(events_tab);
    events_layout->setContentsMargins(4, 4, 4, 4);
    events_layout->setSpacing(6);
    events_text = new QPlainTextEdit(events_tab);
    events_text->setReadOnly(true);
    events_text->setLineWrapMode(QPlainTextEdit::NoWrap);
    events_text->setMaximumBlockCount(2048);
    events_text->setAccessibleName(QStringLiteral("Telemetry event log"));
    events_layout->addWidget(events_text);
    tabs->addTab(events_tab, QStringLiteral("Events"));

    auto* snapshot_tab = new QWidget(tabs);
    auto* snapshot_layout = new QVBoxLayout(snapshot_tab);
    snapshot_layout->setContentsMargins(4, 4, 4, 4);
    snapshot_layout->setSpacing(6);
    snapshot_text = new QPlainTextEdit(snapshot_tab);
    snapshot_text->setReadOnly(true);
    snapshot_text->setLineWrapMode(QPlainTextEdit::NoWrap);
    snapshot_text->setAccessibleName(
        QStringLiteral("Latest telemetry snapshot JSON")
    );
    snapshot_layout->addWidget(snapshot_text);
    tabs->addTab(snapshot_tab, QStringLiteral("Snapshot"));

    auto* warnings_tab = new QWidget(tabs);
    auto* warnings_layout = new QVBoxLayout(warnings_tab);
    warnings_layout->setContentsMargins(4, 4, 4, 4);
    warnings_layout->setSpacing(6);
    warnings_text = new QPlainTextEdit(warnings_tab);
    warnings_text->setReadOnly(true);
    warnings_text->setLineWrapMode(QPlainTextEdit::NoWrap);
    warnings_text->setMaximumBlockCount(2048);
    warnings_text->setAccessibleName(QStringLiteral("Telemetry warnings"));
    warnings_layout->addWidget(warnings_text);
    tabs->addTab(warnings_tab, QStringLiteral("Warnings"));

    root_layout->addWidget(tabs, 1);
    setCentralWidget(root);

    QObject::connect(
        connect_button, &QPushButton::clicked, this,
        &external_monitor_window::on_connect_clicked
    );
    QObject::connect(
        disconnect_button, &QPushButton::clicked, this,
        &external_monitor_window::on_disconnect_clicked
    );
    QObject::connect(
        export_charts_button, &QPushButton::clicked, this,
        &external_monitor_window::on_export_charts_clicked
    );
    QObject::connect(
        session, &telemetry_session::connected, this,
        &external_monitor_window::on_session_connected
    );
    QObject::connect(
        session, &telemetry_session::disconnected, this,
        &external_monitor_window::on_session_disconnected
    );
    QObject::connect(
        session, &telemetry_session::connection_failed, this,
        &external_monitor_window::on_session_connection_failed
    );
    QObject::connect(
        session, &telemetry_session::protocol_error, this,
        &external_monitor_window::on_session_protocol_error
    );
    QObject::connect(
        session, &telemetry_session::history_path_changed, this,
        &external_monitor_window::on_history_path_changed
    );
    QObject::connect(
        session, &telemetry_session::history_error, this,
        [this](const QString& message) {
            append_warning_line(
                QStringLiteral("[%1] history error: %2")
                    .arg(monitor_shared::utc_now_text(), message)
            );
        }
    );
    QObject::connect(
        session, &telemetry_session::history_line_dropped, this,
        [this](qint64 dropped) {
            append_warning_line(
                QStringLiteral("[%1] history queue dropped %2 record(s)")
                    .arg(monitor_shared::utc_now_text())
                    .arg(dropped)
            );
        }
    );
    QObject::connect(
        session, &telemetry_session::session_reset, this,
        &external_monitor_window::on_session_reset
    );
    QObject::connect(
        session, &telemetry_session::protocol_message_received, this,
        &external_monitor_window::handle_protocol_message
    );
    QObject::connect(
        session, &telemetry_session::geometry_received, this,
        &external_monitor_window::handle_geometry
    );
    QObject::connect(
        session, &telemetry_session::layout_transition_received, this,
        &external_monitor_window::handle_layout_transition
    );
    QObject::connect(
        session, &telemetry_session::cache_decision_received, this,
        &external_monitor_window::handle_cache_decision
    );

    render_timer->setSingleShot(true);
    render_timer->setInterval(75);
    QObject::connect(
        render_timer, &QTimer::timeout, this,
        &external_monitor_window::on_render_timeout
    );
    QWidget::setTabOrder(endpoint_input, connect_button);
    QWidget::setTabOrder(connect_button, disconnect_button);
    QWidget::setTabOrder(disconnect_button, export_charts_button);

    update_status_labels();
    update_primary_memory_chart();
    update_leak_signal_chart();
    update_cache_entry_chart();
}

external_monitor_window::~external_monitor_window() {
    disconnect_from_endpoint();
}

void external_monitor_window::set_initial_endpoint(
    const QString& endpoint_path
) {
    if (endpoint_input != nullptr) {
        endpoint_input->setText(endpoint_path);
    }
    if (!endpoint_path.trimmed().isEmpty()) {
        connect_to_endpoint(endpoint_path.trimmed());
    }
}

void external_monitor_window::on_connect_clicked() {
    if (endpoint_input == nullptr) {
        return;
    }
    connect_to_endpoint(endpoint_input->text().trimmed());
}

void external_monitor_window::on_disconnect_clicked() {
    disconnect_from_endpoint();
}

void external_monitor_window::on_export_charts_clicked() {
    if (primary_memory_chart == nullptr || leak_signal_chart == nullptr) {
        return;
    }

    const QString timestamp = QDateTime::currentDateTimeUtc().toString(
        QStringLiteral("yyyyMMdd_hhmmss")
    );
    QString output_path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save chart image"),
        QStringLiteral("monitor_charts_%1.png").arg(timestamp),
        QStringLiteral("PNG files (*.png)")
    );
    if (output_path.isEmpty()) {
        return;
    }
    if (QFileInfo(output_path).suffix().isEmpty()) {
        output_path += QStringLiteral(".png");
    }

    const QVector<QPixmap> charts = {
        primary_memory_chart->grab(),
        leak_signal_chart->grab(),
    };
    for (const QPixmap& chart : charts) {
        if (chart.isNull()) {
            QMessageBox::warning(
                this, QStringLiteral("Unable to save charts"),
                QStringLiteral("A chart could not be captured.")
            );
            return;
        }
    }

    constexpr int spacing_px = 10;
    int width_px = 0;
    int height_px = 0;
    for (qsizetype index = 0; index < charts.size(); ++index) {
        width_px = std::max(width_px, charts.at(index).width());
        height_px += charts.at(index).height();
        if (index + 1 < charts.size()) {
            height_px += spacing_px;
        }
    }

    if (width_px <= 0 || height_px <= 0) {
        QMessageBox::warning(
            this, QStringLiteral("Unable to save charts"),
            QStringLiteral("The composed chart image has an invalid size.")
        );
        return;
    }

    QPixmap composed(width_px, height_px);
    composed.fill(primary_memory_chart->palette().color(QPalette::Base));

    QPainter painter(&composed);
    int y_offset = 0;
    for (qsizetype index = 0; index < charts.size(); ++index) {
        const QPixmap& chart = charts.at(index);
        painter.drawPixmap(0, y_offset, chart);
        y_offset += chart.height();
        if (index + 1 < charts.size()) {
            y_offset += spacing_px;
        }
    }
    painter.end();

    QSaveFile output_file(output_path);
    if (!output_file.open(QIODevice::WriteOnly)
        || !composed.save(&output_file, "PNG") || !output_file.commit()) {
        QMessageBox::warning(
            this, QStringLiteral("Unable to save charts"),
            QStringLiteral("Could not write '%1': %2")
                .arg(output_path, output_file.errorString())
        );
        return;
    }
    statusBar()->showMessage(
        QStringLiteral("Saved chart image to %1").arg(output_path), 5000
    );
}

void external_monitor_window::on_session_connected(
    const QString& endpoint_path
) {
    current_endpoint_path = endpoint_path;
    append_event_line(QStringLiteral("[%1] connected to %2")
                          .arg(monitor_shared::utc_now_text(), endpoint_path));
    update_status_labels();
}

void external_monitor_window::on_session_disconnected() {
    current_endpoint_path.clear();
    append_event_line(
        QStringLiteral("[%1] disconnected").arg(monitor_shared::utc_now_text())
    );
    update_status_labels();
}

void external_monitor_window::on_session_connection_failed(
    const QString& message
) {
    append_warning_line(QStringLiteral("[%1] connection failed: %2")
                            .arg(monitor_shared::utc_now_text(), message));
}

void external_monitor_window::on_session_protocol_error(
    const QString& code, const QString& message
) {
    QTimer::singleShot(0, this, [this, code, message]() {
        append_warning_line(
            QStringLiteral("[%1] protocol error %2: %3")
                .arg(monitor_shared::utc_now_text(), code, message)
        );
    });
}

void external_monitor_window::on_history_path_changed(const QString& path) {
    history_log_path = path;
    update_status_labels();
}

void external_monitor_window::on_session_reset() { reset_session_state(); }

void external_monitor_window::on_render_timeout() {
    update_primary_memory_chart();
    update_leak_signal_chart();
    update_cache_entry_chart();
}

void external_monitor_window::connect_to_endpoint(
    const QString& endpoint_path
) {
    if (endpoint_path.isEmpty()) {
        append_warning_line(QStringLiteral("[%1] endpoint is empty")
                                .arg(monitor_shared::utc_now_text()));
        return;
    }

    current_endpoint_path = endpoint_path;
    if (endpoint_input != nullptr) {
        endpoint_input->setText(endpoint_path);
    }
    session->connect_to_endpoint(endpoint_path);
    update_status_labels();
}

void external_monitor_window::disconnect_from_endpoint() {
    session->disconnect_from_endpoint();
    current_endpoint_path.clear();
    update_status_labels();
}

void external_monitor_window::reset_session_state() {
    if (render_timer != nullptr) {
        render_timer->stop();
    }
    history_log_path.clear();
    line_counter = 0;
    warning_counter = 0;
    marker_counter = 0;
    snapshot_counter = 0;
    monotonic_growth_suspicion = false;
    current_app_name.clear();
    legacy_memory_view_active = false;
    latest_metric_point = metric_point();
    high_water_point = metric_point();
    settle_baseline_point = metric_point();
    settle_baseline_valid = false;
    metric_hints_by_id.clear();
    metric_catalog_ids_in_order.clear();
    generic_primary_metric_ids.clear();
    latest_numeric_metrics_by_id.clear();
    generic_series_by_id.clear();
    generic_primary_display_unit.clear();
    reported_metric_hint_warnings.clear();
    marker_history.clear();
    marker_cache_diffs.clear();
    series_cache_mib.clear();
    series_widget_mib.clear();
    series_rss_mib.clear();
    series_gap_mib.clear();
    series_high_water_cache_mib.clear();
    series_baseline_delta_mib.clear();
    resize_entries.clear();
    if (events_text != nullptr) {
        events_text->clear();
    }
    if (snapshot_text != nullptr) {
        snapshot_text->clear();
    }
    if (warnings_text != nullptr) {
        warnings_text->clear();
    }
    if (geometry_schematic != nullptr) {
        geometry_schematic->clear_snapshot();
    }
    if (resize_history_chart != nullptr) {
        resize_history_chart->set_entries(resize_entries);
    }
    update_status_labels();
    update_primary_memory_chart();
    update_leak_signal_chart();
    update_cache_entry_chart();
}

void external_monitor_window::schedule_chart_update() {
    if (render_timer != nullptr && !render_timer->isActive()) {
        render_timer->start();
    }
}

void external_monitor_window::handle_protocol_message(
    const QJsonObject& message
) {
    ++line_counter;
    const QString family = message_family(message);
    if (family.isEmpty()) {
        append_warning_line(
            QStringLiteral("[%1] message missing protocol family")
                .arg(monitor_shared::utc_now_text())
        );
        return;
    }

    if (family == QStringLiteral("hello")) {
        handle_hello(message);
    } else if (family == QStringLiteral("capabilities")) {
        handle_capabilities(message);
    } else if (family == QStringLiteral("sample_batch")) {
        handle_sample_batch(message);
    } else if (family == QStringLiteral("event_batch")) {
        handle_event_batch(message);
    } else if (family == QStringLiteral("snapshot")) {
        handle_snapshot(message);
    } else if (family == QStringLiteral("marker")) {
        handle_marker(message);
    } else if (family == QStringLiteral("warning")) {
        handle_warning(message);
    } else if (family == QStringLiteral("goodbye")) {
        append_event_line(QStringLiteral("[%1] goodbye message")
                              .arg(monitor_shared::utc_now_text()));
    }

    update_status_labels();
    schedule_chart_update();
}

void external_monitor_window::handle_hello(const QJsonObject& message) {
    const QJsonObject identity = protocol_identity(message);
    current_app_name = identity.value(QStringLiteral("app")).toString();
    append_event_line(
        QStringLiteral("[%1] hello app=%2 pid=%3 session=%4 build=%5 mode=%6")
            .arg(
                monitor_shared::utc_now_text(),
                identity.value(QStringLiteral("app")).toString(),
                QString::number(
                    identity.value(QStringLiteral("pid")).toInteger()
                ),
                identity.value(QStringLiteral("session")).toString(),
                identity.value(QStringLiteral("build")).toString(),
                identity.value(QStringLiteral("instrumentation_mode"))
                    .toString()
            )
    );
}

void external_monitor_window::handle_capabilities(const QJsonObject& message) {
    current_app_name
        = protocol_identity(message).value(QStringLiteral("app")).toString();
    const QJsonObject capabilities
        = message.value(QStringLiteral("capabilities")).toObject();
    const QJsonArray catalog
        = capabilities.value(QStringLiteral("metric_catalog")).toArray();
    if (catalog.isEmpty()) {
        append_parity_warning(
            QStringLiteral("capabilities_missing_metric_catalog"),
            QStringLiteral("metric catalog is empty")
        );
        return;
    }

    metric_hints_by_id.clear();
    metric_catalog_ids_in_order.clear();
    for (const auto& value : catalog) {
        const QJsonObject metric = value.toObject();
        const QString id = metric.value(QStringLiteral("id")).toString();
        if (!id.isEmpty()) {
            metric_hints_by_id.insert(id, metric);
            metric_catalog_ids_in_order.push_back(id);
        }
    }
    legacy_memory_view_active = has_legacy_memory_metrics();
    rebuild_generic_metric_selection();

    const QJsonArray expected_catalog
        = debug_probe_core::protocol_metric_catalog_v1();
    for (const auto& value : expected_catalog) {
        const QJsonObject expected = value.toObject();
        const QString id = expected.value(QStringLiteral("id")).toString();
        if (id.isEmpty()) {
            continue;
        }
        if (!metric_hints_by_id.contains(id)) {
            continue;
        }

        const QJsonObject incoming = metric_hints_by_id.value(id);
        const QJsonArray required_fields
            = debug_probe_core::required_metric_hint_fields_v1();
        for (const auto& field_value : required_fields) {
            const QString field = field_value.toString();
            if (field.isEmpty()) {
                continue;
            }
            if (incoming.value(field).toString()
                != expected.value(field).toString()) {
                append_parity_warning(
                    QStringLiteral("metric_hint_drift_%1_%2").arg(id, field),
                    QStringLiteral(
                        "metric '%1' hint '%2' drift incoming='%3' "
                        "expected='%4'"
                    )
                        .arg(
                            id, field, incoming.value(field).toString(),
                            expected.value(field).toString()
                        )
                );
            }
        }
    }

    schedule_chart_update();
}

void external_monitor_window::handle_sample_batch(const QJsonObject& message) {
    const QJsonArray samples
        = message.value(QStringLiteral("samples")).toArray();
    if (samples.isEmpty()) {
        return;
    }

    for (const auto& sample_value : samples) {
        const QJsonObject sample = sample_value.toObject();
        const QString metric_id
            = sample.value(QStringLiteral("metric_id")).toString();
        if (metric_id.isEmpty()) {
            continue;
        }

        QJsonObject metric_hint
            = sample.value(QStringLiteral("metric_hint")).toObject();
        if (metric_hint.isEmpty()) {
            metric_hint = metric_hints_by_id.value(metric_id);
        }
        if (metric_hint.isEmpty()) {
            const QString warning_key
                = QStringLiteral("missing_hint_%1").arg(metric_id);
            if (!reported_metric_hint_warnings.contains(warning_key)) {
                reported_metric_hint_warnings.insert(warning_key);
                append_parity_warning(
                    QStringLiteral("sample_metric_hint_missing_%1")
                        .arg(metric_id),
                    QStringLiteral(
                        "sample metric '%1' is missing required hint metadata"
                    )
                        .arg(metric_id)
                );
            }
            continue;
        }

        const QJsonArray required_fields
            = debug_probe_core::required_metric_hint_fields_v1();
        for (const auto& field_value : required_fields) {
            const QString field = field_value.toString();
            if (field.isEmpty()) {
                continue;
            }
            if (!metric_hint.contains(field)
                || metric_hint.value(field).toString().isEmpty()) {
                const QString warning_key
                    = QStringLiteral("hint_field_%1_%2").arg(metric_id, field);
                if (reported_metric_hint_warnings.contains(warning_key)) {
                    continue;
                }
                reported_metric_hint_warnings.insert(warning_key);
                append_parity_warning(
                    QStringLiteral("sample_metric_hint_field_missing")
                        + QStringLiteral("_%1_%2").arg(metric_id, field),
                    QStringLiteral(
                        "sample metric '%1' hint field '%2' is missing"
                    )
                        .arg(metric_id, field)
                );
            }
        }

        const QJsonValue value = sample.value(QStringLiteral("value"));
        if (is_numeric_json_value(value)) {
            latest_numeric_metrics_by_id.insert(metric_id, value.toDouble());
        }
    }

    const metric_point sample_point
        = debug_probe_core::point_from_sample_batch_v1(samples);
    debug_probe_core::merge_metric_point_v1(&latest_metric_point, sample_point);

    if (sample_point.cache_accounted_ready_bytes >= 0) {
        high_water_point.cache_accounted_ready_bytes = std::max(
            high_water_point.cache_accounted_ready_bytes,
            sample_point.cache_accounted_ready_bytes
        );
    }
    if (sample_point.widget_local_display_bytes_estimated >= 0) {
        high_water_point.widget_local_display_bytes_estimated = std::max(
            high_water_point.widget_local_display_bytes_estimated,
            sample_point.widget_local_display_bytes_estimated
        );
    }
    if (sample_point.process_memory_rss_bytes >= 0) {
        high_water_point.process_memory_rss_bytes = std::max(
            high_water_point.process_memory_rss_bytes,
            sample_point.process_memory_rss_bytes
        );
    }
    if (sample_point.measured_accounted_gap_bytes_derived >= 0) {
        high_water_point.measured_accounted_gap_bytes_derived = std::max(
            high_water_point.measured_accounted_gap_bytes_derived,
            sample_point.measured_accounted_gap_bytes_derived
        );
    }

    series_cache_mib.push_back(
        monitor_shared::to_mib_nullable(
            latest_metric_point.cache_accounted_ready_bytes
        )
    );
    series_widget_mib.push_back(
        monitor_shared::to_mib_nullable(
            latest_metric_point.widget_local_display_bytes_estimated
        )
    );
    series_rss_mib.push_back(
        monitor_shared::to_mib_nullable(
            latest_metric_point.process_memory_rss_bytes
        )
    );
    series_gap_mib.push_back(
        monitor_shared::to_mib_nullable(
            latest_metric_point.measured_accounted_gap_bytes_derived
        )
    );
    series_high_water_cache_mib.push_back(
        monitor_shared::to_mib_nullable(
            high_water_point.cache_accounted_ready_bytes
        )
    );
    series_baseline_delta_mib.push_back(
        settle_baseline_valid
            ? monitor_shared::to_mib(
                  latest_metric_point.cache_accounted_ready_bytes
                  - settle_baseline_point.cache_accounted_ready_bytes
              )
            : std::numeric_limits<double>::quiet_NaN()
    );

    trim_series_to_limit(&series_cache_mib);
    trim_series_to_limit(&series_widget_mib);
    trim_series_to_limit(&series_rss_mib);
    trim_series_to_limit(&series_gap_mib);
    trim_series_to_limit(&series_high_water_cache_mib);
    trim_series_to_limit(&series_baseline_delta_mib);

    append_generic_chart_sample();

    schedule_chart_update();
}

void external_monitor_window::handle_event_batch(const QJsonObject& message) {
    const QJsonArray events = message.value(QStringLiteral("events")).toArray();
    for (const auto& event_value : events) {
        const QJsonObject event = event_value.toObject();
        QStringList details;
        const QString kind = event.value(QStringLiteral("kind")).toString();
        if (!kind.isEmpty()) {
            details.push_back(QStringLiteral("kind=%1").arg(kind));
        }
        const QString stream_name
            = event.value(QStringLiteral("stream_name")).toString();
        if (!stream_name.isEmpty()) {
            details.push_back(QStringLiteral("stream=%1").arg(stream_name));
        }
        const QString line_name
            = event.value(QStringLiteral("line_name")).toString();
        if (!line_name.isEmpty()) {
            details.push_back(QStringLiteral("line=%1").arg(line_name));
        }
        const QString event_message
            = event.value(QStringLiteral("message")).toString();
        if (!event_message.isEmpty()) {
            details.push_back(QStringLiteral("message=%1").arg(event_message));
        }
        const qint64 timestamp_ms
            = event.value(QStringLiteral("timestamp_ms")).toInteger();
        const qint64 collector_sequence
            = event.value(QStringLiteral("collector_sequence")).toInteger();
        append_event_line(QStringLiteral("[%1] event %2 t=%3 seq=%4")
                              .arg(
                                  monitor_shared::utc_now_text(),
                                  details.join(QLatin1Char(' ')),
                                  QString::number(timestamp_ms),
                                  QString::number(collector_sequence)
                              ));
    }
}

void external_monitor_window::handle_snapshot(const QJsonObject& message) {
    ++snapshot_counter;
    const QJsonObject snapshot
        = message.value(QStringLiteral("snapshot")).toObject();
    const qint64 previous_live_cache
        = latest_metric_point.cache_accounted_ready_bytes;
    if (!snapshot.isEmpty()) {
        merge_numeric_snapshot(snapshot);
        const metric_point snapshot_point
            = debug_probe_core::point_from_snapshot_payload_v1(snapshot);
        debug_probe_core::merge_metric_point_v1(
            &latest_metric_point, snapshot_point
        );
        if (snapshot_point.cache_accounted_ready_bytes >= 0) {
            high_water_point.cache_accounted_ready_bytes = std::max(
                high_water_point.cache_accounted_ready_bytes,
                snapshot_point.cache_accounted_ready_bytes
            );
        }
        if (snapshot_point.widget_local_display_bytes_estimated >= 0) {
            high_water_point.widget_local_display_bytes_estimated = std::max(
                high_water_point.widget_local_display_bytes_estimated,
                snapshot_point.widget_local_display_bytes_estimated
            );
        }
        if (snapshot_point.process_memory_rss_bytes >= 0) {
            high_water_point.process_memory_rss_bytes = std::max(
                high_water_point.process_memory_rss_bytes,
                snapshot_point.process_memory_rss_bytes
            );
        }
        if (snapshot_point.measured_accounted_gap_bytes_derived >= 0) {
            high_water_point.measured_accounted_gap_bytes_derived = std::max(
                high_water_point.measured_accounted_gap_bytes_derived,
                snapshot_point.measured_accounted_gap_bytes_derived
            );
        }
    }

    if (snapshot_text != nullptr) {
        const QJsonDocument document(snapshot);
        snapshot_text->setPlainText(
            QString::fromUtf8(document.toJson(QJsonDocument::Indented))
        );
    }

    if (snapshot.contains(QStringLiteral("cache_accounted_ready_bytes"))
        && previous_live_cache >= 0) {
        const qint64 snapshot_cache = integer_like_value(
            snapshot.value(QStringLiteral("cache_accounted_ready_bytes"))
        );
        const qint64 live_cache = previous_live_cache;
        if (std::llabs(snapshot_cache - live_cache) > (8 * 1024 * 1024)) {
            append_parity_warning(
                QStringLiteral("snapshot_sample_cache_drift"),
                QStringLiteral(
                    "snapshot cache bytes (%1) and sample cache bytes (%2) "
                    "drift"
                )
                    .arg(snapshot_cache)
                    .arg(live_cache)
            );
        }
    }

    update_status_labels();
}

void external_monitor_window::handle_marker(const QJsonObject& message) {
    ++marker_counter;
    const QString label = message.value(QStringLiteral("label")).toString();
    const qint64 monotonic_timestamp_ms = integer_like_value(
        message.value(QStringLiteral("monotonic_timestamp_ms"))
    );

    append_event_line(QStringLiteral("[%1] marker '%2' t=%3")
                          .arg(monitor_shared::utc_now_text(), label)
                          .arg(monotonic_timestamp_ms));

    const marker_checkpoint checkpoint {
        .label = label,
        .monotonic_timestamp_ms = monotonic_timestamp_ms,
        .point = latest_metric_point,
    };

    if (!marker_history.isEmpty()) {
        const marker_checkpoint previous = marker_history.constLast();
        if (previous.point.cache_accounted_ready_bytes >= 0
            && checkpoint.point.cache_accounted_ready_bytes >= 0) {
            const qint64 diff_cache
                = checkpoint.point.cache_accounted_ready_bytes
                - previous.point.cache_accounted_ready_bytes;
            marker_cache_diffs.push_back(diff_cache);
            while (marker_cache_diffs.size() > marker_diff_history_limit) {
                marker_cache_diffs.removeFirst();
            }

            append_event_line(
                QStringLiteral(
                    "  marker-to-marker diff: cache(accounted)=%1 MiB"
                )
                    .arg(
                        QString::number(
                            monitor_shared::to_mib(diff_cache), 'f', 2
                        )
                    )
            );
        }
    }

    const QString lowered_label = label.toLower();
    if (lowered_label.contains(QStringLiteral("settle"))
        || lowered_label.contains(QStringLiteral("quiescent"))
        || lowered_label.contains(QStringLiteral("idle"))) {
        settle_baseline_point = checkpoint.point;
        settle_baseline_valid = true;
        append_event_line(
            QStringLiteral("  baseline-after-settle captured at marker '%1'")
                .arg(label)
        );
    }

    if (settle_baseline_valid
        && checkpoint.point.cache_accounted_ready_bytes >= 0
        && settle_baseline_point.cache_accounted_ready_bytes >= 0) {
        const qint64 baseline_delta
            = checkpoint.point.cache_accounted_ready_bytes
            - settle_baseline_point.cache_accounted_ready_bytes;
        append_event_line(
            QStringLiteral(
                "  baseline-after-settle delta: cache(accounted)=%1 MiB"
            )
                .arg(
                    QString::number(
                        monitor_shared::to_mib(baseline_delta), 'f', 2
                    )
                )
        );
    }

    if (marker_cache_diffs.size() >= 3) {
        const int n = static_cast<int>(marker_cache_diffs.size());
        const bool strictly_positive = marker_cache_diffs.at(n - 1) > 0
            && marker_cache_diffs.at(n - 2) > 0
            && marker_cache_diffs.at(n - 3) > 0;
        monotonic_growth_suspicion = strictly_positive;
        if (strictly_positive) {
            append_warning_line(QStringLiteral(
                                    "[%1] monotonic-growth suspicion: last 3 "
                                    "marker cache diffs "
                                    "are positive"
            )
                                    .arg(monitor_shared::utc_now_text()));
        }
    }

    marker_history.push_back(checkpoint);
    while (marker_history.size() > marker_diff_history_limit) {
        marker_history.removeFirst();
    }

    schedule_chart_update();
    update_status_labels();
}

void external_monitor_window::handle_warning(const QJsonObject& message) {
    const QString code
        = message.value(QStringLiteral("warning_code")).toString();
    const QString warning_message
        = message.value(QStringLiteral("warning_message")).toString();
    append_warning_line(
        QStringLiteral("[%1] warning %2: %3")
            .arg(monitor_shared::utc_now_text(), code, warning_message)
    );
}

void external_monitor_window::handle_geometry(const QJsonObject& geometry) {
    if (geometry.isEmpty() || geometry_schematic == nullptr) {
        return;
    }
    const geometry_debug_snapshot snapshot = geometry_from_json(geometry);
    geometry_schematic->set_snapshot(snapshot);
    geometry_schematic->setAccessibleDescription(
        QStringLiteral(
            "%1 slots; window %2 by %3 pixels; required card short side %4 "
            "pixels at %5 device pixel ratio; active SVG cache bucket %6 "
            "pixels; accepted need window %7 to %8 pixels; decision %9."
        )
            .arg(snapshot.slot_count)
            .arg(snapshot.window_size.width())
            .arg(snapshot.window_size.height())
            .arg(snapshot.display_card_need_short_px)
            .arg(QString::number(snapshot.device_pixel_ratio, 'g', 4))
            .arg(snapshot.active_bucket_px)
            .arg(snapshot.cache_window_minimum_need_px)
            .arg(snapshot.cache_window_maximum_need_px)
            .arg(snapshot.cache_decision)
    );
}

void external_monitor_window::handle_layout_transition(
    const QJsonObject& event
) {
    QJsonObject payload
        = event.value(QStringLiteral("layout_transition")).toObject();
    if (payload.isEmpty()) {
        payload = event.value(QStringLiteral("transition")).toObject();
    }
    if (payload.isEmpty()) {
        payload = event;
    }
    const monitor_resize_history_widget::resize_entry entry {
        .timestamp_ms
        = payload.value(QStringLiteral("transition_end_timestamp_ms"))
              .toInteger(
                  payload.value(QStringLiteral("timestamp_ms")).toInteger()
              ),
        .prewarm_completion_ms
        = payload.value(QStringLiteral("prewarm_completion_ms")).toInteger(-1),
        .old_active_bucket_px
        = payload.value(QStringLiteral("old_active_bucket_px")).toInt(),
        .new_active_bucket_px
        = payload.value(QStringLiteral("new_active_bucket_px")).toInt(),
        .old_window_size
        = size_from_json(payload.value(QStringLiteral("old_window_size"))),
        .new_window_size
        = size_from_json(payload.value(QStringLiteral("new_window_size"))),
    };
    resize_entries.push_back(entry);
    while (resize_entries.size() > 64) {
        resize_entries.removeFirst();
    }
    if (resize_history_chart != nullptr) {
        resize_history_chart->set_entries(resize_entries);
        resize_history_chart->setAccessibleDescription(
            QStringLiteral(
                "%1 recent layout transitions; latest window %2 by "
                "%3 pixels; active SVG bucket %4 pixels."
            )
                .arg(resize_entries.size())
                .arg(entry.new_window_size.width())
                .arg(entry.new_window_size.height())
                .arg(entry.new_active_bucket_px)
        );
    }
    const QJsonObject geometry
        = payload.value(QStringLiteral("geometry_after_resize")).toObject();
    if (!geometry.isEmpty()) {
        handle_geometry(geometry);
    }
    append_event_line(
        QStringLiteral(
            "[%1] layout transition window=%2x%3 bucket=%4 prewarm=%5 "
            "ms"
        )
            .arg(monitor_shared::utc_now_text())
            .arg(entry.new_window_size.width())
            .arg(entry.new_window_size.height())
            .arg(entry.new_active_bucket_px)
            .arg(entry.prewarm_completion_ms)
    );
}

void external_monitor_window::handle_cache_decision(const QJsonObject& event) {
    QJsonObject payload = event.value(QStringLiteral("decision")).toObject();
    if (payload.isEmpty()) {
        payload = event;
    }

    QString decision = payload.value(QStringLiteral("reason")).toString();
    int required_short_px
        = payload.value(QStringLiteral("required_short_px")).toInt();
    int cached_short_px
        = payload.value(QStringLiteral("cached_short_px")).toInt();
    int target_short_px
        = payload.value(QStringLiteral("target_short_px")).toInt();
    if (decision.isEmpty()) {
        decision = payload.value(QStringLiteral("decision")).toString();
    }
    if (decision.isEmpty()) {
        decision = payload.value(QStringLiteral("cache_decision")).toString();
    }
    if (required_short_px <= 0) {
        required_short_px
            = payload.value(QStringLiteral("display_card_need_short_px"))
                  .toInt();
    }
    if (cached_short_px <= 0) {
        cached_short_px
            = payload.value(QStringLiteral("active_bucket_px")).toInt();
    }
    if (target_short_px <= 0) {
        target_short_px
            = payload.value(QStringLiteral("requested_target_bucket_px"))
                  .toInt();
    }
    QString trigger = payload.value(QStringLiteral("trigger")).toString();
    if (trigger.isEmpty()) {
        trigger = payload.value(QStringLiteral("cache_trigger")).toString();
    }
    append_event_line(
        QStringLiteral(
            "[%1] SVG cache decision=%2 required=%3 px cached=%4 px target=%5 "
            "px window=[%6,%7] trigger=%8"
        )
            .arg(monitor_shared::utc_now_text(), decision)
            .arg(required_short_px)
            .arg(cached_short_px)
            .arg(target_short_px)
            .arg(payload.value(QStringLiteral("cache_window_minimum_need_px"))
                     .toInt())
            .arg(payload.value(QStringLiteral("cache_window_maximum_need_px"))
                     .toInt())
            .arg(trigger)
    );
}

void external_monitor_window::update_primary_memory_chart() {
    if (primary_memory_chart == nullptr) {
        return;
    }

    if (!use_legacy_memory_view()) {
        QVector<monitor_line_chart_widget::series> lines;
        const QVector<QColor>& colors = generic_chart_palette();
        for (qsizetype index = 0; index < generic_primary_metric_ids.size();
             ++index) {
            const QString& metric_id = generic_primary_metric_ids.at(index);
            lines.push_back(
                monitor_line_chart_widget::series {
                    .label = metric_display_label(metric_id),
                    .color = colors.at(static_cast<int>(index) % colors.size()),
                    .values = generic_series_by_id.value(metric_id),
                }
            );
        }

        primary_memory_chart->set_title(
            QStringLiteral("Primary metrics (catalog-driven fallback)")
        );
        primary_memory_chart->set_unit_label(
            chart_label_for_unit(generic_primary_display_unit)
        );
        primary_memory_chart->set_x_axis_label(
            QStringLiteral("sample index (oldest -> newest)")
        );
        primary_memory_chart->set_series(lines);
        primary_memory_chart->set_footer_lines(
            generic_primary_metric_ids.isEmpty()
                ? QStringList() << QStringLiteral(
                      "Emitter does not currently expose chartable "
                      "primary metrics."
                  )
                : QStringList() << QStringLiteral(
                      "Fallback view for non-kcuckoounter metric "
                      "catalogs."
                  )
                                << QStringLiteral("Primary metrics: %1")
                                       .arg(primary_metric_labels_text())
        );
        return;
    }

    QVector<monitor_line_chart_widget::series> lines;
    lines.push_back(
        monitor_line_chart_widget::series {
            .label = QStringLiteral("Cache-accounted bytes (accounted)"),
            .color = monitor_palette::blue(),
            .values = series_cache_mib,
        }
    );
    lines.push_back(
        monitor_line_chart_widget::series {
            .label = QStringLiteral("Widget-local bytes (estimated)"),
            .color = monitor_palette::orange(),
            .values = series_widget_mib,
        }
    );
    lines.push_back(
        monitor_line_chart_widget::series {
            .label = QStringLiteral("Process RSS (measured)"),
            .color = monitor_palette::green(),
            .values = series_rss_mib,
        }
    );
    lines.push_back(
        monitor_line_chart_widget::series {
            .label = QStringLiteral("Measured-accounted gap (derived)"),
            .color = monitor_palette::red(),
            .values = series_gap_mib,
        }
    );
    primary_memory_chart->set_series(lines);
    primary_memory_chart->set_footer_lines(
        QStringList() << QStringLiteral(
            "Measured/accounted/estimated/derived remain separate."
        )
                      << QStringLiteral("Samples retained in-memory: %1")
                             .arg(series_cache_mib.size())
    );
}

void external_monitor_window::update_leak_signal_chart() {
    if (leak_signal_chart == nullptr) {
        return;
    }

    if (!use_legacy_memory_view()) {
        leak_signal_chart->set_title(QStringLiteral(
            "Leak-oriented derived lines unavailable for this emitter"
        ));
        leak_signal_chart->set_unit_label(QString());
        leak_signal_chart->set_x_axis_label(
            QStringLiteral("sample index (oldest -> newest)")
        );
        leak_signal_chart->set_series(
            QVector<monitor_line_chart_widget::series>()
        );
        leak_signal_chart->set_footer_lines(
            QStringList() << QStringLiteral(
                "This session exposes honest non-memory metrics, so "
                "kcuckoounter leak diagnostics stay disabled."
            )
        );
        return;
    }

    QVector<monitor_line_chart_widget::series> lines;
    lines.push_back(
        monitor_line_chart_widget::series {
            .label = QStringLiteral("Cache high-water (derived diagnostic)"),
            .color = monitor_palette::purple(),
            .values = series_high_water_cache_mib,
        }
    );
    lines.push_back(
        monitor_line_chart_widget::series {
            .label = QStringLiteral("Cache delta vs settle baseline (derived)"),
            .color = monitor_palette::orange(),
            .values = series_baseline_delta_mib,
        }
    );
    leak_signal_chart->set_series(lines);
    leak_signal_chart->set_footer_lines(
        QStringList() << QStringLiteral(
            "Leak aids only: high-water, baseline delta, marker diffs, "
            "monotonic suspicion."
        )
    );
}

void external_monitor_window::update_cache_entry_chart() {
    if (cache_entry_chart == nullptr) {
        return;
    }
    const double displayed = latest_numeric_metrics_by_id.value(
        QStringLiteral("displayed_recent_entries"), 0.0
    );
    const double cached_only = latest_numeric_metrics_by_id.value(
        QStringLiteral("cached_only_ready_entries"), 0.0
    );
    cache_entry_chart->set_slices(
        QVector<monitor_pie_chart_widget::slice> {
            {
                .label = QStringLiteral("Displayed recently"),
                .color = monitor_palette::blue(),
                .value = displayed,
            },
            {
                .label = QStringLiteral("Cached only"),
                .color = monitor_palette::orange(),
                .value = cached_only,
            },
        }
    );
    cache_entry_chart->set_footer_text(
        QStringLiteral("Entry counts only; memory estimates are not additive.")
    );
    cache_entry_chart->setAccessibleDescription(
        QStringLiteral(
            "%1 displayed-recent entries and %2 cached-only entries. "
            "These are counts, not memory ownership totals."
        )
            .arg(QString::number(displayed, 'g', 8))
            .arg(QString::number(cached_only, 'g', 8))
    );
}

void external_monitor_window::update_status_labels() {
    if (connection_status_label != nullptr) {
        connection_status_label->setText(
            QStringLiteral("Connection: %1")
                .arg(connection_status_label_for_state(session->state()))
        );
    }

    if (session_status_label != nullptr) {
        session_status_label->setText(
            QStringLiteral("Messages=%1 snapshots=%2 markers=%3 warnings=%4")
                .arg(line_counter)
                .arg(snapshot_counter)
                .arg(marker_counter)
                .arg(warning_counter)
        );
    }

    if (leak_status_label != nullptr) {
        if (!use_legacy_memory_view()) {
            QStringList summary_parts;
            for (const QString& metric_id : generic_primary_metric_ids) {
                const auto it
                    = latest_numeric_metrics_by_id.constFind(metric_id);
                if (it == latest_numeric_metrics_by_id.constEnd()) {
                    continue;
                }

                const QString unit = metric_unit(metric_id);
                if (unit == QStringLiteral("bytes")) {
                    summary_parts.push_back(
                        QStringLiteral("%1=%2 MiB")
                            .arg(metric_display_label(metric_id))
                            .arg(
                                QString::number(
                                    chart_value_for_metric(
                                        metric_id, it.value()
                                    ),
                                    'f', 2
                                )
                            )
                    );
                } else {
                    summary_parts.push_back(
                        QStringLiteral("%1=%2%3")
                            .arg(metric_display_label(metric_id))
                            .arg(QString::number(it.value(), 'g', 8))
                            .arg(
                                unit.isEmpty() ? QString()
                                               : QStringLiteral(" %1").arg(unit)
                            )
                    );
                }

                if (summary_parts.size() >= 3) {
                    break;
                }
            }

            leak_status_label->setText(
                summary_parts.isEmpty()
                    ? QStringLiteral(
                          "Primary metrics: awaiting numeric samples"
                      )
                    : QStringLiteral("Primary metrics: %1")
                          .arg(summary_parts.join(QStringLiteral(" | ")))
            );
        } else {
            const QString high_water_cache
                = high_water_point.cache_accounted_ready_bytes >= 0
                ? QString::number(
                      monitor_shared::to_mib(
                          high_water_point.cache_accounted_ready_bytes
                      ),
                      'f', 2
                  )
                : QStringLiteral("n/a");
            const QString baseline_delta = settle_baseline_valid
                    && latest_metric_point.cache_accounted_ready_bytes >= 0
                    && settle_baseline_point.cache_accounted_ready_bytes >= 0
                ? QString::number(
                      monitor_shared::to_mib(
                          latest_metric_point.cache_accounted_ready_bytes
                          - settle_baseline_point.cache_accounted_ready_bytes
                      ),
                      'f', 2
                  )
                : QStringLiteral("n/a");

            leak_status_label->setText(
                QStringLiteral(
                    "Leak signals: high-water=%1 MiB baseline-delta=%2 MiB "
                    "monotonic-suspicion=%3"
                )
                    .arg(high_water_cache)
                    .arg(baseline_delta)
                    .arg(
                        monotonic_growth_suspicion ? QStringLiteral("yes")
                                                   : QStringLiteral("no")
                    )
            );
        }
    }

    if (log_path_label != nullptr) {
        log_path_label->setText(QStringLiteral("Long-history log: %1")
                                    .arg(
                                        history_log_path.isEmpty()
                                            ? QStringLiteral("not started")
                                            : history_log_path
                                    ));
    }

    if (connect_button != nullptr) {
        connect_button->setEnabled(
            session->state()
            == telemetry_session::connection_state::disconnected
        );
    }
    if (disconnect_button != nullptr) {
        disconnect_button->setEnabled(
            session->state()
            != telemetry_session::connection_state::disconnected
        );
    }
}

void external_monitor_window::rebuild_generic_metric_selection() {
    generic_primary_metric_ids.clear();
    generic_primary_display_unit.clear();
    generic_series_by_id.clear();

    if (use_legacy_memory_view()) {
        return;
    }

    QVector<QString> primary_candidates;
    for (const QString& metric_id : metric_catalog_ids_in_order) {
        const QJsonObject hint = metric_hints_by_id.value(metric_id);
        const QString display_role
            = hint.value(QStringLiteral("default_display_role")).toString();
        if (display_role.startsWith(QStringLiteral("primary"))) {
            primary_candidates.push_back(metric_id);
        }
    }

    const QVector<QString>& source_ids = primary_candidates.isEmpty()
        ? metric_catalog_ids_in_order
        : primary_candidates;
    if (source_ids.isEmpty()) {
        return;
    }

    generic_primary_display_unit = metric_unit(source_ids.constFirst());
    for (const QString& metric_id : source_ids) {
        if (generic_primary_metric_ids.size() >= 4) {
            break;
        }
        if (!generic_primary_display_unit.isEmpty()
            && metric_unit(metric_id) != generic_primary_display_unit) {
            continue;
        }
        generic_primary_metric_ids.push_back(metric_id);
        generic_series_by_id.insert(metric_id, QVector<double>());
    }

    if (generic_primary_metric_ids.isEmpty()) {
        generic_primary_display_unit.clear();
        for (const QString& metric_id : source_ids) {
            if (generic_primary_metric_ids.size() >= 4) {
                break;
            }
            generic_primary_metric_ids.push_back(metric_id);
            generic_series_by_id.insert(metric_id, QVector<double>());
        }
    }
}

bool external_monitor_window::has_legacy_memory_metrics() const {
    for (auto it = metric_hints_by_id.cbegin(); it != metric_hints_by_id.cend();
         ++it) {
        if (is_legacy_memory_metric_id(it.key())) {
            return true;
        }
    }
    return false;
}

bool external_monitor_window::use_legacy_memory_view() const {
    return legacy_memory_view_active;
}

QString
external_monitor_window::metric_display_label(const QString& metric_id) const {
    const QJsonObject hint = metric_hints_by_id.value(metric_id);
    const QString explicit_label
        = hint.value(QStringLiteral("label")).toString();
    if (!explicit_label.trimmed().isEmpty()) {
        return explicit_label.trimmed();
    }
    const QString display_role
        = hint.value(QStringLiteral("default_display_role")).toString();
    QString pretty_id = metric_id;
    pretty_id.replace(QLatin1Char('_'), QLatin1Char(' '));
    if (display_role.isEmpty()
        || display_role.startsWith(QStringLiteral("primary"))) {
        return pretty_id;
    }
    return QStringLiteral("%1 (%2)").arg(pretty_id, display_role);
}

QString external_monitor_window::primary_metric_labels_text() const {
    QStringList labels;
    for (const QString& metric_id : generic_primary_metric_ids) {
        labels.push_back(metric_display_label(metric_id));
    }
    return labels.join(QStringLiteral(", "));
}

QString external_monitor_window::metric_unit(const QString& metric_id) const {
    return metric_hints_by_id.value(metric_id)
        .value(QStringLiteral("unit"))
        .toString();
}

QString external_monitor_window::chart_label_for_unit(const QString& unit) {
    if (unit == QStringLiteral("bytes")) {
        return QStringLiteral("MiB");
    }
    if (unit == QStringLiteral("count")) {
        return QStringLiteral("count");
    }
    return unit;
}

double external_monitor_window::chart_value_for_metric(
    const QString& metric_id, double value
) const {
    return metric_unit(metric_id) == QStringLiteral("bytes")
        ? value / (1024.0 * 1024.0)
        : value;
}

void external_monitor_window::append_generic_chart_sample() {
    if (use_legacy_memory_view() || generic_primary_metric_ids.isEmpty()) {
        return;
    }

    for (const QString& metric_id : generic_primary_metric_ids) {
        QVector<double>& series = generic_series_by_id[metric_id];
        const auto it = latest_numeric_metrics_by_id.constFind(metric_id);
        series.push_back(
            it == latest_numeric_metrics_by_id.constEnd()
                ? std::numeric_limits<double>::quiet_NaN()
                : chart_value_for_metric(metric_id, it.value())
        );
        trim_series_to_limit(&series);
    }
}

void external_monitor_window::merge_numeric_snapshot(
    const QJsonObject& snapshot
) {
    for (auto it = snapshot.constBegin(); it != snapshot.constEnd(); ++it) {
        const QJsonValue& value = it.value();
        if (!is_numeric_json_value(value)) {
            continue;
        }
        latest_numeric_metrics_by_id.insert(it.key(), value.toDouble());
    }
}

bool external_monitor_window::is_numeric_json_value(const QJsonValue& value) {
    return value.isDouble();
}

bool external_monitor_window::is_legacy_memory_metric_id(
    const QString& metric_id
) {
    return metric_id == QStringLiteral("cache_accounted_ready_bytes")
        || metric_id == QStringLiteral("widget_local_display_bytes_estimated")
        || metric_id == QStringLiteral("measured_accounted_gap_bytes_derived");
}

void external_monitor_window::append_event_line(const QString& line) {
    if (events_text == nullptr) {
        return;
    }
    events_text->appendPlainText(line);
}

void external_monitor_window::append_warning_line(const QString& line) {
    ++warning_counter;
    if (warnings_text != nullptr) {
        warnings_text->appendPlainText(line);
    }
    update_status_labels();
}

void external_monitor_window::append_parity_warning(
    const QString& code, const QString& details
) {
    append_warning_line(
        QStringLiteral("[%1] parity warning %2: %3")
            .arg(monitor_shared::utc_now_text(), code, details)
    );
}

QString external_monitor_window::message_family(const QJsonObject& message) {
    const QJsonObject protocol
        = message.value(QStringLiteral("protocol_v1")).toObject();
    return protocol.value(QStringLiteral("message_family")).toString();
}

QJsonObject
external_monitor_window::protocol_identity(const QJsonObject& message) {
    const QJsonObject protocol
        = message.value(QStringLiteral("protocol_v1")).toObject();
    return protocol.value(QStringLiteral("identity")).toObject();
}

qint64 external_monitor_window::integer_like_value(const QJsonValue& value) {
    if (value.isDouble()) {
        return static_cast<qint64>(std::llround(value.toDouble()));
    }
    return value.toInteger();
}

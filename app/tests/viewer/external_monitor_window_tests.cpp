#include "../include/viewer/external_monitor_window_tests.hpp"

#include "viewer/external_monitor_window.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QPlainTextEdit>
#include <QtTest/QtTest>

namespace external_monitor_window_tests_support {

void append_metric_sample(
    QJsonArray* samples, const QString& metric_id, qint64 value
) {
    if (samples == nullptr) {
        return;
    }

    QJsonObject sample;
    sample.insert(QStringLiteral("metric_id"), metric_id);
    sample.insert(QStringLiteral("value"), value);
    samples->push_back(sample);
}

QJsonObject metric_catalog_entry(
    const QString& id, const QString& label, const QString& kind,
    const QString& provenance, const QString& unit, const QString& scope,
    const QString& cardinality_semantics, const QString& stability,
    const QString& additive_semantics, const QString& confidence,
    const QString& default_display_role, const QString& domain_namespace
) {
    QJsonObject object;
    object.insert(QStringLiteral("id"), id);
    object.insert(QStringLiteral("label"), label);
    object.insert(QStringLiteral("kind"), kind);
    object.insert(QStringLiteral("provenance"), provenance);
    object.insert(QStringLiteral("unit"), unit);
    object.insert(QStringLiteral("scope"), scope);
    object.insert(
        QStringLiteral("cardinality_semantics"), cardinality_semantics
    );
    object.insert(QStringLiteral("stability"), stability);
    object.insert(QStringLiteral("additive_semantics"), additive_semantics);
    object.insert(QStringLiteral("confidence"), confidence);
    object.insert(QStringLiteral("default_display_role"), default_display_role);
    object.insert(QStringLiteral("domain_namespace"), domain_namespace);
    return object;
}

QJsonObject make_protocol_message(
    const QString& family, const QString& app_name, const QJsonObject& payload
) {
    QJsonObject identity;
    identity.insert(QStringLiteral("app"), app_name);
    identity.insert(QStringLiteral("pid"), qint64(77));
    identity.insert(QStringLiteral("session"), QStringLiteral("session-test"));
    identity.insert(QStringLiteral("build"), QStringLiteral("debug"));
    identity.insert(
        QStringLiteral("instrumentation_mode"), QStringLiteral("debug_opt_in")
    );

    QJsonObject protocol;
    protocol.insert(QStringLiteral("message_family"), family);
    protocol.insert(QStringLiteral("identity"), identity);
    protocol.insert(
        QStringLiteral("version"), QStringLiteral("debug_telemetry.v1")
    );

    QJsonObject message = payload;
    message.insert(QStringLiteral("protocol_v1"), protocol);
    message.insert(QStringLiteral("monotonic_timestamp_ms"), qint64(1234));
    return message;
}

QJsonObject make_capabilities_message() {
    QJsonArray catalog;
    catalog.push_back(metric_catalog_entry(
        QStringLiteral("configured_stream_count"),
        QStringLiteral("Configured streams"), QStringLiteral("count"),
        QStringLiteral("accounted"), QStringLiteral("count"),
        QStringLiteral("application"), QStringLiteral("stock"),
        QStringLiteral("stable"), QStringLiteral("additive_within_scope"),
        QStringLiteral("exact"), QStringLiteral("primary"),
        QStringLiteral("yodau.streams")
    ));
    catalog.push_back(metric_catalog_entry(
        QStringLiteral("visible_stream_count"),
        QStringLiteral("Visible streams"), QStringLiteral("count"),
        QStringLiteral("accounted"), QStringLiteral("count"),
        QStringLiteral("application"), QStringLiteral("stock"),
        QStringLiteral("recent_window"),
        QStringLiteral("additive_within_scope"), QStringLiteral("exact"),
        QStringLiteral("primary"), QStringLiteral("yodau.streams")
    ));
    catalog.push_back(metric_catalog_entry(
        QStringLiteral("active_stream_count"), QStringLiteral("Active streams"),
        QStringLiteral("count"), QStringLiteral("accounted"),
        QStringLiteral("count"), QStringLiteral("application"),
        QStringLiteral("state"), QStringLiteral("stable"),
        QStringLiteral("non_additive"), QStringLiteral("exact"),
        QStringLiteral("primary"), QStringLiteral("yodau.streams")
    ));
    catalog.push_back(metric_catalog_entry(
        QStringLiteral("configured_line_count"),
        QStringLiteral("Configured lines"), QStringLiteral("count"),
        QStringLiteral("accounted"), QStringLiteral("count"),
        QStringLiteral("application"), QStringLiteral("stock"),
        QStringLiteral("stable"), QStringLiteral("additive_within_scope"),
        QStringLiteral("exact"), QStringLiteral("primary"),
        QStringLiteral("yodau.streams")
    ));
    catalog.push_back(metric_catalog_entry(
        QStringLiteral("process_memory_rss_bytes"),
        QStringLiteral("Process RSS (measured)"), QStringLiteral("memory"),
        QStringLiteral("measured"), QStringLiteral("bytes"),
        QStringLiteral("process"), QStringLiteral("stock"),
        QStringLiteral("stable"), QStringLiteral("non_additive_system_total"),
        QStringLiteral("exact"), QStringLiteral("primary"),
        QStringLiteral("system.process")
    ));

    QJsonObject capabilities;
    capabilities.insert(QStringLiteral("metric_catalog"), catalog);

    QJsonObject payload;
    payload.insert(QStringLiteral("capabilities"), capabilities);
    return make_protocol_message(
        QStringLiteral("capabilities"), QStringLiteral("yodau"), payload
    );
}

QJsonObject make_sample_batch_message() {
    QJsonArray samples;

    append_metric_sample(
        &samples, QStringLiteral("configured_stream_count"), 4
    );
    append_metric_sample(&samples, QStringLiteral("visible_stream_count"), 3);
    append_metric_sample(&samples, QStringLiteral("active_stream_count"), 2);
    append_metric_sample(&samples, QStringLiteral("configured_line_count"), 6);
    append_metric_sample(
        &samples, QStringLiteral("process_memory_rss_bytes"), 32 * 1024 * 1024
    );

    QJsonObject payload;
    payload.insert(QStringLiteral("samples"), samples);
    return make_protocol_message(
        QStringLiteral("sample_batch"), QStringLiteral("yodau"), payload
    );
}

QJsonObject make_snapshot_message() {
    QJsonObject snapshot;
    snapshot.insert(QStringLiteral("configured_stream_count"), 7);
    snapshot.insert(QStringLiteral("visible_stream_count"), 5);
    snapshot.insert(QStringLiteral("active_stream_count"), 3);
    snapshot.insert(QStringLiteral("configured_line_count"), 8);
    snapshot.insert(QStringLiteral("reason"), QStringLiteral("manual"));

    QJsonObject payload;
    payload.insert(QStringLiteral("snapshot"), snapshot);
    return make_protocol_message(
        QStringLiteral("snapshot"), QStringLiteral("yodau"), payload
    );
}

QJsonObject make_event_batch_message() {
    QJsonArray events;

    QJsonObject first_event;
    first_event.insert(QStringLiteral("kind"), QStringLiteral("motion"));
    first_event.insert(QStringLiteral("timestamp_ms"), qint64(2200));
    first_event.insert(QStringLiteral("collector_sequence"), qint64(5));
    first_event.insert(QStringLiteral("stream_name"), QStringLiteral("cam-a"));
    first_event.insert(
        QStringLiteral("line_name"), QStringLiteral("tripwire-1")
    );
    first_event.insert(
        QStringLiteral("message"), QStringLiteral("motion detected")
    );
    events.push_back(first_event);

    QJsonObject payload;
    payload.insert(QStringLiteral("events"), events);
    return make_protocol_message(
        QStringLiteral("event_batch"), QStringLiteral("yodau"), payload
    );
}

} // namespace external_monitor_window_tests_support

using external_monitor_window_tests_support::make_capabilities_message;
using external_monitor_window_tests_support::make_event_batch_message;
using external_monitor_window_tests_support::make_sample_batch_message;
using external_monitor_window_tests_support::make_snapshot_message;

void external_monitor_window_tests::
    generic_catalog_drives_primary_metric_selection() {
    external_monitor_window window;

    window.handle_capabilities(make_capabilities_message());

    QVERIFY(!window.use_legacy_memory_view());
    QCOMPARE(window.current_app_name, QStringLiteral("yodau"));
    QCOMPARE(window.metric_catalog_ids_in_order.size(), 5);
    QCOMPARE(window.generic_primary_display_unit, QStringLiteral("count"));
    QCOMPARE(window.generic_primary_metric_ids.size(), 4);
    QCOMPARE(
        window.generic_primary_metric_ids.at(0),
        QStringLiteral("configured_stream_count")
    );
    QCOMPARE(
        window.generic_primary_metric_ids.at(1),
        QStringLiteral("visible_stream_count")
    );
    QCOMPARE(
        window.generic_primary_metric_ids.at(2),
        QStringLiteral("active_stream_count")
    );
    QCOMPARE(
        window.generic_primary_metric_ids.at(3),
        QStringLiteral("configured_line_count")
    );
    QVERIFY(window.warnings_text->toPlainText().trimmed().isEmpty());

    window.handle_sample_batch(make_sample_batch_message());

    QCOMPARE(
        window.latest_numeric_metrics_by_id.value(
            QStringLiteral("configured_stream_count")
        ),
        qint64(4)
    );
    QCOMPARE(
        window.latest_numeric_metrics_by_id.value(
            QStringLiteral("configured_line_count")
        ),
        qint64(6)
    );
    QCOMPARE(
        window.generic_series_by_id
            .value(QStringLiteral("configured_stream_count"))
            .size(),
        1
    );
    QCOMPARE(
        window.generic_series_by_id
            .value(QStringLiteral("configured_stream_count"))
            .constLast(),
        4.0
    );
    QCOMPARE(
        window.generic_series_by_id
            .value(QStringLiteral("visible_stream_count"))
            .constLast(),
        3.0
    );
    QVERIFY(window.leak_status_label->text().contains(
        QStringLiteral("Primary metrics:")
    ));
}

void external_monitor_window_tests::
    generic_snapshot_and_event_batch_update_monitor_state() {
    external_monitor_window window;

    window.handle_capabilities(make_capabilities_message());
    window.handle_sample_batch(make_sample_batch_message());
    window.handle_snapshot(make_snapshot_message());

    QCOMPARE(
        window.latest_numeric_metrics_by_id.value(
            QStringLiteral("configured_stream_count")
        ),
        qint64(7)
    );
    QCOMPARE(
        window.latest_numeric_metrics_by_id.value(
            QStringLiteral("visible_stream_count")
        ),
        qint64(5)
    );
    QVERIFY(window.snapshot_text->toPlainText().contains(
        QStringLiteral("\"configured_stream_count\": 7")
    ));

    window.handle_event_batch(make_event_batch_message());
    const QString events_log = window.events_text->toPlainText();
    QVERIFY(events_log.contains(QStringLiteral("kind=motion")));
    QVERIFY(events_log.contains(QStringLiteral("stream=cam-a")));
    QVERIFY(events_log.contains(QStringLiteral("line=tripwire-1")));
    QVERIFY(events_log.contains(QStringLiteral("message=motion detected")));
}

#include "../include/parity/parity_checker_tests.hpp"

#include "parity/parity_checker.hpp"
#include "telemetry/debug_probe_core.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <QtTest/QtTest>

namespace parity_checker_tests_support {

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

QJsonObject make_protocol_message(
    const QString& family, const QString& session_id,
    const QJsonObject& payload, const QString& app_name = QStringLiteral("cppr")
) {
    const debug_probe_core::protocol_identity identity {
        .app_name = app_name,
        .process_id = 777,
        .session_id = session_id,
        .build_id = QStringLiteral("build-dev"),
        .protocol_version = debug_probe_core::protocol_version_string(),
        .debug_flags = QStringList() << QStringLiteral("debug_build"),
        .instrumentation_mode = QStringLiteral("realistic"),
    };
    return debug_probe_core::build_protocol_message_v1(
        family, identity, 10, payload
    );
}

QJsonObject make_embedded_snapshot_export(
    qint64 cache_accounted_ready_bytes, qint64 widget_local_estimated_bytes,
    qint64 process_rss_bytes
) {
    QJsonObject latest;
    latest.insert(
        QStringLiteral("cache_accounted_ready_bytes"),
        cache_accounted_ready_bytes
    );
    latest.insert(
        QStringLiteral("widget_local_display_bytes_estimated"),
        widget_local_estimated_bytes
    );
    latest.insert(
        QStringLiteral("process_memory_rss_bytes"), process_rss_bytes
    );
    latest.insert(
        QStringLiteral("measured_accounted_gap_bytes_derived"),
        process_rss_bytes - cache_accounted_ready_bytes
    );

    QJsonArray cache_timeline;
    cache_timeline.push_back(latest);

    QJsonObject root;
    root.insert(QStringLiteral("cache_timeline"), cache_timeline);
    return root;
}

QJsonObject make_sample_batch(
    qint64 cache_accounted_ready_bytes, qint64 widget_local_estimated_bytes,
    qint64 process_rss_bytes
) {
    QJsonArray samples;

    append_metric_sample(
        &samples, QStringLiteral("cache_accounted_ready_bytes"),
        cache_accounted_ready_bytes
    );
    append_metric_sample(
        &samples, QStringLiteral("widget_local_display_bytes_estimated"),
        widget_local_estimated_bytes
    );
    append_metric_sample(
        &samples, QStringLiteral("process_memory_rss_bytes"), process_rss_bytes
    );
    append_metric_sample(
        &samples, QStringLiteral("measured_accounted_gap_bytes_derived"),
        process_rss_bytes - cache_accounted_ready_bytes
    );

    QJsonObject payload;
    payload.insert(QStringLiteral("sample_count"), samples.size());
    payload.insert(QStringLiteral("samples"), samples);
    return payload;
}

QJsonObject make_capabilities_message(const QString& session_id) {
    QJsonObject payload;
    payload.insert(
        QStringLiteral("capabilities"),
        debug_probe_core::protocol_capabilities_v1()
    );
    return make_protocol_message(
        QStringLiteral("capabilities"), session_id, payload
    );
}

QJsonObject make_generic_snapshot_export(
    qint64 configured_stream_count, qint64 visible_stream_count,
    qint64 active_stream_count, qint64 configured_line_count,
    qint64 process_rss_bytes
) {
    QJsonObject snapshot;
    snapshot.insert(
        QStringLiteral("configured_stream_count"), configured_stream_count
    );
    snapshot.insert(
        QStringLiteral("visible_stream_count"), visible_stream_count
    );
    snapshot.insert(QStringLiteral("active_stream_count"), active_stream_count);
    snapshot.insert(
        QStringLiteral("configured_line_count"), configured_line_count
    );
    snapshot.insert(
        QStringLiteral("process_memory_rss_bytes"), process_rss_bytes
    );
    return snapshot;
}

QJsonObject make_generic_sample_batch(
    qint64 configured_stream_count, qint64 visible_stream_count,
    qint64 active_stream_count, qint64 configured_line_count,
    qint64 process_rss_bytes
) {
    QJsonArray samples;

    append_metric_sample(
        &samples, QStringLiteral("configured_stream_count"),
        configured_stream_count
    );
    append_metric_sample(
        &samples, QStringLiteral("visible_stream_count"), visible_stream_count
    );
    append_metric_sample(
        &samples, QStringLiteral("active_stream_count"), active_stream_count
    );
    append_metric_sample(
        &samples, QStringLiteral("configured_line_count"), configured_line_count
    );
    append_metric_sample(
        &samples, QStringLiteral("process_memory_rss_bytes"), process_rss_bytes
    );

    QJsonObject payload;
    payload.insert(QStringLiteral("sample_count"), samples.size());
    payload.insert(QStringLiteral("samples"), samples);
    return payload;
}

QJsonObject make_generic_capabilities_message(const QString& session_id) {
    QJsonArray catalog;
    catalog.push_back(
        QJsonObject {
            { QStringLiteral("id"),
              QStringLiteral("process_memory_rss_bytes") },
            { QStringLiteral("label"),
              QStringLiteral("Process RSS (measured)") },
            { QStringLiteral("kind"), QStringLiteral("memory") },
            { QStringLiteral("provenance"), QStringLiteral("measured") },
            { QStringLiteral("unit"), QStringLiteral("bytes") },
            { QStringLiteral("scope"), QStringLiteral("process") },
            { QStringLiteral("cardinality_semantics"),
              QStringLiteral("stock") },
            { QStringLiteral("stability"), QStringLiteral("stable") },
            {
                QStringLiteral("additive_semantics"),
                QStringLiteral("non_additive_system_total"),
            },
            { QStringLiteral("confidence"), QStringLiteral("exact") },
            { QStringLiteral("default_display_role"),
              QStringLiteral("primary") },
            { QStringLiteral("domain_namespace"),
              QStringLiteral("system.process") },
        }
    );
    catalog.push_back(
        QJsonObject {
            { QStringLiteral("id"), QStringLiteral("configured_stream_count") },
            { QStringLiteral("label"), QStringLiteral("Configured streams") },
            { QStringLiteral("kind"), QStringLiteral("count") },
            { QStringLiteral("provenance"), QStringLiteral("accounted") },
            { QStringLiteral("unit"), QStringLiteral("count") },
            { QStringLiteral("scope"), QStringLiteral("application") },
            { QStringLiteral("cardinality_semantics"),
              QStringLiteral("stock") },
            { QStringLiteral("stability"), QStringLiteral("stable") },
            {
                QStringLiteral("additive_semantics"),
                QStringLiteral("additive_within_scope"),
            },
            { QStringLiteral("confidence"), QStringLiteral("exact") },
            { QStringLiteral("default_display_role"),
              QStringLiteral("secondary") },
            { QStringLiteral("domain_namespace"),
              QStringLiteral("yodau.streams") },
        }
    );
    catalog.push_back(
        QJsonObject {
            { QStringLiteral("id"), QStringLiteral("visible_stream_count") },
            { QStringLiteral("label"), QStringLiteral("Visible streams") },
            { QStringLiteral("kind"), QStringLiteral("count") },
            { QStringLiteral("provenance"), QStringLiteral("accounted") },
            { QStringLiteral("unit"), QStringLiteral("count") },
            { QStringLiteral("scope"), QStringLiteral("application") },
            {
                QStringLiteral("cardinality_semantics"),
                QStringLiteral("recent_window"),
            },
            { QStringLiteral("stability"), QStringLiteral("stable") },
            {
                QStringLiteral("additive_semantics"),
                QStringLiteral("additive_within_scope"),
            },
            { QStringLiteral("confidence"), QStringLiteral("exact") },
            { QStringLiteral("default_display_role"),
              QStringLiteral("secondary") },
            { QStringLiteral("domain_namespace"),
              QStringLiteral("yodau.streams") },
        }
    );
    catalog.push_back(
        QJsonObject {
            { QStringLiteral("id"), QStringLiteral("active_stream_count") },
            { QStringLiteral("label"), QStringLiteral("Active streams") },
            { QStringLiteral("kind"), QStringLiteral("count") },
            { QStringLiteral("provenance"), QStringLiteral("accounted") },
            { QStringLiteral("unit"), QStringLiteral("count") },
            { QStringLiteral("scope"), QStringLiteral("application") },
            { QStringLiteral("cardinality_semantics"),
              QStringLiteral("state") },
            { QStringLiteral("stability"), QStringLiteral("stable") },
            { QStringLiteral("additive_semantics"),
              QStringLiteral("non_additive") },
            { QStringLiteral("confidence"), QStringLiteral("exact") },
            { QStringLiteral("default_display_role"),
              QStringLiteral("secondary") },
            { QStringLiteral("domain_namespace"),
              QStringLiteral("yodau.streams") },
        }
    );
    catalog.push_back(
        QJsonObject {
            { QStringLiteral("id"), QStringLiteral("configured_line_count") },
            { QStringLiteral("label"), QStringLiteral("Configured lines") },
            { QStringLiteral("kind"), QStringLiteral("count") },
            { QStringLiteral("provenance"), QStringLiteral("accounted") },
            { QStringLiteral("unit"), QStringLiteral("count") },
            { QStringLiteral("scope"), QStringLiteral("application") },
            { QStringLiteral("cardinality_semantics"),
              QStringLiteral("stock") },
            { QStringLiteral("stability"), QStringLiteral("stable") },
            {
                QStringLiteral("additive_semantics"),
                QStringLiteral("additive_within_scope"),
            },
            { QStringLiteral("confidence"), QStringLiteral("exact") },
            { QStringLiteral("default_display_role"),
              QStringLiteral("secondary") },
            { QStringLiteral("domain_namespace"),
              QStringLiteral("yodau.streams") },
        }
    );

    QJsonObject capabilities;
    capabilities.insert(QStringLiteral("metric_catalog"), catalog);

    QJsonObject payload;
    payload.insert(QStringLiteral("capabilities"), capabilities);
    return make_protocol_message(
        QStringLiteral("capabilities"), session_id, payload,
        QStringLiteral("yodau")
    );
}

QJsonObject make_snapshot_message(
    const QString& session_id, qint64 cache_accounted_ready_bytes,
    qint64 widget_local_estimated_bytes, qint64 process_rss_bytes
) {
    QJsonObject snapshot;
    snapshot.insert(
        QStringLiteral("cache_accounted_ready_bytes"),
        cache_accounted_ready_bytes
    );
    snapshot.insert(
        QStringLiteral("widget_local_display_bytes_estimated"),
        widget_local_estimated_bytes
    );
    snapshot.insert(
        QStringLiteral("process_memory_rss_bytes"), process_rss_bytes
    );

    QJsonObject payload;
    payload.insert(QStringLiteral("snapshot"), snapshot);
    return make_protocol_message(
        QStringLiteral("snapshot"), session_id, payload
    );
}

} // namespace parity_checker_tests_support

using parity_checker_tests_support::make_capabilities_message;
using parity_checker_tests_support::make_embedded_snapshot_export;
using parity_checker_tests_support::make_generic_capabilities_message;
using parity_checker_tests_support::make_generic_sample_batch;
using parity_checker_tests_support::make_generic_snapshot_export;
using parity_checker_tests_support::make_protocol_message;
using parity_checker_tests_support::make_sample_batch;
using parity_checker_tests_support::make_snapshot_message;

void monitor_parity_checker_tests::
    aligned_embedded_and_external_payloads_have_no_warnings() {
    const qint64 cache_bytes = 80 * 1024 * 1024;
    const qint64 widget_bytes = 16 * 1024 * 1024;
    const qint64 rss_bytes = 180 * 1024 * 1024;
    const QString session_id = QStringLiteral("session-aligned");

    const QJsonObject embedded_snapshot
        = make_embedded_snapshot_export(cache_bytes, widget_bytes, rss_bytes);
    const QVector<QJsonObject> external_messages {
        make_capabilities_message(session_id),
        make_protocol_message(
            QStringLiteral("sample_batch"), session_id,
            make_sample_batch(cache_bytes, widget_bytes, rss_bytes)
        ),
        make_snapshot_message(session_id, cache_bytes, widget_bytes, rss_bytes),
    };

    const auto parity = monitor_parity_checker::
        compare_embedded_snapshot_and_external_messages(
            embedded_snapshot, external_messages
        );
    QVERIFY(parity.ok());
    QVERIFY(parity.warnings.isEmpty());
    QCOMPARE(parity.compared_message_count, qint64(3));
    QVERIFY(parity.compared_metric_count >= 4);
}

void monitor_parity_checker_tests::drifting_payloads_surface_warnings() {
    const qint64 embedded_cache_bytes = 64 * 1024 * 1024;
    const qint64 embedded_widget_bytes = 8 * 1024 * 1024;
    const qint64 embedded_rss_bytes = 160 * 1024 * 1024;
    const QString session_id = QStringLiteral("session-drift");

    const QJsonObject embedded_snapshot = make_embedded_snapshot_export(
        embedded_cache_bytes, embedded_widget_bytes, embedded_rss_bytes
    );
    const QVector<QJsonObject> external_messages {
        make_capabilities_message(session_id),
        make_protocol_message(
            QStringLiteral("sample_batch"), session_id,
            make_sample_batch(
                embedded_cache_bytes + (32 * 1024 * 1024),
                embedded_widget_bytes + (16 * 1024 * 1024),
                embedded_rss_bytes + (48 * 1024 * 1024)
            )
        ),
    };

    const auto parity = monitor_parity_checker::
        compare_embedded_snapshot_and_external_messages(
            embedded_snapshot, external_messages, 1 * 1024 * 1024
        );
    QVERIFY(!parity.ok());
    QVERIFY(!parity.warnings.isEmpty());
}

void monitor_parity_checker_tests::
    generic_aligned_payloads_compare_on_overlapping_metrics_only() {
    const QString session_id = QStringLiteral("session-generic-aligned");
    const QJsonObject embedded_snapshot
        = make_generic_snapshot_export(4, 3, 2, 6, 96 * 1024 * 1024);
    const QVector<QJsonObject> external_messages {
        make_generic_capabilities_message(session_id),
        make_protocol_message(
            QStringLiteral("sample_batch"), session_id,
            make_generic_sample_batch(4, 3, 2, 6, 96 * 1024 * 1024),
            QStringLiteral("yodau")
        ),
    };

    const auto parity = monitor_parity_checker::
        compare_embedded_snapshot_and_external_messages(
            embedded_snapshot, external_messages
        );
    QVERIFY(parity.ok());
    QVERIFY(parity.warnings.isEmpty());
    QVERIFY(parity.compared_metric_count >= 5);
}

void monitor_parity_checker_tests::
    legacy_and_generic_payloads_compare_only_shared_metric() {
    const QString session_id = QStringLiteral("session-generic-rss-overlap");
    const QJsonObject embedded_snapshot = make_embedded_snapshot_export(
        64 * 1024 * 1024, 8 * 1024 * 1024, 160 * 1024 * 1024
    );
    const QVector<QJsonObject> external_messages {
        make_generic_capabilities_message(session_id),
        make_protocol_message(
            QStringLiteral("sample_batch"), session_id,
            make_generic_sample_batch(4, 3, 2, 6, 160 * 1024 * 1024),
            QStringLiteral("yodau")
        ),
    };

    const auto parity = monitor_parity_checker::
        compare_embedded_snapshot_and_external_messages(
            embedded_snapshot, external_messages
        );
    QVERIFY(parity.ok());
    QVERIFY(parity.warnings.isEmpty());
    QCOMPARE(parity.compared_metric_count, qint64(1));
}

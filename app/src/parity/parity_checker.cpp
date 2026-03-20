#include "parity/parity_checker.hpp"

#include "telemetry/debug_probe_core.hpp"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStringList>
#include <QtGlobal>

#include <cmath>

using numeric_metric_map = QHash<QString, qint64>;

namespace parity_checker_support {

qint64 integer_like_value(const QJsonValue& value) {
    if (value.isDouble()) {
        return static_cast<qint64>(std::llround(value.toDouble()));
    }
    return value.toInteger();
}

void compare_metric_with_tolerance(
    const QString& metric_name, qint64 embedded_value, qint64 external_value,
    qint64 tolerance, monitor_parity_checker::parity_result* result
) {
    if (result == nullptr) {
        return;
    }
    if (embedded_value < 0 || external_value < 0) {
        result->warnings.push_back(
            QStringLiteral(
                "metric '%1' unavailable for parity comparison (embedded=%2 "
                "external=%3)"
            )
                .arg(metric_name)
                .arg(embedded_value)
                .arg(external_value)
        );
        return;
    }

    ++result->compared_metric_count;
    if (std::llabs(embedded_value - external_value) > tolerance) {
        result->warnings.push_back(
            QStringLiteral(
                "metric '%1' drift exceeds tolerance: embedded=%2 external=%3 "
                "tolerance=%4"
            )
                .arg(metric_name)
                .arg(embedded_value)
                .arg(external_value)
                .arg(tolerance)
        );
    }
}

void merge_numeric_object(
    numeric_metric_map* target, const QJsonObject& object
) {
    if (target == nullptr) {
        return;
    }

    for (auto it = object.begin(); it != object.end(); ++it) {
        if (!it.value().isDouble()) {
            continue;
        }
        target->insert(it.key(), integer_like_value(it.value()));
    }
}

void merge_sample_batch_metrics(
    numeric_metric_map* target, const QJsonArray& samples
) {
    if (target == nullptr) {
        return;
    }

    for (const QJsonValue& sample_value : samples) {
        const QJsonObject sample = sample_value.toObject();
        const QString metric_id
            = sample.value(QStringLiteral("metric_id")).toString();
        const QJsonValue value = sample.value(QStringLiteral("value"));
        if (metric_id.isEmpty() || !value.isDouble()) {
            continue;
        }
        target->insert(metric_id, integer_like_value(value));
    }
}

void populate_derived_metrics(numeric_metric_map* target) {
    if (target == nullptr) {
        return;
    }

    if (!target->contains(
            QStringLiteral("measured_accounted_gap_bytes_derived")
        )
        && target->contains(QStringLiteral("process_memory_rss_bytes"))
        && target->contains(QStringLiteral("cache_accounted_ready_bytes"))) {
        target->insert(
            QStringLiteral("measured_accounted_gap_bytes_derived"),
            target->value(QStringLiteral("process_memory_rss_bytes"))
                - target->value(QStringLiteral("cache_accounted_ready_bytes"))
        );
    }
}

numeric_metric_map extract_embedded_metrics(
    const QJsonObject& embedded_snapshot_export,
    monitor_parity_checker::parity_result* result
) {
    numeric_metric_map embedded_metrics;

    const QJsonArray cache_timeline
        = embedded_snapshot_export.value(QStringLiteral("cache_timeline"))
              .toArray();
    if (!cache_timeline.isEmpty()) {
        merge_numeric_object(
            &embedded_metrics,
            cache_timeline.at(cache_timeline.size() - 1).toObject()
        );
        populate_derived_metrics(&embedded_metrics);
        return embedded_metrics;
    }

    const QJsonObject nested_snapshot
        = embedded_snapshot_export.value(QStringLiteral("snapshot")).toObject();
    if (!nested_snapshot.isEmpty()) {
        merge_numeric_object(&embedded_metrics, nested_snapshot);
        populate_derived_metrics(&embedded_metrics);
        return embedded_metrics;
    }

    merge_numeric_object(&embedded_metrics, embedded_snapshot_export);
    populate_derived_metrics(&embedded_metrics);

    if (embedded_metrics.isEmpty() && result != nullptr) {
        result->warnings.push_back(
            QStringLiteral("embedded snapshot has no numeric metrics")
        );
    }

    return embedded_metrics;
}

QHash<QString, QJsonObject>
metric_hints_by_id_from_catalog(const QJsonArray& metric_catalog) {
    QHash<QString, QJsonObject> hints_by_id;
    for (const QJsonValue& entry : metric_catalog) {
        const QJsonObject metric = entry.toObject();
        const QString id = metric.value(QStringLiteral("id")).toString();
        if (!id.isEmpty()) {
            hints_by_id.insert(id, metric);
        }
    }
    return hints_by_id;
}

} // namespace parity_checker_support

using parity_checker_support::compare_metric_with_tolerance;
using parity_checker_support::extract_embedded_metrics;
using parity_checker_support::merge_numeric_object;
using parity_checker_support::merge_sample_batch_metrics;
using parity_checker_support::metric_hints_by_id_from_catalog;
using parity_checker_support::populate_derived_metrics;

monitor_parity_checker::parity_result
monitor_parity_checker::compare_embedded_snapshot_and_external_messages(
    const QJsonObject& embedded_snapshot_export,
    const QVector<QJsonObject>& external_messages, qint64 byte_tolerance
) {
    parity_result result;
    result.compared_message_count = external_messages.size();
    const numeric_metric_map embedded_metrics
        = extract_embedded_metrics(embedded_snapshot_export, &result);
    if (embedded_metrics.isEmpty()) {
        return result;
    }

    numeric_metric_map external_metrics;
    bool saw_capabilities = false;

    for (const QJsonObject& message : external_messages) {
        const QJsonObject protocol
            = message.value(QStringLiteral("protocol_v1")).toObject();
        const QString family
            = protocol.value(QStringLiteral("message_family")).toString();

        if (family == QStringLiteral("capabilities")) {
            saw_capabilities = true;
            const QJsonObject capabilities
                = message.value(QStringLiteral("capabilities")).toObject();
            const QJsonArray incoming_catalog
                = capabilities.value(QStringLiteral("metric_catalog"))
                      .toArray();
            if (incoming_catalog.isEmpty()) {
                result.warnings.push_back(
                    QStringLiteral("capabilities metric catalog is empty")
                );
                continue;
            }

            const QHash<QString, QJsonObject> incoming_by_id
                = metric_hints_by_id_from_catalog(incoming_catalog);
            const QJsonArray required_fields
                = debug_probe_core::protocol_required_metric_hint_fields_v1();
            for (auto it = incoming_by_id.cbegin(); it != incoming_by_id.cend();
                 ++it) {
                for (const QJsonValue& field_value : required_fields) {
                    const QString field = field_value.toString();
                    if (field.isEmpty()) {
                        continue;
                    }
                    if (!it.value().contains(field)
                        || it.value().value(field).toString().isEmpty()) {
                        result.warnings.push_back(
                            QStringLiteral(
                                "metric '%1' missing required hint field '%2'"
                            )
                                .arg(it.key(), field)
                        );
                    }
                }
            }

            const QJsonArray expected_catalog
                = debug_probe_core::protocol_metric_catalog_v1();
            const QHash<QString, QJsonObject> expected_by_id
                = metric_hints_by_id_from_catalog(expected_catalog);
            for (const QJsonValue& expected_value : expected_catalog) {
                const QJsonObject expected = expected_value.toObject();
                const QString id
                    = expected.value(QStringLiteral("id")).toString();
                if (id.isEmpty()) {
                    continue;
                }
                if (!incoming_by_id.contains(id)) {
                    continue;
                }
                const QString incoming_provenance
                    = incoming_by_id.value(id)
                          .value(QStringLiteral("provenance"))
                          .toString();
                const QString expected_provenance
                    = expected.value(QStringLiteral("provenance")).toString();
                if (incoming_provenance != expected_provenance) {
                    result.warnings.push_back(
                        QStringLiteral(
                            "metric '%1' provenance drift: incoming='%2' "
                            "expected='%3'"
                        )
                            .arg(id, incoming_provenance, expected_provenance)
                    );
                }
            }
            continue;
        }

        if (family == QStringLiteral("sample_batch")) {
            const QJsonArray samples
                = message.value(QStringLiteral("samples")).toArray();
            merge_sample_batch_metrics(&external_metrics, samples);
            continue;
        }

        if (family == QStringLiteral("snapshot")) {
            const QJsonObject snapshot
                = message.value(QStringLiteral("snapshot")).toObject();
            merge_numeric_object(&external_metrics, snapshot);
        }
    }

    populate_derived_metrics(&external_metrics);

    if (!saw_capabilities) {
        result.warnings.push_back(QStringLiteral(
            "external trace did not include capabilities message"
        ));
    }

    QStringList overlapping_metric_ids;
    for (auto it = embedded_metrics.cbegin(); it != embedded_metrics.cend();
         ++it) {
        if (external_metrics.contains(it.key())) {
            overlapping_metric_ids.push_back(it.key());
        }
    }
    overlapping_metric_ids.sort();

    if (overlapping_metric_ids.isEmpty()) {
        result.warnings.push_back(QStringLiteral(
            "no overlapping numeric metrics found for parity comparison"
        ));
        return result;
    }

    for (const QString& metric_id : overlapping_metric_ids) {
        compare_metric_with_tolerance(
            metric_id, embedded_metrics.value(metric_id),
            external_metrics.value(metric_id), byte_tolerance, &result
        );
    }

    return result;
}

QVector<QJsonObject> monitor_parity_checker::parse_external_history_jsonl(
    const QByteArray& jsonl_data
) {
    QVector<QJsonObject> messages;
    const QList<QByteArray> lines = jsonl_data.split('\n');
    for (const QByteArray& raw_line : lines) {
        const QByteArray line = raw_line.trimmed();
        if (line.isEmpty()) {
            continue;
        }
        const QJsonDocument document = QJsonDocument::fromJson(line);
        if (document.isObject()) {
            messages.push_back(document.object());
        }
    }
    return messages;
}

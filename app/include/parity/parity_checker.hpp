#pragma once

#include <QByteArray>
#include <QIODevice>
#include <QJsonObject>
#include <QString>
#include <QVector>

class monitor_parity_checker {
public:
    struct parity_result {
        QVector<QString> warnings;
        QVector<QString> comparison_details;
        qint64 compared_metric_count = 0;
        qint64 compared_message_count = 0;
        qint64 ignored_message_count = 0;
        QString selected_session_id;

        [[nodiscard]] bool ok() const { return warnings.isEmpty(); }
    };

    struct comparison_policy {
        qint64 byte_tolerance = 4 * 1024 * 1024;
        double duration_absolute_tolerance_ms = 1.0;
        double fractional_absolute_tolerance = 0.000001;
        double relative_tolerance = 0.01;
        bool allow_unknown_units = false;
        QString session_id;
    };

    struct history_parse_result {
        QVector<QJsonObject> messages;
        QVector<QString> warnings;
        qint64 invalid_record_count = 0;
        qint64 oversized_record_count = 0;
        qint64 unterminated_record_count = 0;
        qint64 estimated_retained_bytes = 0;
        bool message_limit_reached = false;
        bool retained_byte_limit_reached = false;
        bool multiple_sessions = false;

        [[nodiscard]] bool ok() const { return warnings.isEmpty(); }
    };

    static parity_result compare_snapshot_to_messages(
        const QJsonObject& embedded_snapshot_export,
        const QVector<QJsonObject>& external_messages,
        const comparison_policy& policy
    );

    static parity_result compare_snapshot_to_messages(
        const QJsonObject& embedded_snapshot_export,
        const QVector<QJsonObject>& external_messages,
        qint64 byte_tolerance = 4 * 1024 * 1024
    );

    static QVector<QJsonObject>
    parse_external_history_jsonl(const QByteArray& jsonl_data);

    static history_parse_result parse_external_history_jsonl(
        QIODevice* input, qsizetype maximum_record_bytes = 1024 * 1024,
        qint64 maximum_message_count = 100000,
        qint64 maximum_retained_bytes = 64 * 1024 * 1024
    );
};

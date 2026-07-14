#include "parity/parity_checker.hpp"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QTextStream>

#include <cmath>
#include <limits>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(
        QStringLiteral("monitor_parity_check")
    );
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Compare snapshot JSON objects or legacy embedded exports with "
        "monitor traces."
    ));
    parser.addHelpOption();
    parser.addVersionOption();

    const QCommandLineOption embedded_option(
        QStringList() << QStringLiteral("embedded"),
        QStringLiteral(
            "Path to a snapshot JSON object or legacy embedded snapshot export."
        ),
        QStringLiteral("path")
    );
    const QCommandLineOption external_option(
        QStringList() << QStringLiteral("external"),
        QStringLiteral("Path to monitor history JSONL."), QStringLiteral("path")
    );
    const QCommandLineOption tolerance_option(
        QStringList() << QStringLiteral("tolerance-bytes"),
        QStringLiteral("Byte tolerance for overlapping metric drift checks."),
        QStringLiteral("bytes"), QStringLiteral("4194304")
    );
    const QCommandLineOption duration_tolerance_option(
        QStringList() << QStringLiteral("tolerance-ms"),
        QStringLiteral("Absolute tolerance for duration metrics."),
        QStringLiteral("milliseconds"), QStringLiteral("1")
    );
    const QCommandLineOption fractional_tolerance_option(
        QStringList() << QStringLiteral("tolerance-fraction"),
        QStringLiteral("Absolute tolerance for ratio and percent metrics."),
        QStringLiteral("value"), QStringLiteral("0.000001")
    );
    const QCommandLineOption relative_tolerance_option(
        QStringList() << QStringLiteral("relative-tolerance"),
        QStringLiteral(
            "Relative tolerance used for duration, ratio, and percent metrics."
        ),
        QStringLiteral("fraction"), QStringLiteral("0.01")
    );
    const QCommandLineOption allow_unknown_units_option(
        QStringList() << QStringLiteral("allow-unknown-units"),
        QStringLiteral(
            "Compare unknown units with the generic absolute/relative "
            "tolerance policy."
        )
    );
    const QCommandLineOption session_option(
        QStringList() << QStringLiteral("session"),
        QStringLiteral(
            "Protocol session to analyze; defaults to the embedded session or "
            "the trace's only session. A multi-session input still fails."
        ),
        QStringLiteral("session-id")
    );
    const QCommandLineOption maximum_record_option(
        QStringList() << QStringLiteral("max-record-bytes"),
        QStringLiteral("Maximum accepted JSONL record size."),
        QStringLiteral("bytes"), QStringLiteral("1048576")
    );
    const QCommandLineOption maximum_messages_option(
        QStringList() << QStringLiteral("max-messages"),
        QStringLiteral("Maximum JSONL messages retained for comparison."),
        QStringLiteral("count"), QStringLiteral("100000")
    );
    const QCommandLineOption maximum_retained_bytes_option(
        QStringList() << QStringLiteral("max-retained-bytes"),
        QStringLiteral(
            "Maximum aggregate estimated bytes retained from parsed JSONL "
            "messages."
        ),
        QStringLiteral("bytes"), QStringLiteral("67108864")
    );
    parser.addOption(embedded_option);
    parser.addOption(external_option);
    parser.addOption(tolerance_option);
    parser.addOption(duration_tolerance_option);
    parser.addOption(fractional_tolerance_option);
    parser.addOption(relative_tolerance_option);
    parser.addOption(allow_unknown_units_option);
    parser.addOption(session_option);
    parser.addOption(maximum_record_option);
    parser.addOption(maximum_messages_option);
    parser.addOption(maximum_retained_bytes_option);
    parser.process(app);

    const QString embedded_path = parser.value(embedded_option).trimmed();
    const QString external_path = parser.value(external_option).trimmed();
    if (embedded_path.isEmpty() || external_path.isEmpty()) {
        QTextStream(stderr)
            << "Both --embedded and --external paths are required.\n";
        return 2;
    }

    QFile embedded_file(embedded_path);
    if (!embedded_file.open(QIODevice::ReadOnly)) {
        QTextStream(stderr)
            << "Unable to open embedded snapshot: " << embedded_path << "\n";
        return 2;
    }
    const QJsonDocument embedded_document
        = QJsonDocument::fromJson(embedded_file.readAll());
    if (!embedded_document.isObject()) {
        QTextStream(stderr) << "Embedded snapshot is not valid JSON object.\n";
        return 2;
    }

    QFile external_file(external_path);
    if (!external_file.open(QIODevice::ReadOnly)) {
        QTextStream(stderr)
            << "Unable to open external history: " << external_path << "\n";
        return 2;
    }
    bool tolerance_ok = false;
    const qint64 tolerance_bytes
        = parser.value(tolerance_option).toLongLong(&tolerance_ok);
    if (!tolerance_ok || tolerance_bytes < 0) {
        QTextStream(stderr)
            << "--tolerance-bytes must be a non-negative integer.\n";
        return 2;
    }

    bool duration_tolerance_ok = false;
    const double duration_tolerance_ms = parser.value(duration_tolerance_option)
                                             .toDouble(&duration_tolerance_ok);
    bool fractional_tolerance_ok = false;
    const double fractional_tolerance
        = parser.value(fractional_tolerance_option)
              .toDouble(&fractional_tolerance_ok);
    bool relative_tolerance_ok = false;
    const double relative_tolerance = parser.value(relative_tolerance_option)
                                          .toDouble(&relative_tolerance_ok);
    if (!duration_tolerance_ok || !fractional_tolerance_ok
        || !relative_tolerance_ok || duration_tolerance_ms < 0.0
        || fractional_tolerance < 0.0 || relative_tolerance < 0.0
        || !std::isfinite(duration_tolerance_ms)
        || !std::isfinite(fractional_tolerance)
        || !std::isfinite(relative_tolerance)) {
        QTextStream(
            stderr
        ) << "All non-byte tolerances must be finite, non-negative numbers.\n";
        return 2;
    }

    bool maximum_record_ok = false;
    const qint64 maximum_record_bytes
        = parser.value(maximum_record_option).toLongLong(&maximum_record_ok);
    bool maximum_messages_ok = false;
    const qint64 maximum_messages = parser.value(maximum_messages_option)
                                        .toLongLong(&maximum_messages_ok);
    bool maximum_retained_bytes_ok = false;
    const qint64 maximum_retained_bytes
        = parser.value(maximum_retained_bytes_option)
              .toLongLong(&maximum_retained_bytes_ok);
    if (!maximum_record_ok || !maximum_messages_ok || !maximum_retained_bytes_ok
        || maximum_record_bytes <= 0 || maximum_messages <= 0
        || maximum_retained_bytes <= 0
        || maximum_record_bytes > std::numeric_limits<qsizetype>::max() - 2) {
        QTextStream(stderr)
            << "--max-record-bytes, --max-messages, and --max-retained-bytes "
               "must be positive integers in range.\n";
        return 2;
    }

    const auto parsed_history
        = monitor_parity_checker::parse_external_history_jsonl(
            &external_file, static_cast<qsizetype>(maximum_record_bytes),
            maximum_messages, maximum_retained_bytes
        );
    if (parsed_history.messages.isEmpty()) {
        QTextStream(stderr)
            << "External history has no valid protocol JSON messages.\n";
        for (const QString& warning : parsed_history.warnings) {
            QTextStream(stderr) << " - " << warning << "\n";
        }
        return 2;
    }

    monitor_parity_checker::comparison_policy policy;
    policy.byte_tolerance = tolerance_bytes;
    policy.duration_absolute_tolerance_ms = duration_tolerance_ms;
    policy.fractional_absolute_tolerance = fractional_tolerance;
    policy.relative_tolerance = relative_tolerance;
    policy.allow_unknown_units = parser.isSet(allow_unknown_units_option);
    policy.session_id = parser.value(session_option).trimmed();

    monitor_parity_checker::parity_result parity
        = monitor_parity_checker::compare_snapshot_to_messages(
            embedded_document.object(), parsed_history.messages, policy
        );
    parity.warnings += parsed_history.warnings;

    QTextStream(stdout) << "Compared messages: "
                        << parity.compared_message_count
                        << "\nIgnored messages: "
                        << parity.ignored_message_count
                        << "\nSelected session: " << parity.selected_session_id
                        << "\nCompared metrics: "
                        << parity.compared_metric_count
                        << "\nEstimated retained bytes: "
                        << parsed_history.estimated_retained_bytes
                        << "\nWarnings: " << parity.warnings.size() << "\n";
    for (const QString& detail : parity.comparison_details) {
        QTextStream(stdout) << " = " << detail << "\n";
    }
    for (const QString& warning : parity.warnings) {
        QTextStream(stdout) << " - " << warning << "\n";
    }

    return parity.ok() ? 0 : 1;
}

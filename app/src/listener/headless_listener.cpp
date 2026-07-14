#include "listener/headless_listener.hpp"

#include "listener/telemetry_session.hpp"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QTimer>

#include <algorithm>
#include <functional>
#include <limits>

namespace {

constexpr qint64 default_reconnect_window_ms = 30'000;
constexpr int initial_reconnect_delay_ms = 250;
constexpr int maximum_reconnect_delay_ms = 2'000;
constexpr int sample_output_interval_ms = 250;

QString endpoint_from_parser(const QCommandLineParser& parser) {
    QString endpoint = parser.value(QStringLiteral("endpoint")).trimmed();
    if (endpoint.isEmpty()) {
        endpoint = qEnvironmentVariable("MONITOR_ENDPOINT").trimmed();
    }
    if (endpoint.isEmpty()) {
        endpoint = qEnvironmentVariable("CPPR_DEBUG_ENDPOINT").trimmed();
    }
    return endpoint;
}

QString message_family(const QJsonObject& message) {
    return message.value(QStringLiteral("protocol_v1"))
        .toObject()
        .value(QStringLiteral("message_family"))
        .toString();
}

QJsonObject numeric_summary(const QHash<QString, double>& metrics) {
    static const QStringList summary_metric_ids {
        QStringLiteral("process_memory_rss_bytes"),
        QStringLiteral("process_memory_rss_bytes_delta"),
        QStringLiteral("cache_accounted_ready_bytes"),
        QStringLiteral("widget_local_display_bytes_estimated"),
        QStringLiteral("displayed_recent_entries"),
        QStringLiteral("cached_only_ready_entries"),
        QStringLiteral("cache_ready_entries"),
        QStringLiteral("cache_ready_images"),
        QStringLiteral("cache_entries_added_interval"),
        QStringLiteral("cache_entries_removed_interval"),
        QStringLiteral("cache_bytes_added_interval"),
        QStringLiteral("cache_bytes_removed_interval"),
        QStringLiteral("raster_lifecycle_average_ms"),
        QStringLiteral("raster_lifecycle_max_ms"),
        QStringLiteral("layout_slot_count"),
        QStringLiteral("layout_required_short_px"),
        QStringLiteral("layout_device_pixel_ratio"),
        QStringLiteral("layout_cache_window_minimum_need_px"),
        QStringLiteral("layout_cache_window_maximum_need_px"),
        QStringLiteral("layout_requested_target_bucket_px"),
        QStringLiteral("layout_active_bucket_px"),
        QStringLiteral("layout_warming_bucket_px"),
        QStringLiteral("layout_cache_raster_width_px"),
        QStringLiteral("layout_cache_raster_height_px"),
    };
    QJsonObject summary;
    for (const QString& metric_id : summary_metric_ids) {
        const auto it = metrics.constFind(metric_id);
        if (it != metrics.constEnd()) {
            summary.insert(metric_id, it.value());
        }
    }
    return summary;
}

void write_json_line(QTextStream& stream, const QJsonObject& object) {
    stream << QString::fromUtf8(
        QJsonDocument(object).toJson(QJsonDocument::Compact)
    ) << '\n';
    stream.flush();
}

bool parse_nonnegative_duration(
    const QCommandLineParser& parser, const QString& option_name, qint64* value,
    QTextStream* error_stream
) {
    bool valid = false;
    const qint64 parsed = parser.value(option_name).toLongLong(&valid);
    if (!valid || parsed < 0) {
        if (error_stream != nullptr) {
            *error_stream << "--" << option_name
                          << " must be a non-negative integer.\n";
        }
        return false;
    }
    *value = parsed;
    return true;
}

} // namespace

int run_monitor_headless_listener(QCoreApplication& application) {
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Headless listener for versioned local application telemetry."
    ));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption(QCommandLineOption(
        QStringList() << QStringLiteral("listen") << QStringLiteral("headless"),
        QStringLiteral("Run without constructing a GUI application or window.")
    ));
    parser.addOption(QCommandLineOption(
        QStringList() << QStringLiteral("e") << QStringLiteral("endpoint"),
        QStringLiteral(
            "Absolute local socket path or kcuckoounter endpoint name."
        ),
        QStringLiteral("path-or-name")
    ));
    parser.addOption(QCommandLineOption(
        QStringList() << QStringLiteral("o") << QStringLiteral("output-dir"),
        QStringLiteral("Directory for bounded session history JSONL."),
        QStringLiteral("directory")
    ));
    parser.addOption(QCommandLineOption(
        QStringList() << QStringLiteral("duration-ms"),
        QStringLiteral(
            "Stop after this total runtime (zero means no runtime limit)."
        ),
        QStringLiteral("milliseconds"), QStringLiteral("0")
    ));
    parser.addOption(QCommandLineOption(
        QStringList() << QStringLiteral("reconnect-for-ms"),
        QStringLiteral(
            "Retry with bounded backoff after startup failure or target "
            "restart."
        ),
        QStringLiteral("milliseconds"),
        QString::number(default_reconnect_window_ms)
    ));
    parser.addOption(QCommandLineOption(
        QStringList() << QStringLiteral("quiet"),
        QStringLiteral("Write only lifecycle, warning, and final records.")
    ));
    parser.process(application);

    QTextStream standard_output(stdout);
    QTextStream standard_error(stderr);
    const QString requested_endpoint = endpoint_from_parser(parser);
    if (requested_endpoint.isEmpty()) {
        standard_error
            << "An endpoint is required through --endpoint, MONITOR_ENDPOINT, "
               "or CPPR_DEBUG_ENDPOINT.\n";
        return 2;
    }

    qint64 duration_ms = 0;
    qint64 reconnect_window_ms = 0;
    if (!parse_nonnegative_duration(
            parser, QStringLiteral("duration-ms"), &duration_ms, &standard_error
        )
        || !parse_nonnegative_duration(
            parser, QStringLiteral("reconnect-for-ms"), &reconnect_window_ms,
            &standard_error
        )) {
        return 2;
    }
    if (duration_ms > std::numeric_limits<int>::max()) {
        standard_error << "--duration-ms exceeds the supported timer range.\n";
        return 2;
    }

    telemetry_session session;
    const QString output_directory
        = parser.value(QStringLiteral("output-dir")).trimmed();
    if (!output_directory.isEmpty()) {
        session.set_history_output_directory(output_directory);
    }

    const bool quiet = parser.isSet(QStringLiteral("quiet"));
    bool saw_session = false;
    bool stopping = false;
    bool received_goodbye = false;
    bool fatal_protocol_error = false;
    int result_code = 0;
    int reconnect_delay_ms = initial_reconnect_delay_ms;
    qint64 session_count = 0;
    qint64 sample_batch_count = 0;
    qint64 geometry_count = 0;
    qint64 cache_decision_count = 0;
    qint64 layout_transition_count = 0;
    QHash<QString, double> latest_metrics;
    QElapsedTimer reconnect_window;
    QTimer reconnect_timer;
    reconnect_timer.setSingleShot(true);
    QTimer sample_output_timer;
    sample_output_timer.setSingleShot(true);
    sample_output_timer.setInterval(sample_output_interval_ms);
    bool sample_output_pending = false;

    std::function<void(int)> finish;
    std::function<void()> schedule_reconnect;
    finish = [&](int requested_code) {
        result_code = std::max(result_code, requested_code);
        if (stopping) {
            return;
        }
        stopping = true;
        reconnect_timer.stop();
        sample_output_timer.stop();
        QString history_error;
        if (!session.close_history(&history_error)) {
            standard_error << "History error: " << history_error << '\n';
            result_code = std::max(result_code, 4);
        }

        QJsonObject final_record;
        final_record.insert(QStringLiteral("type"), QStringLiteral("summary"));
        final_record.insert(QStringLiteral("sessions"), session_count);
        final_record.insert(
            QStringLiteral("sample_batches"), sample_batch_count
        );
        final_record.insert(QStringLiteral("geometry_records"), geometry_count);
        final_record.insert(
            QStringLiteral("cache_decisions"), cache_decision_count
        );
        final_record.insert(
            QStringLiteral("layout_transitions"), layout_transition_count
        );
        final_record.insert(
            QStringLiteral("latest_metrics"), numeric_summary(latest_metrics)
        );
        write_json_line(standard_output, final_record);
        QCoreApplication::exit(result_code);
    };

    schedule_reconnect = [&]() {
        if (stopping || received_goodbye) {
            return;
        }
        if (reconnect_window_ms == 0) {
            finish(saw_session ? result_code : std::max(result_code, 2));
            return;
        }
        if (!reconnect_window.isValid()) {
            reconnect_window.start();
            reconnect_delay_ms = initial_reconnect_delay_ms;
        }
        const qint64 remaining_ms
            = reconnect_window_ms - reconnect_window.elapsed();
        if (remaining_ms <= 0) {
            finish(saw_session ? result_code : std::max(result_code, 2));
            return;
        }

        const int delay_ms = static_cast<int>(std::min<qint64>(
            remaining_ms, static_cast<qint64>(reconnect_delay_ms)
        ));
        QJsonObject record;
        record.insert(QStringLiteral("type"), QStringLiteral("reconnecting"));
        record.insert(QStringLiteral("delay_ms"), delay_ms);
        record.insert(
            QStringLiteral("endpoint"),
            telemetry_session::normalize_endpoint_path(requested_endpoint)
        );
        write_json_line(standard_output, record);
        reconnect_timer.start(delay_ms);
        reconnect_delay_ms
            = std::min(maximum_reconnect_delay_ms, reconnect_delay_ms * 2);
    };

    QObject::connect(&reconnect_timer, &QTimer::timeout, &application, [&]() {
        if (!stopping) {
            session.connect_to_endpoint(requested_endpoint);
        }
    });
    QObject::connect(
        &sample_output_timer, &QTimer::timeout, &application, [&]() {
            if (quiet || stopping || !sample_output_pending) {
                return;
            }
            sample_output_pending = false;
            QJsonObject record;
            record.insert(QStringLiteral("type"), QStringLiteral("sample"));
            record.insert(
                QStringLiteral("metrics"), numeric_summary(latest_metrics)
            );
            write_json_line(standard_output, record);
        }
    );
    QObject::connect(
        &session, &telemetry_session::connected, &application,
        [&](const QString& connected_endpoint) {
            QJsonObject record;
            record.insert(QStringLiteral("type"), QStringLiteral("connected"));
            record.insert(QStringLiteral("endpoint"), connected_endpoint);
            write_json_line(standard_output, record);
        }
    );
    QObject::connect(
        &session, &telemetry_session::session_started, &application,
        [&](const QJsonObject& identity) {
            saw_session = true;
            ++session_count;
            reconnect_window.invalidate();
            reconnect_delay_ms = initial_reconnect_delay_ms;
            QJsonObject record;
            record.insert(
                QStringLiteral("type"), QStringLiteral("session_started")
            );
            record.insert(QStringLiteral("identity"), identity);
            record.insert(
                QStringLiteral("history_path"), session.history_path()
            );
            write_json_line(standard_output, record);
        }
    );
    QObject::connect(
        &session, &telemetry_session::history_path_changed, &application,
        [&](const QString& path) {
            if (path.isEmpty()) {
                return;
            }
            QJsonObject record;
            record.insert(
                QStringLiteral("type"), QStringLiteral("history_opened")
            );
            record.insert(QStringLiteral("history_path"), path);
            write_json_line(standard_output, record);
        }
    );
    QObject::connect(
        &session, &telemetry_session::protocol_message_received, &application,
        [&](const QJsonObject& message) {
            latest_metrics = session.latest_numeric_metrics();
            const QString family = message_family(message);
            if (family == QStringLiteral("sample_batch")) {
                ++sample_batch_count;
                if (!quiet) {
                    sample_output_pending = true;
                    if (!sample_output_timer.isActive()) {
                        sample_output_timer.start();
                    }
                }
            } else if (family == QStringLiteral("goodbye")) {
                received_goodbye = true;
                QTimer::singleShot(0, &application, [&]() {
                    finish(result_code);
                });
            }
        }
    );
    QObject::connect(
        &session, &telemetry_session::geometry_received, &application,
        [&](const QJsonObject&) { ++geometry_count; }
    );
    QObject::connect(
        &session, &telemetry_session::cache_decision_received, &application,
        [&](const QJsonObject&) { ++cache_decision_count; }
    );
    QObject::connect(
        &session, &telemetry_session::layout_transition_received, &application,
        [&](const QJsonObject&) { ++layout_transition_count; }
    );
    QObject::connect(
        &session, &telemetry_session::connection_failed, &application,
        [&](const QString& message) {
            standard_error << "Connection failed: " << message << '\n';
        }
    );
    QObject::connect(
        &session, &telemetry_session::protocol_error, &application,
        [&](const QString& code, const QString& message) {
            standard_error << "Protocol error " << code << ": " << message
                           << '\n';
            fatal_protocol_error = true;
            result_code = std::max(result_code, 3);
            QTimer::singleShot(0, &application, [&]() { finish(result_code); });
        }
    );
    QObject::connect(
        &session, &telemetry_session::history_error, &application,
        [&](const QString& message) {
            standard_error << "History error: " << message << '\n';
            result_code = std::max(result_code, 4);
        }
    );
    QObject::connect(
        &session, &telemetry_session::history_line_dropped, &application,
        [&](qint64 total_dropped) {
            standard_error << "History queue dropped records: " << total_dropped
                           << '\n';
            result_code = std::max(result_code, 4);
        }
    );
    QObject::connect(
        &session, &telemetry_session::disconnected, &application, [&]() {
            if (!stopping && !received_goodbye && !fatal_protocol_error) {
                schedule_reconnect();
            }
        }
    );

    QTimer duration_timer;
    duration_timer.setSingleShot(true);
    if (duration_ms > 0) {
        QObject::connect(
            &duration_timer, &QTimer::timeout, &application, [&]() {
                finish(saw_session ? result_code : std::max(result_code, 2));
            }
        );
        duration_timer.start(static_cast<int>(duration_ms));
    }

    session.connect_to_endpoint(requested_endpoint);
    return QCoreApplication::exec();
}

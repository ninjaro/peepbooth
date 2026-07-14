#include "listener/telemetry_session.hpp"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QLocalSocket>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>

#include <array>
#include <cmath>
#include <initializer_list>
#include <limits>

namespace {

constexpr int connection_timeout_ms = 2000;
const QString protocol_version = QStringLiteral("debug_telemetry.v1");

const std::array<QString, 10>& required_metric_hint_fields() {
    static const std::array<QString, 10> fields {
        QStringLiteral("kind"),
        QStringLiteral("provenance"),
        QStringLiteral("unit"),
        QStringLiteral("scope"),
        QStringLiteral("cardinality_semantics"),
        QStringLiteral("stability"),
        QStringLiteral("additive_semantics"),
        QStringLiteral("confidence"),
        QStringLiteral("default_display_role"),
        QStringLiteral("domain_namespace"),
    };
    return fields;
}

bool valid_semantic_token(const QString& value) {
    static const QRegularExpression token_pattern(
        QStringLiteral("^[A-Za-z0-9_.-]{1,128}$")
    );
    return token_pattern.match(value).hasMatch();
}

const QSet<QString>& supported_families() {
    static const QSet<QString> families {
        QStringLiteral("hello"),        QStringLiteral("capabilities"),
        QStringLiteral("sample_batch"), QStringLiteral("event_batch"),
        QStringLiteral("snapshot"),     QStringLiteral("marker"),
        QStringLiteral("warning"),      QStringLiteral("goodbye"),
    };
    return families;
}

const QSet<QString>& snapshot_metadata_number_fields() {
    static const QSet<QString> fields {
        QStringLiteral("collector_sequence"),
        QStringLiteral("cache_timeline_size"),
        QStringLiteral("event_timeline_size"),
        QStringLiteral("geometry_timeline_size"),
        QStringLiteral("resize_history_size"),
    };
    return fields;
}

bool require_object_array(
    const QJsonObject& message, const QString& key, qsizetype maximum_entries,
    QString* error_message
) {
    const QJsonValue value = message.value(key);
    if (!value.isArray()) {
        if (error_message != nullptr) {
            *error_message = QStringLiteral("'%1' must be an array").arg(key);
        }
        return false;
    }
    const QJsonArray entries = value.toArray();
    if (entries.size() > maximum_entries) {
        if (error_message != nullptr) {
            *error_message
                = QStringLiteral("'%1' exceeds the entry limit").arg(key);
        }
        return false;
    }
    // Keep the loop form so the failing protocol entry can set one clear error.
    // NOLINTNEXTLINE(readability-use-anyofallof)
    for (const auto& entry : entries) {
        if (!entry.isObject()) {
            if (error_message != nullptr) {
                *error_message
                    = QStringLiteral("'%1' entries must be objects").arg(key);
            }
            return false;
        }
    }
    return true;
}

bool nonempty_string(const QJsonObject& object, const QString& key) {
    return object.value(key).isString()
        && !object.value(key).toString().trimmed().isEmpty();
}

bool valid_integer(const QJsonValue& value, qint64 minimum = 0) {
    if (!value.isDouble()) {
        return false;
    }
    const double number = value.toDouble();
    return std::isfinite(number) && std::trunc(number) == number
        && number >= static_cast<double>(minimum);
}

bool valid_number(const QJsonValue& value, double minimum = 0.0) {
    if (!value.isDouble()) {
        return false;
    }
    const double number = value.toDouble();
    return std::isfinite(number) && number >= minimum;
}

bool valid_declared_count(
    const QJsonObject& message, const QString& count_key,
    qsizetype actual_count, QString* error_message
) {
    const QJsonValue count = message.value(count_key);
    if (!valid_integer(count)
        || count.toInteger(-1) != static_cast<qint64>(actual_count)) {
        *error_message
            = QStringLiteral("'%1' must exactly match the payload entry count")
                  .arg(count_key);
        return false;
    }
    return true;
}

bool valid_size_object(const QJsonValue& value, qint64 minimum = 0) {
    if (!value.isObject()) {
        return false;
    }
    const QJsonObject size = value.toObject();
    return valid_integer(size.value(QStringLiteral("width")), minimum)
        && valid_integer(size.value(QStringLiteral("height")), minimum);
}

bool require_integer_fields(
    const QJsonObject& object, const QString& event_kind,
    const std::initializer_list<QString>& fields, qint64 minimum,
    QString* error_message
) {
    // Keep the loop form so the failing field can populate error_message.
    // NOLINTNEXTLINE(readability-use-anyofallof)
    for (const QString& field : fields) {
        if (!valid_integer(object.value(field), minimum)) {
            *error_message = QStringLiteral("%1 requires integer field '%2'")
                                 .arg(event_kind, field);
            return false;
        }
    }
    return true;
}

bool require_number_fields(
    const QJsonObject& object, const QString& event_kind,
    const std::initializer_list<QString>& fields, double minimum,
    QString* error_message
) {
    // Keep the loop form so the failing field can populate error_message.
    // NOLINTNEXTLINE(readability-use-anyofallof)
    for (const QString& field : fields) {
        if (!valid_number(object.value(field), minimum)) {
            *error_message = QStringLiteral("%1 requires numeric field '%2'")
                                 .arg(event_kind, field);
            return false;
        }
    }
    return true;
}

bool valid_geometry_payload(
    const QJsonValue& value, const QString& event_kind, QString* error_message
) {
    if (!value.isObject()) {
        *error_message
            = QStringLiteral("%1 requires object field 'geometry_after_resize'")
                  .arg(event_kind);
        return false;
    }
    const QJsonObject geometry = value.toObject();
    if (!require_integer_fields(
            geometry, event_kind,
            {
                QStringLiteral("timestamp_ms"),
                QStringLiteral("slot_count"),
                QStringLiteral("visible_slot_count"),
                QStringLiteral("display_card_need_short_px"),
                QStringLiteral("active_bucket_px"),
                QStringLiteral("warming_bucket_px"),
                QStringLiteral("cache_window_minimum_need_px"),
                QStringLiteral("cache_window_maximum_need_px"),
                QStringLiteral("requested_target_bucket_px"),
                QStringLiteral("coverage_percent"),
                QStringLiteral("coverage_window_ms"),
                QStringLiteral("unique_size_buckets"),
                QStringLiteral("active_generation_id"),
                QStringLiteral("warming_generation_id"),
            },
            0, error_message
        )
        || !require_number_fields(
            geometry, event_kind, { QStringLiteral("device_pixel_ratio") },
            0.000001, error_message
        )
        || !valid_size_object(geometry.value(QStringLiteral("window_size")), -1)
        || !valid_size_object(geometry.value(QStringLiteral("layout_size")), -1)
        || !valid_size_object(
            geometry.value(QStringLiteral("display_card_size")), -1
        )
        || !valid_size_object(
            geometry.value(QStringLiteral("cache_raster_size")), -1
        )
        || !valid_size_object(
            geometry.value(QStringLiteral("preloaded_raster_size")), -1
        )
        || !valid_semantic_token(
            geometry.value(QStringLiteral("cache_decision")).toString()
        )
        || !valid_semantic_token(
            geometry.value(QStringLiteral("cache_trigger")).toString()
        )
        || !geometry.value(QStringLiteral("prewarm_in_flight")).isBool()) {
        if (error_message->isEmpty()) {
            *error_message
                = QStringLiteral(
                      "%1 has incomplete or invalid geometry_after_resize"
                )
                      .arg(event_kind);
        }
        return false;
    }
    return true;
}

bool valid_known_event(const QJsonObject& event, QString* error_message) {
    const QString kind = event.value(QStringLiteral("kind")).toString();
    if (kind == QStringLiteral("svg_cache_decision")) {
        if (!valid_integer(event.value(QStringLiteral("schema_version")))
            || event.value(QStringLiteral("schema_version")).toInteger() != 1) {
            *error_message = QStringLiteral(
                "svg_cache_decision requires schema_version 1"
            );
            return false;
        }
        if (!valid_semantic_token(
                event.value(QStringLiteral("trigger")).toString()
            )
            || !valid_semantic_token(
                event.value(QStringLiteral("decision")).toString()
            )) {
            *error_message = QStringLiteral(
                "svg_cache_decision requires token-valued trigger and decision"
            );
            return false;
        }
        if (!require_integer_fields(
                event, kind,
                {
                    QStringLiteral("timestamp_ms"),
                    QStringLiteral("required_short_px"),
                    QStringLiteral("cache_window_minimum_need_px"),
                    QStringLiteral("cache_window_maximum_need_px"),
                    QStringLiteral("requested_target_bucket_px"),
                    QStringLiteral("active_bucket_px"),
                    QStringLiteral("warming_bucket_px"),
                    QStringLiteral("slot_count"),
                },
                0, error_message
            )
            || !require_number_fields(
                event, kind, { QStringLiteral("device_pixel_ratio") }, 0.000001,
                error_message
            )) {
            return false;
        }
        if (event.value(QStringLiteral("cache_window_minimum_need_px"))
                .toInteger()
            > event.value(QStringLiteral("cache_window_maximum_need_px"))
                  .toInteger()) {
            *error_message = QStringLiteral(
                "svg_cache_decision cache window minimum exceeds maximum"
            );
            return false;
        }
    } else if (kind == QStringLiteral("layout_transition")) {
        QJsonObject payload
            = event.value(QStringLiteral("transition")).toObject();
        if (payload.isEmpty()) {
            payload = event;
        }
        if (!valid_integer(event.value(QStringLiteral("timestamp_ms")))
            || !valid_size_object(
                payload.value(QStringLiteral("old_window_size"))
            )
            || !valid_size_object(
                payload.value(QStringLiteral("new_window_size"))
            )
            || !require_integer_fields(
                payload, kind,
                {
                    QStringLiteral("collector_sequence"),
                    QStringLiteral("transition_start_timestamp_ms"),
                    QStringLiteral("transition_end_timestamp_ms"),
                    QStringLiteral("old_active_bucket_px"),
                    QStringLiteral("new_active_bucket_px"),
                    QStringLiteral("old_warming_bucket_px"),
                    QStringLiteral("new_warming_bucket_px"),
                },
                0, error_message
            )
            || !require_integer_fields(
                payload, kind,
                {
                    QStringLiteral("prewarm_completion_ms"),
                    QStringLiteral("before_process_rss_bytes_measured"),
                    QStringLiteral("after_process_rss_bytes_measured"),
                    QStringLiteral("before_cache_accounted_ready_bytes"),
                    QStringLiteral("after_cache_accounted_ready_bytes"),
                    QStringLiteral(
                        "before_widget_local_display_bytes_estimated"
                    ),
                    QStringLiteral(
                        "after_widget_local_display_bytes_estimated"
                    ),
                },
                -1, error_message
            )
            || !require_integer_fields(
                payload, kind,
                {
                    QStringLiteral(
                        "before_measured_accounted_gap_bytes_derived"
                    ),
                    QStringLiteral(
                        "after_measured_accounted_gap_bytes_derived"
                    ),
                },
                std::numeric_limits<qint64>::lowest(), error_message
            )
            || !valid_geometry_payload(
                payload.value(QStringLiteral("geometry_after_resize")), kind,
                error_message
            )) {
            if (!error_message->isEmpty()) {
                return false;
            }
            *error_message = QStringLiteral(
                "layout_transition has incomplete size, cache, or timing fields"
            );
            return false;
        }
    } else if (kind == QStringLiteral("resize_transition")) {
        const QJsonObject payload
            = event.value(QStringLiteral("transition")).toObject();
        if (!valid_integer(event.value(QStringLiteral("timestamp_ms")))
            || payload.isEmpty()
            || !valid_integer(payload.value(QStringLiteral("schema_version")))
            || payload.value(QStringLiteral("schema_version")).toInteger() != 1
            || !valid_integer(payload.value(QStringLiteral("timestamp_ms")))
            || !valid_size_object(
                payload.value(QStringLiteral("old_window_size"))
            )
            || !valid_size_object(
                payload.value(QStringLiteral("new_window_size"))
            )
            || !require_integer_fields(
                payload, kind,
                {
                    QStringLiteral("old_active_bucket_px"),
                    QStringLiteral("new_active_bucket_px"),
                    QStringLiteral("old_warming_bucket_px"),
                    QStringLiteral("new_warming_bucket_px"),
                },
                0, error_message
            )
            || !valid_geometry_payload(
                payload.value(QStringLiteral("geometry_after_resize")), kind,
                error_message
            )) {
            if (!error_message->isEmpty()) {
                return false;
            }
            *error_message
                = QStringLiteral("resize_transition payload is incomplete");
            return false;
        }
    }
    return true;
}

} // namespace

telemetry_session::telemetry_session(QObject* parent)
    : QObject(parent)
    , socket(nullptr)
    , connect_timeout_timer(this)
    , pending_record()
    , current_endpoint()
    , history_output_directory(default_history_directory())
    , current_state(connection_state::disconnected)
    , current_identity()
    , metric_catalog()
    , numeric_metrics()
    , history_writer(this)
    , history_is_enabled(true)
    , accepted_hello(false)
    , accepted_capabilities(false)
    , accepted_goodbye(false) {
    connect_timeout_timer.setSingleShot(true);
    connect_timeout_timer.setInterval(connection_timeout_ms);
    QObject::connect(
        &connect_timeout_timer, &QTimer::timeout, this,
        &telemetry_session::on_connect_timeout
    );
    QObject::connect(
        &history_writer, &monitor_history_writer::path_changed, this,
        &telemetry_session::history_path_changed
    );
    QObject::connect(
        &history_writer, &monitor_history_writer::write_error, this,
        &telemetry_session::history_error
    );
    QObject::connect(
        &history_writer, &monitor_history_writer::line_dropped, this,
        &telemetry_session::history_line_dropped
    );
}

telemetry_session::~telemetry_session() { disconnect_from_endpoint(); }

void telemetry_session::connect_to_endpoint(const QString& endpoint_path) {
    const QString normalized_endpoint = normalize_endpoint_path(endpoint_path);
    if (normalized_endpoint.isEmpty()) {
        emit connection_failed(QStringLiteral("endpoint is empty"));
        return;
    }

    disconnect_from_endpoint();
    current_endpoint = normalized_endpoint;
    socket = new QLocalSocket(this);
    socket->setReadBufferSize(maximum_record_bytes * 2);
    QObject::connect(
        socket, &QLocalSocket::connected, this,
        &telemetry_session::on_socket_connected
    );
    QObject::connect(
        socket, &QLocalSocket::disconnected, this,
        &telemetry_session::on_socket_disconnected
    );
    QObject::connect(
        socket, &QLocalSocket::readyRead, this,
        &telemetry_session::on_socket_ready_read
    );
    QObject::connect(
        socket, &QLocalSocket::errorOccurred, this,
        &telemetry_session::on_socket_error
    );

    current_state = connection_state::connecting;
    emit connection_state_changed(current_state);
    socket->connectToServer(current_endpoint);
    connect_timeout_timer.start();
}

void telemetry_session::disconnect_from_endpoint() {
    const bool was_active = socket != nullptr
        || current_state != connection_state::disconnected || accepted_hello;
    connect_timeout_timer.stop();
    cleanup_socket();
    reset_session_state();
    current_endpoint.clear();
    current_state = connection_state::disconnected;
    emit connection_state_changed(current_state);
    if (was_active) {
        emit disconnected();
    }
}

void telemetry_session::set_history_output_directory(const QString& directory) {
    if (!accepted_hello) {
        history_output_directory = directory.trimmed();
    }
}

void telemetry_session::set_history_enabled(bool enabled) {
    if (accepted_hello) {
        return;
    }
    history_is_enabled = enabled;
}

telemetry_session::connection_state telemetry_session::state() const {
    return current_state;
}

bool telemetry_session::has_active_session() const { return accepted_hello; }

QString telemetry_session::endpoint_path() const { return current_endpoint; }

QString telemetry_session::history_path() const {
    return history_writer.current_path();
}

QJsonObject telemetry_session::identity() const { return current_identity; }

QHash<QString, double> telemetry_session::latest_numeric_metrics() const {
    return numeric_metrics;
}

bool telemetry_session::flush_history_batch(QString* error_message) {
    return history_writer.flush_one_batch(error_message);
}

bool telemetry_session::close_history(QString* error_message) {
    return history_writer.close_session(error_message);
}

QString telemetry_session::normalize_endpoint_path(const QString& endpoint) {
    QString normalized = endpoint.trimmed();
    if (normalized.isEmpty() || QDir::isAbsolutePath(normalized)) {
        return normalized;
    }

    normalized.replace(
        QRegularExpression(QStringLiteral("[^A-Za-z0-9_.-]+")),
        QStringLiteral("_")
    );
    constexpr qsizetype maximum_endpoint_name_length = 24;
    if (normalized.size() > maximum_endpoint_name_length) {
        normalized
            = normalized.left(12) + QLatin1Char('_') + normalized.right(11);
    }
    if (normalized.isEmpty()) {
        return {};
    }
    return QDir::temp().filePath(QStringLiteral("%1.sock").arg(normalized));
}

void telemetry_session::on_socket_connected() {
    connect_timeout_timer.stop();
    current_state = connection_state::connected;
    emit connection_state_changed(current_state);
    emit connected(current_endpoint);
}

void telemetry_session::on_socket_disconnected() {
    connect_timeout_timer.stop();
    if (!pending_record.trimmed().isEmpty()) {
        emit protocol_error(
            QStringLiteral("truncated_record"),
            QStringLiteral(
                "connection closed with an unterminated telemetry record"
            )
        );
    }
    cleanup_socket();
    reset_session_state();
    current_endpoint.clear();
    current_state = connection_state::disconnected;
    emit connection_state_changed(current_state);
    emit disconnected();
}

void telemetry_session::on_socket_ready_read() {
    if (socket != nullptr) {
        consume_bytes(socket->readAll());
    }
}

void telemetry_session::on_socket_error() {
    if (socket == nullptr) {
        return;
    }
    const QString message = socket->errorString();
    if (socket->error() == QLocalSocket::PeerClosedError) {
        return;
    }
    if (current_state == connection_state::connecting) {
        emit connection_failed(message);
    } else {
        emit protocol_error(QStringLiteral("socket_error"), message);
    }
    disconnect_from_endpoint();
}

void telemetry_session::on_connect_timeout() {
    if (current_state != connection_state::connecting) {
        return;
    }
    emit connection_failed(QStringLiteral("connection timed out after %1 ms")
                               .arg(connection_timeout_ms));
    disconnect_from_endpoint();
}

void telemetry_session::consume_bytes(const QByteArray& bytes) {
    qsizetype offset = 0;
    while (offset < bytes.size()) {
        const qsizetype newline = bytes.indexOf('\n', offset);
        const qsizetype end = newline < 0 ? bytes.size() : newline;
        const qsizetype part_size = end - offset;
        if (part_size > maximum_record_bytes
            || pending_record.size() > maximum_record_bytes - part_size) {
            reject_protocol(
                QStringLiteral("record_too_large"),
                QStringLiteral("telemetry record exceeds %1 bytes")
                    .arg(maximum_record_bytes)
            );
            return;
        }
        pending_record.append(bytes.constData() + offset, part_size);
        if (newline < 0) {
            return;
        }

        const QByteArray record = pending_record.trimmed();
        pending_record.clear();
        offset = newline + 1;
        if (!record.isEmpty() && !process_record(record)) {
            return;
        }
    }
}

bool telemetry_session::process_record(const QByteArray& compact_json_line) {
    QJsonParseError parse_error;
    const QJsonDocument document
        = QJsonDocument::fromJson(compact_json_line, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        reject_protocol(
            QStringLiteral("invalid_json"),
            QStringLiteral("invalid JSON telemetry record: %1")
                .arg(parse_error.errorString())
        );
        return false;
    }

    const QJsonObject message = document.object();
    QString family;
    QJsonObject incoming_identity;
    QString error_code;
    QString error_message;
    if (!validate_envelope(
            message, &family, &incoming_identity, &error_code, &error_message
        )
        || !validate_payload(family, message, &error_code, &error_message)) {
        reject_protocol(error_code, error_message);
        return false;
    }

    if (!accepted_hello) {
        if (family != QStringLiteral("hello")) {
            reject_protocol(
                QStringLiteral("hello_required"),
                QStringLiteral("the first telemetry record must be hello")
            );
            return false;
        }
        if (!accept_hello(incoming_identity, compact_json_line)) {
            return false;
        }
    } else {
        if (family == QStringLiteral("hello")) {
            reject_protocol(
                QStringLiteral("duplicate_hello"),
                QStringLiteral("a session may contain only one hello record")
            );
            return false;
        }
        if (!identities_match(current_identity, incoming_identity)) {
            reject_protocol(
                QStringLiteral("identity_changed"),
                QStringLiteral("telemetry identity changed within a session")
            );
            return false;
        }
        if (accepted_goodbye) {
            reject_protocol(
                QStringLiteral("record_after_goodbye"),
                QStringLiteral("no telemetry records are allowed after goodbye")
            );
            return false;
        }
        if (!accepted_capabilities
            && family != QStringLiteral("capabilities")) {
            reject_protocol(
                QStringLiteral("capabilities_required"),
                QStringLiteral(
                    "capabilities must immediately follow the hello record"
                )
            );
            return false;
        }
        if (accepted_capabilities && family == QStringLiteral("capabilities")) {
            reject_protocol(
                QStringLiteral("duplicate_capabilities"),
                QStringLiteral(
                    "a session may contain only one capabilities record"
                )
            );
            return false;
        }
        if (!validate_catalog_references(
                family, message, &error_code, &error_message
            )) {
            reject_protocol(error_code, error_message);
            return false;
        }
        if (history_is_enabled) {
            history_writer.enqueue_line(compact_json_line);
        }
    }

    update_model_and_emit(family, message);
    if (family == QStringLiteral("capabilities")) {
        accepted_capabilities = true;
    } else if (family == QStringLiteral("goodbye")) {
        accepted_goodbye = true;
    }
    emit protocol_message_received(message);
    return true;
}

bool telemetry_session::validate_envelope(
    const QJsonObject& message, QString* family, QJsonObject* identity,
    QString* error_code, QString* error_message
) {
    const QJsonValue protocol_value
        = message.value(QStringLiteral("protocol_v1"));
    if (!protocol_value.isObject()) {
        *error_code = QStringLiteral("missing_envelope");
        *error_message = QStringLiteral("protocol_v1 must be an object");
        return false;
    }

    const QJsonObject protocol = protocol_value.toObject();
    const QString incoming_version
        = protocol.value(QStringLiteral("version")).toString();
    if (incoming_version != protocol_version) {
        *error_code = QStringLiteral("unsupported_version");
        *error_message = QStringLiteral("unsupported protocol version '%1'")
                             .arg(incoming_version);
        return false;
    }

    *family = protocol.value(QStringLiteral("message_family")).toString();
    if (!supported_families().contains(*family)) {
        *error_code = QStringLiteral("unknown_family");
        *error_message
            = QStringLiteral("unsupported message family '%1'").arg(*family);
        return false;
    }

    const QJsonValue identity_value
        = protocol.value(QStringLiteral("identity"));
    if (!identity_value.isObject()) {
        *error_code = QStringLiteral("invalid_identity");
        *error_message = QStringLiteral("identity must be an object");
        return false;
    }
    *identity = identity_value.toObject();
    const std::array<QString, 5> string_fields {
        QStringLiteral("app"),
        QStringLiteral("session"),
        QStringLiteral("build"),
        QStringLiteral("protocol_version"),
        QStringLiteral("instrumentation_mode"),
    };
    for (const QString& field : string_fields) {
        if (!nonempty_string(*identity, field)) {
            *error_code = QStringLiteral("invalid_identity");
            *error_message
                = QStringLiteral("identity field '%1' is required").arg(field);
            return false;
        }
    }
    if (identity->value(QStringLiteral("protocol_version")).toString()
        != protocol_version) {
        *error_code = QStringLiteral("identity_version_mismatch");
        *error_message
            = QStringLiteral("identity protocol version does not match");
        return false;
    }
    const QJsonValue pid = identity->value(QStringLiteral("pid"));
    if (!pid.isDouble() || pid.toInteger(-1) <= 0) {
        *error_code = QStringLiteral("invalid_identity");
        *error_message = QStringLiteral("identity pid must be positive");
        return false;
    }
    const QJsonValue debug_flags_value
        = identity->value(QStringLiteral("debug_flags"));
    if (!debug_flags_value.isArray()) {
        *error_code = QStringLiteral("invalid_identity");
        *error_message
            = QStringLiteral("identity debug_flags must be an array");
        return false;
    }
    const QJsonArray debug_flags = debug_flags_value.toArray();
    if (debug_flags.size() > maximum_debug_flags) {
        *error_code = QStringLiteral("invalid_identity");
        *error_message = QStringLiteral("identity has too many debug_flags");
        return false;
    }
    QSet<QString> seen_debug_flags;
    for (const auto& value : debug_flags) {
        const QString flag = value.toString();
        if (!value.isString() || !valid_semantic_token(flag)
            || seen_debug_flags.contains(flag)) {
            *error_code = QStringLiteral("invalid_identity");
            *error_message = QStringLiteral(
                "identity debug_flags must be unique semantic tokens"
            );
            return false;
        }
        seen_debug_flags.insert(flag);
    }
    return true;
}

bool telemetry_session::validate_payload(
    const QString& family, const QJsonObject& message, QString* error_code,
    QString* error_message
) {
    *error_code = QStringLiteral("invalid_payload");
    if (family == QStringLiteral("hello")) {
        return true;
    }
    if (family == QStringLiteral("capabilities")) {
        const QJsonValue capabilities_value
            = message.value(QStringLiteral("capabilities"));
        if (!capabilities_value.isObject()) {
            *error_message = QStringLiteral("capabilities must be an object");
            return false;
        }
        const QJsonValue catalog_value = capabilities_value.toObject().value(
            QStringLiteral("metric_catalog")
        );
        if (!catalog_value.isArray()
            || catalog_value.toArray().size() > maximum_catalog_entries) {
            *error_message
                = QStringLiteral("metric catalog is missing or too large");
            return false;
        }
        QSet<QString> seen_ids;
        for (const auto& value : catalog_value.toArray()) {
            if (!value.isObject()) {
                *error_message
                    = QStringLiteral("metric catalog entries must be objects");
                return false;
            }
            const QJsonObject metric = value.toObject();
            const QString id
                = metric.value(QStringLiteral("id")).toString().trimmed();
            if (!valid_semantic_token(id) || seen_ids.contains(id)) {
                *error_message = QStringLiteral(
                    "metric catalog IDs must be unique semantic tokens"
                );
                return false;
            }
            for (const QString& field : required_metric_hint_fields()) {
                const QString field_value
                    = metric.value(field).toString().trimmed();
                if (!valid_semantic_token(field_value)) {
                    *error_message = QStringLiteral(
                                         "metric '%1' has invalid or missing "
                                         "hint field '%2'"
                    )
                                         .arg(id, field);
                    return false;
                }
            }
            seen_ids.insert(id);
        }
        return true;
    }
    if (family == QStringLiteral("sample_batch")) {
        if (!require_object_array(
                message, QStringLiteral("samples"), maximum_batch_entries,
                error_message
            )) {
            return false;
        }
        const QJsonArray samples
            = message.value(QStringLiteral("samples")).toArray();
        if (!valid_declared_count(
                message, QStringLiteral("sample_count"), samples.size(),
                error_message
            )) {
            return false;
        }
        // Preserve the direct validation/error path for each wire entry.
        // NOLINTNEXTLINE(readability-use-anyofallof)
        for (const auto& value : samples) {
            const QJsonObject sample = value.toObject();
            if (!valid_semantic_token(
                    sample.value(QStringLiteral("metric_id")).toString()
                )
                || !sample.value(QStringLiteral("value")).isDouble()
                || !sample.value(QStringLiteral("metric_hint")).isObject()) {
                *error_message = QStringLiteral(
                    "sample entries require metric_id, numeric value, and "
                    "metric_hint"
                );
                return false;
            }
        }
        return true;
    }
    if (family == QStringLiteral("event_batch")) {
        if (!require_object_array(
                message, QStringLiteral("events"), maximum_batch_entries,
                error_message
            )) {
            return false;
        }
        const QJsonArray events
            = message.value(QStringLiteral("events")).toArray();
        if (!valid_declared_count(
                message, QStringLiteral("event_count"), events.size(),
                error_message
            )) {
            return false;
        }
        // Preserve the direct validation/error path for each wire entry.
        // NOLINTNEXTLINE(readability-use-anyofallof)
        for (const auto& value : events) {
            const QJsonObject event = value.toObject();
            if (!valid_semantic_token(
                    event.value(QStringLiteral("kind")).toString()
                )) {
                *error_message = QStringLiteral(
                    "event entries require a token-valued kind"
                );
                return false;
            }
            if (!valid_known_event(event, error_message)) {
                return false;
            }
        }
        return true;
    }
    if (family == QStringLiteral("snapshot")) {
        if (!message.value(QStringLiteral("snapshot")).isObject()) {
            *error_message = QStringLiteral("snapshot must be an object");
            return false;
        }
        return true;
    }
    if (family == QStringLiteral("marker")) {
        if (!message.value(QStringLiteral("label")).isString()) {
            *error_message = QStringLiteral("marker label must be a string");
            return false;
        }
        return true;
    }
    if (family == QStringLiteral("warning")) {
        if (!message.value(QStringLiteral("warning_code")).isString()
            || !message.value(QStringLiteral("warning_message")).isString()) {
            *error_message
                = QStringLiteral("warning code and message are required");
            return false;
        }
        return true;
    }
    return family == QStringLiteral("goodbye");
}

bool telemetry_session::accept_hello(
    const QJsonObject& identity, const QByteArray& compact_json_line
) {
    current_identity = identity;
    accepted_hello = true;
    if (history_is_enabled) {
        QString error_message;
        if (!history_writer.start_session(
                history_output_directory,
                identity.value(QStringLiteral("session")).toString(),
                compact_json_line, &error_message
            )) {
            emit history_error(error_message);
        }
    }
    emit session_started(current_identity);
    return true;
}

bool telemetry_session::validate_catalog_references(
    const QString& family, const QJsonObject& message, QString* error_code,
    QString* error_message
) const {
    if (family == QStringLiteral("capabilities")) {
        return true;
    }

    auto reject_unknown_metric = [&](const QString& metric_id) {
        if (metric_catalog.contains(metric_id)) {
            return false;
        }
        *error_code = QStringLiteral("unknown_metric_id");
        *error_message
            = QStringLiteral("metric '%1' is absent from the session catalog")
                  .arg(metric_id);
        return true;
    };

    if (family == QStringLiteral("sample_batch")) {
        const QJsonArray samples
            = message.value(QStringLiteral("samples")).toArray();
        for (const auto& value : samples) {
            const QJsonObject sample = value.toObject();
            const QString metric_id
                = sample.value(QStringLiteral("metric_id")).toString();
            if (reject_unknown_metric(metric_id)) {
                return false;
            }
            const QJsonObject expected_hint = metric_catalog.value(metric_id);
            const QJsonObject sample_hint
                = sample.value(QStringLiteral("metric_hint")).toObject();
            if (sample_hint.value(QStringLiteral("id")).toString()
                != metric_id) {
                *error_code = QStringLiteral("metric_hint_mismatch");
                *error_message = QStringLiteral(
                                     "sample hint ID does not match metric '%1'"
                )
                                     .arg(metric_id);
                return false;
            }
            for (const QString& field : required_metric_hint_fields()) {
                if (sample_hint.value(field) != expected_hint.value(field)) {
                    *error_code = QStringLiteral("metric_hint_mismatch");
                    *error_message = QStringLiteral(
                                         "sample hint field '%1' differs from "
                                         "catalog metric '%2'"
                    )
                                         .arg(field, metric_id);
                    return false;
                }
            }
        }
    } else if (family == QStringLiteral("snapshot")) {
        const QJsonObject snapshot
            = message.value(QStringLiteral("snapshot")).toObject();
        for (auto it = snapshot.constBegin(); it != snapshot.constEnd(); ++it) {
            if (it.value().isDouble()
                && !snapshot_metadata_number_fields().contains(it.key())
                && reject_unknown_metric(it.key())) {
                return false;
            }
        }
    }
    return true;
}

void telemetry_session::update_model_and_emit(
    const QString& family, const QJsonObject& message
) {
    if (family == QStringLiteral("capabilities")) {
        metric_catalog.clear();
        const QJsonArray catalog = message.value(QStringLiteral("capabilities"))
                                       .toObject()
                                       .value(QStringLiteral("metric_catalog"))
                                       .toArray();
        for (const auto& value : catalog) {
            const QJsonObject metric = value.toObject();
            metric_catalog.insert(
                metric.value(QStringLiteral("id")).toString(), metric
            );
        }
        return;
    }
    if (family == QStringLiteral("sample_batch")) {
        const QJsonArray samples
            = message.value(QStringLiteral("samples")).toArray();
        for (const auto& value : samples) {
            const QJsonObject sample = value.toObject();
            numeric_metrics.insert(
                sample.value(QStringLiteral("metric_id")).toString(),
                sample.value(QStringLiteral("value")).toDouble()
            );
        }
        const QJsonObject geometry
            = message.value(QStringLiteral("geometry")).toObject();
        if (!geometry.isEmpty()) {
            emit geometry_received(geometry);
        }
        return;
    }
    if (family == QStringLiteral("snapshot")) {
        const QJsonObject snapshot
            = message.value(QStringLiteral("snapshot")).toObject();
        for (auto it = snapshot.constBegin(); it != snapshot.constEnd(); ++it) {
            if (it.value().isDouble() && metric_catalog.contains(it.key())) {
                numeric_metrics.insert(it.key(), it.value().toDouble());
            }
        }
        const QJsonObject geometry
            = snapshot.value(QStringLiteral("geometry")).toObject();
        if (!geometry.isEmpty()) {
            emit geometry_received(geometry);
        }
        return;
    }
    if (family != QStringLiteral("event_batch")) {
        return;
    }

    const QJsonArray events = message.value(QStringLiteral("events")).toArray();
    for (const auto& value : events) {
        const QJsonObject event = value.toObject();
        const QString kind = event.value(QStringLiteral("kind")).toString();
        if (kind == QStringLiteral("svg_cache_decision")) {
            emit cache_decision_received(event);
        } else if (
            kind == QStringLiteral("layout_transition")
            || kind == QStringLiteral("resize_transition")
        ) {
            emit layout_transition_received(event);
        }
        const QJsonObject geometry
            = event.value(QStringLiteral("geometry")).toObject();
        if (!geometry.isEmpty()) {
            emit geometry_received(geometry);
        }
    }
}

void telemetry_session::reject_protocol(
    const QString& code, const QString& message
) {
    emit protocol_error(code, message);
    disconnect_from_endpoint();
}

void telemetry_session::cleanup_socket() {
    if (socket == nullptr) {
        return;
    }
    socket->disconnect(this);
    socket->close();
    socket->deleteLater();
    socket = nullptr;
}

void telemetry_session::reset_session_state() {
    const bool had_session = accepted_hello || !current_identity.isEmpty()
        || !metric_catalog.isEmpty() || !numeric_metrics.isEmpty();
    pending_record.clear();
    current_identity = QJsonObject();
    metric_catalog.clear();
    numeric_metrics.clear();
    accepted_hello = false;
    accepted_capabilities = false;
    accepted_goodbye = false;
    if (had_session) {
        emit session_reset();
    }
    // Close after the per-session UI reset so any bounded-drain error remains
    // visible as an operational warning instead of being immediately erased.
    history_writer.close_session();
}

bool telemetry_session::identities_match(
    const QJsonObject& expected, const QJsonObject& incoming
) {
    const std::array<QString, 5> stable_fields {
        QStringLiteral("app"),
        QStringLiteral("pid"),
        QStringLiteral("session"),
        QStringLiteral("build"),
        QStringLiteral("protocol_version"),
    };
    // NOLINTNEXTLINE(readability-use-anyofallof)
    for (const QString& field : stable_fields) {
        if (expected.value(field) != incoming.value(field)) {
            return false;
        }
    }
    return true;
}

QString telemetry_session::default_history_directory() {
    QString base_directory
        = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base_directory.isEmpty()) {
        base_directory
            = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    }
    if (base_directory.isEmpty()) {
        base_directory = QDir::tempPath();
    }
    return QDir(base_directory).filePath(QStringLiteral("monitor_history"));
}

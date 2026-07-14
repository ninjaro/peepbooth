#include "parity/parity_checker.hpp"

#include "telemetry/debug_probe_core.hpp"

#include <QBuffer>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>
#include <QStringList>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <limits>

using numeric_metric_map = QHash<QString, double>;
using metric_catalog_map = QHash<QString, QJsonObject>;

namespace parity_checker_support {

struct protocol_identity_view {
    QString app;
    qint64 pid = -1;
    QString session;
    QString build;
    QString protocol_version;
    QString instrumentation_mode;
};

void append_warning_once(
    monitor_parity_checker::parity_result* result, const QString& warning
) {
    if (result == nullptr || result->warnings.contains(warning)) {
        return;
    }
    result->warnings.push_back(warning);
}

protocol_identity_view identity_from_message(const QJsonObject& message) {
    const QJsonObject identity = message.value(QStringLiteral("protocol_v1"))
                                     .toObject()
                                     .value(QStringLiteral("identity"))
                                     .toObject();
    return protocol_identity_view {
        .app = identity.value(QStringLiteral("app")).toString(),
        .pid = identity.value(QStringLiteral("pid")).toInteger(-1),
        .session = identity.value(QStringLiteral("session")).toString(),
        .build = identity.value(QStringLiteral("build")).toString(),
        .protocol_version
        = identity.value(QStringLiteral("protocol_version")).toString(),
        .instrumentation_mode
        = identity.value(QStringLiteral("instrumentation_mode")).toString(),
    };
}

bool array_contains_string(const QJsonArray& values, const QString& expected) {
    return std::any_of(
        values.cbegin(), values.cend(), [&expected](const QJsonValue& value) {
            return value.isString() && value.toString() == expected;
        }
    );
}

bool nonempty_string(const QJsonObject& object, const QString& key) {
    const QJsonValue value = object.value(key);
    return value.isString() && !value.toString().trimmed().isEmpty();
}

QString validate_protocol_history_message(
    const QJsonObject& message, protocol_identity_view* identity_output
) {
    const QJsonValue protocol_value
        = message.value(QStringLiteral("protocol_v1"));
    if (!protocol_value.isObject()) {
        return QStringLiteral("protocol_v1 must be an object");
    }

    const QJsonObject protocol = protocol_value.toObject();
    const QString expected_version
        = debug_probe_core::protocol_version_string();
    if (protocol.value(QStringLiteral("version")).toString()
        != expected_version) {
        return QStringLiteral("protocol version is missing or unsupported");
    }

    const QString family
        = protocol.value(QStringLiteral("message_family")).toString();
    if (!array_contains_string(
            debug_probe_core::protocol_message_families_v1(), family
        )) {
        return QStringLiteral("message family is missing or unsupported");
    }

    const QJsonValue identity_value
        = protocol.value(QStringLiteral("identity"));
    if (!identity_value.isObject()) {
        return QStringLiteral("protocol identity must be an object");
    }
    const QJsonObject identity = identity_value.toObject();
    const QStringList required_string_fields {
        QStringLiteral("app"),
        QStringLiteral("session"),
        QStringLiteral("build"),
        QStringLiteral("protocol_version"),
        QStringLiteral("instrumentation_mode"),
    };
    for (const QString& field : required_string_fields) {
        if (!nonempty_string(identity, field)) {
            return QStringLiteral("identity field '%1' is required").arg(field);
        }
    }
    if (identity.value(QStringLiteral("protocol_version")).toString()
        != expected_version) {
        return QStringLiteral("identity protocol version does not match");
    }
    if (!identity.value(QStringLiteral("pid")).isDouble()
        || identity.value(QStringLiteral("pid")).toInteger(-1) <= 0) {
        return QStringLiteral("identity pid must be positive");
    }
    if (!identity.value(QStringLiteral("debug_flags")).isArray()) {
        return QStringLiteral("identity debug_flags must be an array");
    }

    constexpr qsizetype maximum_catalog_entries = 1024;
    constexpr qsizetype maximum_batch_entries = 4096;
    if (family == QStringLiteral("capabilities")) {
        const QJsonValue capabilities
            = message.value(QStringLiteral("capabilities"));
        if (!capabilities.isObject()) {
            return QStringLiteral("capabilities must be an object");
        }
        const QJsonValue catalog
            = capabilities.toObject().value(QStringLiteral("metric_catalog"));
        if (!catalog.isArray()
            || catalog.toArray().size() > maximum_catalog_entries) {
            return QStringLiteral("metric catalog is missing or too large");
        }
        QSet<QString> metric_ids;
        for (const auto& value : catalog.toArray()) {
            const QString metric_id = value.toObject()
                                          .value(QStringLiteral("id"))
                                          .toString()
                                          .trimmed();
            if (metric_id.isEmpty() || metric_ids.contains(metric_id)) {
                return QStringLiteral("metric catalog IDs must be unique");
            }
            metric_ids.insert(metric_id);
        }
    } else if (family == QStringLiteral("sample_batch")) {
        const QJsonValue samples = message.value(QStringLiteral("samples"));
        if (!samples.isArray()
            || samples.toArray().size() > maximum_batch_entries) {
            return QStringLiteral("samples must be a bounded array");
        }
        for (const auto& value : samples.toArray()) {
            const QJsonObject sample = value.toObject();
            const double numeric_value
                = sample.value(QStringLiteral("value")).toDouble();
            if (!nonempty_string(sample, QStringLiteral("metric_id"))
                || !sample.value(QStringLiteral("value")).isDouble()
                || !std::isfinite(numeric_value)) {
                return QStringLiteral(
                    "sample entries require metric_id and a finite number"
                );
            }
        }
    } else if (family == QStringLiteral("event_batch")) {
        const QJsonValue events = message.value(QStringLiteral("events"));
        const QJsonArray event_array = events.toArray();
        if (!events.isArray() || event_array.size() > maximum_batch_entries
            || std::any_of(
                event_array.cbegin(), event_array.cend(),
                [](const QJsonValue& value) { return !value.isObject(); }
            )) {
            return QStringLiteral("events must be a bounded object array");
        }
    } else if (family == QStringLiteral("snapshot")) {
        if (!message.value(QStringLiteral("snapshot")).isObject()) {
            return QStringLiteral("snapshot must be an object");
        }
    } else if (family == QStringLiteral("marker")) {
        if (!message.value(QStringLiteral("label")).isString()) {
            return QStringLiteral("marker label must be a string");
        }
    } else if (family == QStringLiteral("warning")) {
        if (!message.value(QStringLiteral("warning_code")).isString()
            || !message.value(QStringLiteral("warning_message")).isString()) {
            return QStringLiteral("warning code and message are required");
        }
    }

    if (identity_output != nullptr) {
        *identity_output = identity_from_message(message);
    }
    return {};
}

bool identities_are_consistent(
    const protocol_identity_view& first, const protocol_identity_view& second
) {
    return first.app == second.app && first.pid == second.pid
        && first.session == second.session && first.build == second.build
        && first.protocol_version == second.protocol_version
        && first.instrumentation_mode == second.instrumentation_mode;
}

QString message_family(const QJsonObject& message) {
    return message.value(QStringLiteral("protocol_v1"))
        .toObject()
        .value(QStringLiteral("message_family"))
        .toString();
}

metric_catalog_map hints_by_id_from_catalog(
    const QJsonArray& metric_catalog,
    monitor_parity_checker::parity_result* result = nullptr
) {
    metric_catalog_map hints_by_id;
    for (const auto& entry_value : metric_catalog) {
        const QJsonObject metric = entry_value.toObject();
        const QString id = metric.value(QStringLiteral("id")).toString();
        if (id.isEmpty()) {
            append_warning_once(
                result, QStringLiteral("metric catalog contains an empty id")
            );
            continue;
        }
        if (hints_by_id.contains(id)) {
            append_warning_once(
                result,
                QStringLiteral("metric catalog contains duplicate id '%1'")
                    .arg(id)
            );
            continue;
        }
        hints_by_id.insert(id, metric);
    }
    return hints_by_id;
}

QJsonArray embedded_metric_catalog(const QJsonObject& embedded) {
    return embedded.value(QStringLiteral("protocol_v1"))
        .toObject()
        .value(QStringLiteral("capabilities"))
        .toObject()
        .value(QStringLiteral("metric_catalog"))
        .toArray();
}

void merge_numeric_object(
    numeric_metric_map* target, const QJsonObject& object
) {
    if (target == nullptr) {
        return;
    }
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (it.value().isDouble()) {
            target->insert(it.key(), it.value().toDouble());
        }
    }
}

void insert_alias_if_present(
    numeric_metric_map* metrics, const QString& source,
    const QString& destination
) {
    if (metrics == nullptr || metrics->contains(destination)
        || !metrics->contains(source)) {
        return;
    }
    metrics->insert(destination, metrics->value(source));
}

void populate_metric_aliases(numeric_metric_map* metrics) {
    insert_alias_if_present(
        metrics, QStringLiteral("ready_entries"),
        QStringLiteral("cache_ready_entries")
    );
    insert_alias_if_present(
        metrics, QStringLiteral("ready_images"),
        QStringLiteral("cache_ready_images")
    );
    insert_alias_if_present(
        metrics, QStringLiteral("in_flight_families"),
        QStringLiteral("cache_in_flight_families")
    );
    insert_alias_if_present(
        metrics, QStringLiteral("pending_families"),
        QStringLiteral("cache_pending_families")
    );
    insert_alias_if_present(
        metrics, QStringLiteral("raster_timing_samples"),
        QStringLiteral("raster_lifecycle_completed_samples")
    );
    insert_alias_if_present(
        metrics, QStringLiteral("raster_timing_avg_ms"),
        QStringLiteral("raster_lifecycle_average_ms")
    );
    insert_alias_if_present(
        metrics, QStringLiteral("raster_timing_max_ms"),
        QStringLiteral("raster_lifecycle_max_ms")
    );
}

void populate_derived_metrics(numeric_metric_map* target) {
    if (target == nullptr) {
        return;
    }
    const QString rss = QStringLiteral("process_memory_rss_bytes");
    const QString cache = QStringLiteral("cache_accounted_ready_bytes");
    const QString gap = QStringLiteral("measured_accounted_gap_bytes_derived");
    const QString ratio
        = QStringLiteral("accounted_to_measured_ratio_percent_derived");
    if (!target->contains(gap) && target->contains(rss)
        && target->contains(cache)) {
        target->insert(gap, target->value(rss) - target->value(cache));
    }
    if (!target->contains(ratio) && target->contains(rss)
        && target->contains(cache) && target->value(rss) > 0.0) {
        target->insert(
            ratio, (target->value(cache) * 100.0) / target->value(rss)
        );
    }
}

numeric_metric_map extract_embedded_metrics(
    const QJsonObject& embedded_snapshot_export,
    monitor_parity_checker::parity_result* result
) {
    numeric_metric_map metrics;
    const QJsonArray cache_timeline
        = embedded_snapshot_export.value(QStringLiteral("cache_timeline"))
              .toArray();
    if (!cache_timeline.isEmpty()) {
        merge_numeric_object(
            &metrics, cache_timeline.at(cache_timeline.size() - 1).toObject()
        );
    } else {
        const QJsonObject nested_snapshot
            = embedded_snapshot_export.value(QStringLiteral("snapshot"))
                  .toObject();
        merge_numeric_object(
            &metrics,
            nested_snapshot.isEmpty() ? embedded_snapshot_export
                                      : nested_snapshot
        );
    }

    const QJsonArray geometry_timeline
        = embedded_snapshot_export.value(QStringLiteral("geometry_timeline"))
              .toArray();
    if (!geometry_timeline.isEmpty()) {
        const QJsonObject geometry
            = geometry_timeline.at(geometry_timeline.size() - 1).toObject();
        const QHash<QString, QString> geometry_aliases {
            { QStringLiteral("slot_count"),
              QStringLiteral("layout_slot_count") },
            { QStringLiteral("display_card_need_short_px"),
              QStringLiteral("layout_required_short_px") },
            { QStringLiteral("device_pixel_ratio"),
              QStringLiteral("layout_device_pixel_ratio") },
            { QStringLiteral("cache_window_minimum_need_px"),
              QStringLiteral("layout_cache_window_minimum_need_px") },
            { QStringLiteral("cache_window_maximum_need_px"),
              QStringLiteral("layout_cache_window_maximum_need_px") },
            { QStringLiteral("requested_target_bucket_px"),
              QStringLiteral("layout_requested_target_bucket_px") },
            { QStringLiteral("active_bucket_px"),
              QStringLiteral("layout_active_bucket_px") },
            { QStringLiteral("warming_bucket_px"),
              QStringLiteral("layout_warming_bucket_px") },
            { QStringLiteral("active_generation_id"),
              QStringLiteral("active_generation_id") },
            { QStringLiteral("warming_generation_id"),
              QStringLiteral("warming_generation_id") },
        };
        for (auto it = geometry_aliases.cbegin(); it != geometry_aliases.cend();
             ++it) {
            const QJsonValue value = geometry.value(it.key());
            if (value.isDouble()) {
                metrics.insert(it.value(), value.toDouble());
            }
        }
    }

    populate_metric_aliases(&metrics);
    populate_derived_metrics(&metrics);
    if (metrics.isEmpty()) {
        append_warning_once(
            result, QStringLiteral("embedded snapshot has no numeric metrics")
        );
    }
    return metrics;
}

void validate_catalog(
    const QJsonArray& catalog, const metric_catalog_map& expected,
    monitor_parity_checker::parity_result* result
) {
    const metric_catalog_map incoming
        = hints_by_id_from_catalog(catalog, result);
    const QJsonArray required_fields
        = debug_probe_core::required_metric_hint_fields_v1();
    for (auto it = incoming.cbegin(); it != incoming.cend(); ++it) {
        for (const auto& field_value : required_fields) {
            const QString field = field_value.toString();
            const QString incoming_value = it.value().value(field).toString();
            if (incoming_value.isEmpty()) {
                append_warning_once(
                    result,
                    QStringLiteral(
                        "metric '%1' missing required hint field '%2'"
                    )
                        .arg(it.key(), field)
                );
                continue;
            }
            if (expected.contains(it.key())) {
                const QString expected_value
                    = expected.value(it.key()).value(field).toString();
                if (!expected_value.isEmpty()
                    && incoming_value != expected_value) {
                    append_warning_once(
                        result,
                        QStringLiteral(
                            "metric '%1' semantic drift in '%2': "
                            "incoming='%3' expected='%4'"
                        )
                            .arg(
                                it.key(), field, incoming_value, expected_value
                            )
                    );
                }
            }
        }
    }
}

void merge_sample_batch_metrics(
    numeric_metric_map* target, const QJsonArray& samples,
    const metric_catalog_map& catalog,
    monitor_parity_checker::parity_result* result
) {
    if (target == nullptr) {
        return;
    }
    for (const auto& sample_value : samples) {
        const QJsonObject sample = sample_value.toObject();
        const QString metric_id
            = sample.value(QStringLiteral("metric_id")).toString();
        const QJsonValue value = sample.value(QStringLiteral("value"));
        if (metric_id.isEmpty() || !value.isDouble()) {
            append_warning_once(
                result,
                QStringLiteral("sample batch contains a malformed metric")
            );
            continue;
        }
        if (!catalog.contains(metric_id)) {
            append_warning_once(
                result,
                QStringLiteral("sample metric '%1' is not catalogued")
                    .arg(metric_id)
            );
            continue;
        }
        const double numeric_value = value.toDouble();
        if (!std::isfinite(numeric_value)) {
            append_warning_once(
                result,
                QStringLiteral("sample metric '%1' is not finite")
                    .arg(metric_id)
            );
            continue;
        }
        target->insert(metric_id, numeric_value);
    }
}

struct tolerance_rule {
    bool supported = false;
    double tolerance = 0.0;
    QString description;
};

tolerance_rule typed_tolerance(
    const QString& unit, double embedded, double external,
    const monitor_parity_checker::comparison_policy& policy
) {
    if (unit == QStringLiteral("bytes")) {
        return {
            true,
            static_cast<double>(policy.byte_tolerance),
            QStringLiteral("absolute byte tolerance"),
        };
    }
    const double magnitude = std::max(std::abs(embedded), std::abs(external));
    if (unit == QStringLiteral("ms")) {
        return {
            true,
            std::max(
                policy.duration_absolute_tolerance_ms,
                magnitude * policy.relative_tolerance
            ),
            QStringLiteral("maximum of absolute-ms and relative tolerance"),
        };
    }
    if (unit == QStringLiteral("percent") || unit == QStringLiteral("ratio")) {
        return {
            true,
            std::max(
                policy.fractional_absolute_tolerance,
                magnitude * policy.relative_tolerance
            ),
            QStringLiteral(
                "maximum of absolute-fraction and relative tolerance"
            ),
        };
    }
    if (unit == QStringLiteral("count") || unit == QStringLiteral("id")
        || unit == QStringLiteral("px")) {
        return { true, 0.0, QStringLiteral("exact integral comparison") };
    }
    if (!policy.allow_unknown_units) {
        return {
            false,
            0.0,
            QStringLiteral("unsupported unit; no comparison policy selected"),
        };
    }
    return {
        true,
        std::max(
            policy.fractional_absolute_tolerance,
            magnitude * policy.relative_tolerance
        ),
        QStringLiteral(
            "explicit generic absolute/relative compatibility policy"
        ),
    };
}

void compare_metric(
    const QString& metric_id, double embedded, double external,
    const QJsonObject& metric_hint,
    const monitor_parity_checker::comparison_policy& policy,
    monitor_parity_checker::parity_result* result
) {
    if (result == nullptr) {
        return;
    }
    ++result->compared_metric_count;
    const QString unit = metric_hint.value(QStringLiteral("unit")).toString();
    if (!std::isfinite(embedded) || !std::isfinite(external)) {
        result->comparison_details.push_back(
            QStringLiteral(
                "metric '%1': unit=%2 rule=rejected non-finite value"
            )
                .arg(
                    metric_id,
                    unit.isEmpty() ? QStringLiteral("<missing>") : unit
                )
        );
        result->warnings.push_back(
            QStringLiteral("metric '%1' contains a non-finite value")
                .arg(metric_id)
        );
        return;
    }

    const bool integral_unit = unit == QStringLiteral("bytes")
        || unit == QStringLiteral("count") || unit == QStringLiteral("id")
        || unit == QStringLiteral("px");
    if (integral_unit
        && (std::trunc(embedded) != embedded
            || std::trunc(external) != external)) {
        result->comparison_details.push_back(
            QStringLiteral(
                "metric '%1': unit=%2 rule=exact integral comparison; rejected "
                "fractional value"
            )
                .arg(metric_id, unit)
        );
        result->warnings.push_back(
            QStringLiteral(
                "metric '%1' uses integral unit '%2' with a fractional value"
            )
                .arg(metric_id, unit)
        );
        return;
    }

    const tolerance_rule rule
        = typed_tolerance(unit, embedded, external, policy);
    result->comparison_details.push_back(
        QStringLiteral("metric '%1': unit=%2 rule=%3 tolerance=%4")
            .arg(
                metric_id, unit.isEmpty() ? QStringLiteral("<missing>") : unit,
                rule.description, QString::number(rule.tolerance, 'g', 16)
            )
    );
    if (!rule.supported) {
        result->warnings.push_back(
            QStringLiteral(
                "metric '%1' uses unknown unit '%2'; pass an explicit "
                "unknown-unit policy to compare it"
            )
                .arg(
                    metric_id,
                    unit.isEmpty() ? QStringLiteral("<missing>") : unit
                )
        );
        return;
    }
    const double drift = std::abs(embedded - external);
    if (drift <= rule.tolerance) {
        return;
    }
    result->warnings.push_back(
        QStringLiteral(
            "metric '%1' drift exceeds %2 tolerance: embedded=%3 "
            "external=%4 tolerance=%5"
        )
            .arg(
                metric_id, unit.isEmpty() ? QStringLiteral("typed") : unit,
                QString::number(embedded, 'g', 16),
                QString::number(external, 'g', 16),
                QString::number(rule.tolerance, 'g', 16)
            )
    );
}

bool identity_field_differs(const QString& expected, const QString& actual) {
    return !expected.isEmpty() && !actual.isEmpty() && expected != actual;
}

void validate_identity(
    const protocol_identity_view& expected,
    const protocol_identity_view& actual,
    monitor_parity_checker::parity_result* result
) {
    if (actual.session.isEmpty()) {
        append_warning_once(
            result,
            QStringLiteral("protocol message is missing session identity")
        );
    }

    const struct {
        const char* name;
        const QString* expected_value;
        const QString* actual_value;
    } fields[] {
        { "app", &expected.app, &actual.app },
        { "build", &expected.build, &actual.build },
        { "protocol_version", &expected.protocol_version,
          &actual.protocol_version },
    };

    for (const auto& field : fields) {
        if (identity_field_differs(
                *field.expected_value, *field.actual_value
            )) {
            append_warning_once(
                result,
                QStringLiteral(
                    "session identity '%1' mismatch: embedded='%2' "
                    "external='%3'"
                )
                    .arg(
                        QString::fromLatin1(field.name), *field.expected_value,
                        *field.actual_value
                    )
            );
        }
    }
    if (expected.pid > 0 && actual.pid > 0 && expected.pid != actual.pid) {
        append_warning_once(
            result,
            QStringLiteral(
                "session identity 'pid' mismatch: embedded='%1' external='%2'"
            )
                .arg(expected.pid)
                .arg(actual.pid)
        );
    }
}

void append_parse_warning(
    monitor_parity_checker::history_parse_result* result, const QString& warning
) {
    constexpr qsizetype maximum_retained_parse_warnings = 32;
    if (result != nullptr
        && result->warnings.size() < maximum_retained_parse_warnings) {
        result->warnings.push_back(warning);
    }
}

} // namespace parity_checker_support

using namespace parity_checker_support;

monitor_parity_checker::parity_result
monitor_parity_checker::compare_snapshot_to_messages(
    const QJsonObject& embedded_snapshot_export,
    const QVector<QJsonObject>& external_messages,
    const comparison_policy& policy
) {
    parity_result result;
    if (policy.byte_tolerance < 0 || policy.duration_absolute_tolerance_ms < 0.0
        || policy.fractional_absolute_tolerance < 0.0
        || policy.relative_tolerance < 0.0
        || !std::isfinite(policy.duration_absolute_tolerance_ms)
        || !std::isfinite(policy.fractional_absolute_tolerance)
        || !std::isfinite(policy.relative_tolerance)) {
        result.warnings.push_back(QStringLiteral(
            "comparison tolerances must be finite and non-negative"
        ));
        return result;
    }

    const protocol_identity_view embedded_identity
        = identity_from_message(embedded_snapshot_export);
    QSet<QString> sessions;
    QVector<QJsonObject> valid_external_messages;
    valid_external_messages.reserve(external_messages.size());
    for (qsizetype index = 0; index < external_messages.size(); ++index) {
        const QJsonObject& message = external_messages.at(index);
        protocol_identity_view identity;
        const QString validation_error
            = validate_protocol_history_message(message, &identity);
        if (!validation_error.isEmpty()) {
            ++result.ignored_message_count;
            append_warning_once(
                &result,
                QStringLiteral("external protocol message %1 is malformed: %2")
                    .arg(index + 1)
                    .arg(validation_error)
            );
            continue;
        }
        sessions.insert(identity.session);
        valid_external_messages.push_back(message);
    }
    if (sessions.size() > 1) {
        append_warning_once(
            &result,
            QStringLiteral(
                "external trace contains multiple sessions; parity requires "
                "one isolated session"
            )
        );
    }

    const bool explicit_session = !policy.session_id.trimmed().isEmpty();
    QString selected_session = policy.session_id.trimmed();
    if (selected_session.isEmpty()) {
        selected_session = embedded_identity.session;
    }
    if (selected_session.isEmpty() && sessions.size() == 1) {
        selected_session = *sessions.cbegin();
    }
    if (selected_session.isEmpty() && sessions.size() > 1) {
        append_warning_once(
            &result,
            QStringLiteral(
                "external trace contains multiple sessions; parity requires "
                "one isolated session"
            )
        );
        return result;
    }
    if (selected_session.isEmpty()) {
        result.warnings.push_back(QStringLiteral(
            "no protocol session identity is available for parity comparison"
        ));
        return result;
    }
    result.selected_session_id = selected_session;
    if (explicit_session && !embedded_identity.session.isEmpty()
        && selected_session != embedded_identity.session) {
        result.warnings.push_back(QStringLiteral(
            "selected external session does not match embedded snapshot session"
        ));
    }

    QVector<QJsonObject> selected_messages;
    selected_messages.reserve(valid_external_messages.size());
    protocol_identity_view session_identity;
    bool has_session_identity = false;
    for (const QJsonObject& message : valid_external_messages) {
        const protocol_identity_view identity = identity_from_message(message);
        if (identity.session != selected_session) {
            ++result.ignored_message_count;
            continue;
        }
        if (!has_session_identity) {
            session_identity = identity;
            has_session_identity = true;
        } else if (!identities_are_consistent(session_identity, identity)) {
            append_warning_once(
                &result,
                QStringLiteral(
                    "external identity changed within selected session '%1'"
                )
                    .arg(selected_session)
            );
        }
        selected_messages.push_back(message);
        validate_identity(embedded_identity, identity, &result);
    }
    result.compared_message_count = selected_messages.size();
    if (selected_messages.isEmpty()) {
        result.warnings.push_back(
            QStringLiteral("selected session has no protocol messages")
        );
        return result;
    }

    const numeric_metric_map embedded_metrics
        = extract_embedded_metrics(embedded_snapshot_export, &result);
    if (embedded_metrics.isEmpty()) {
        return result;
    }

    const metric_catalog_map current_catalog = hints_by_id_from_catalog(
        debug_probe_core::protocol_metric_catalog_v1(), &result
    );
    const QJsonArray embedded_catalog_array
        = embedded_metric_catalog(embedded_snapshot_export);
    metric_catalog_map effective_catalog = embedded_catalog_array.isEmpty()
        ? current_catalog
        : hints_by_id_from_catalog(embedded_catalog_array, &result);

    numeric_metric_map external_metrics;
    bool saw_capabilities = false;
    for (const QJsonObject& message : selected_messages) {
        const QString family = message_family(message);
        if (family == QStringLiteral("capabilities")) {
            saw_capabilities = true;
            const QJsonArray incoming_catalog
                = message.value(QStringLiteral("capabilities"))
                      .toObject()
                      .value(QStringLiteral("metric_catalog"))
                      .toArray();
            if (incoming_catalog.isEmpty()) {
                append_warning_once(
                    &result,
                    QStringLiteral("capabilities metric catalog is empty")
                );
                continue;
            }
            validate_catalog(incoming_catalog, current_catalog, &result);
            if (!embedded_catalog_array.isEmpty()) {
                validate_catalog(
                    incoming_catalog,
                    hints_by_id_from_catalog(embedded_catalog_array, &result),
                    &result
                );
            }
            if (saw_capabilities) {
                validate_catalog(incoming_catalog, effective_catalog, &result);
            }
            effective_catalog
                = hints_by_id_from_catalog(incoming_catalog, &result);
            continue;
        }
        if (family == QStringLiteral("sample_batch")) {
            merge_sample_batch_metrics(
                &external_metrics,
                message.value(QStringLiteral("samples")).toArray(),
                effective_catalog, &result
            );
            continue;
        }
        if (family == QStringLiteral("snapshot")) {
            merge_numeric_object(
                &external_metrics,
                message.value(QStringLiteral("snapshot")).toObject()
            );
        }
    }
    populate_metric_aliases(&external_metrics);
    populate_derived_metrics(&external_metrics);

    if (!saw_capabilities) {
        result.warnings.push_back(QStringLiteral(
            "selected session did not include a capabilities message"
        ));
    }

    QStringList overlapping_metric_ids;
    for (auto it = embedded_metrics.cbegin(); it != embedded_metrics.cend();
         ++it) {
        if (external_metrics.contains(it.key())
            && effective_catalog.contains(it.key())) {
            overlapping_metric_ids.push_back(it.key());
        }
    }
    overlapping_metric_ids.sort();
    if (overlapping_metric_ids.isEmpty()) {
        result.warnings.push_back(QStringLiteral(
            "no overlapping catalogued numeric metrics found for parity "
            "comparison"
        ));
        return result;
    }

    for (const QString& metric_id : overlapping_metric_ids) {
        compare_metric(
            metric_id, embedded_metrics.value(metric_id),
            external_metrics.value(metric_id),
            effective_catalog.value(metric_id), policy, &result
        );
    }
    return result;
}

monitor_parity_checker::parity_result
monitor_parity_checker::compare_snapshot_to_messages(
    const QJsonObject& embedded_snapshot_export,
    const QVector<QJsonObject>& external_messages, qint64 byte_tolerance
) {
    comparison_policy policy;
    policy.byte_tolerance = byte_tolerance;
    return compare_snapshot_to_messages(
        embedded_snapshot_export, external_messages, policy
    );
}

monitor_parity_checker::history_parse_result
monitor_parity_checker::parse_external_history_jsonl(
    QIODevice* input, qsizetype maximum_record_bytes,
    qint64 maximum_message_count, qint64 maximum_retained_bytes
) {
    history_parse_result result;
    if (input == nullptr || !input->isReadable()) {
        result.warnings.push_back(
            QStringLiteral("external history input is not readable")
        );
        return result;
    }
    if (maximum_record_bytes <= 0 || maximum_message_count <= 0
        || maximum_retained_bytes <= 0
        || maximum_record_bytes > std::numeric_limits<qint64>::max() - 2) {
        result.warnings.push_back(
            QStringLiteral("external history bounds must be positive")
        );
        return result;
    }

    constexpr qint64 estimated_message_overhead_bytes = 256;
    qint64 line_number = 0;
    QSet<QString> sessions;
    QHash<QString, protocol_identity_view> identities_by_session;
    QHash<QString, int> protocol_phase_by_session;
    while (!input->atEnd()) {
        ++line_number;
        QByteArray raw
            = input->readLine(static_cast<qint64>(maximum_record_bytes) + 2);
        const bool terminated = raw.endsWith('\n');
        bool oversized
            = raw.size() > maximum_record_bytes + (terminated ? 1 : 0);
        if (!terminated && !input->atEnd()) {
            oversized = true;
            while (!input->atEnd()) {
                const QByteArray remainder = input->readLine(
                    static_cast<qint64>(maximum_record_bytes) + 2
                );
                if (remainder.endsWith('\n')) {
                    break;
                }
            }
        }
        if (oversized) {
            ++result.oversized_record_count;
            append_parse_warning(
                &result,
                QStringLiteral("JSONL record %1 exceeds the %2-byte bound")
                    .arg(line_number)
                    .arg(maximum_record_bytes)
            );
            continue;
        }

        const QByteArray line = raw.trimmed();
        if (line.isEmpty()) {
            continue;
        }
        if (!terminated) {
            ++result.unterminated_record_count;
            append_parse_warning(
                &result,
                QStringLiteral(
                    "unterminated JSONL record %1; the history may be truncated"
                )
                    .arg(line_number)
            );
            continue;
        }
        QJsonParseError parse_error;
        const QJsonDocument document
            = QJsonDocument::fromJson(line, &parse_error);
        if (!document.isObject()) {
            ++result.invalid_record_count;
            const QString reason = parse_error.error == QJsonParseError::NoError
                ? QStringLiteral("record is not a JSON object")
                : parse_error.errorString();
            append_parse_warning(
                &result,
                QStringLiteral("invalid JSON object at JSONL record %1: %2")
                    .arg(line_number)
                    .arg(reason)
            );
            continue;
        }

        protocol_identity_view identity;
        const QString validation_error
            = validate_protocol_history_message(document.object(), &identity);
        if (!validation_error.isEmpty()) {
            ++result.invalid_record_count;
            append_parse_warning(
                &result,
                QStringLiteral(
                    "invalid protocol message at JSONL record %1: %2"
                )
                    .arg(line_number)
                    .arg(validation_error)
            );
            continue;
        }
        const auto previous_identity
            = identities_by_session.constFind(identity.session);
        if (previous_identity != identities_by_session.cend()
            && !identities_are_consistent(*previous_identity, identity)) {
            ++result.invalid_record_count;
            append_parse_warning(
                &result,
                QStringLiteral(
                    "protocol identity changed within session at JSONL record "
                    "%1"
                )
                    .arg(line_number)
            );
            continue;
        }
        identities_by_session.insert(identity.session, identity);
        sessions.insert(identity.session);
        if (sessions.size() > 1 && !result.multiple_sessions) {
            result.multiple_sessions = true;
            append_parse_warning(
                &result,
                QStringLiteral(
                    "JSONL record %1 starts a second session; parity histories "
                    "must contain exactly one session"
                )
                    .arg(line_number)
            );
        }
        const QString family = message_family(document.object());
        const int phase = protocol_phase_by_session.value(identity.session, 0);
        QString phase_error;
        int next_phase = phase;
        if (phase == 0) {
            if (family != QStringLiteral("hello")) {
                phase_error = QStringLiteral("first record must be hello");
            } else {
                next_phase = 1;
            }
        } else if (phase == 1) {
            if (family != QStringLiteral("capabilities")) {
                phase_error = QStringLiteral(
                    "capabilities must immediately follow hello"
                );
            } else {
                next_phase = 2;
            }
        } else if (phase == 2) {
            if (family == QStringLiteral("hello")) {
                phase_error = QStringLiteral("duplicate hello record");
            } else if (family == QStringLiteral("capabilities")) {
                phase_error = QStringLiteral("duplicate capabilities record");
            } else if (family == QStringLiteral("goodbye")) {
                next_phase = 3;
            }
        } else {
            phase_error = QStringLiteral("record follows terminal goodbye");
        }
        if (!phase_error.isEmpty()) {
            ++result.invalid_record_count;
            append_parse_warning(
                &result,
                QStringLiteral(
                    "invalid protocol lifecycle at JSONL record %1: %2"
                )
                    .arg(line_number)
                    .arg(phase_error)
            );
            continue;
        }
        protocol_phase_by_session.insert(identity.session, next_phase);
        if (result.messages.size() >= maximum_message_count) {
            result.message_limit_reached = true;
            append_parse_warning(
                &result,
                QStringLiteral(
                    "external history reached the %1-message bound at JSONL "
                    "record %2"
                )
                    .arg(maximum_message_count)
                    .arg(line_number)
            );
            break;
        }
        const qint64 retained_cost = static_cast<qint64>(line.size())
            + estimated_message_overhead_bytes;
        if (retained_cost > maximum_retained_bytes
            || result.estimated_retained_bytes
                > maximum_retained_bytes - retained_cost) {
            result.retained_byte_limit_reached = true;
            append_parse_warning(
                &result,
                QStringLiteral(
                    "retaining JSONL record %1 would exceed the %2-byte "
                    "aggregate history bound"
                )
                    .arg(line_number)
                    .arg(maximum_retained_bytes)
            );
            break;
        }
        result.messages.push_back(document.object());
        result.estimated_retained_bytes += retained_cost;
    }
    for (auto it = protocol_phase_by_session.cbegin();
         it != protocol_phase_by_session.cend(); ++it) {
        if (it.value() == 1) {
            ++result.invalid_record_count;
            append_parse_warning(
                &result,
                QStringLiteral(
                    "session '%1' ended before its capabilities record"
                )
                    .arg(it.key())
            );
        }
    }
    return result;
}

QVector<QJsonObject> monitor_parity_checker::parse_external_history_jsonl(
    const QByteArray& jsonl_data
) {
    QBuffer buffer;
    buffer.setData(jsonl_data);
    if (!buffer.open(QIODevice::ReadOnly)) {
        return {};
    }
    const history_parse_result result = parse_external_history_jsonl(
        &buffer, 1024 * 1024, 100000, 64 * 1024 * 1024
    );
    return result.ok() ? result.messages : QVector<QJsonObject>();
}

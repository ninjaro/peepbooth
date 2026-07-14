#pragma once

#include "listener/history_writer.hpp"

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QTimer>

class QLocalSocket;

class telemetry_session : public QObject {
    Q_OBJECT

public:
    enum class connection_state {
        disconnected,
        connecting,
        connected,
    };
    Q_ENUM(connection_state)

    static constexpr qsizetype maximum_record_bytes = 1024 * 1024;
    static constexpr qsizetype maximum_catalog_entries = 1024;
    static constexpr qsizetype maximum_batch_entries = 4096;
    static constexpr qsizetype maximum_debug_flags = 64;

    explicit telemetry_session(QObject* parent = nullptr);
    ~telemetry_session() override;

    void connect_to_endpoint(const QString& endpoint_path);
    void disconnect_from_endpoint();
    void set_history_output_directory(const QString& directory);
    void set_history_enabled(bool enabled);

    [[nodiscard]] connection_state state() const;
    [[nodiscard]] bool has_active_session() const;
    [[nodiscard]] QString endpoint_path() const;
    [[nodiscard]] QString history_path() const;
    [[nodiscard]] QJsonObject identity() const;
    [[nodiscard]] QHash<QString, double> latest_numeric_metrics() const;
    bool flush_history_batch(QString* error_message = nullptr);
    bool close_history(QString* error_message = nullptr);

    static QString normalize_endpoint_path(const QString& endpoint);

signals:
    void connection_state_changed(telemetry_session::connection_state state);
    void connected(const QString& endpoint_path);
    void disconnected();
    void connection_failed(const QString& message);
    void session_started(const QJsonObject& identity);
    void session_reset();
    void protocol_error(const QString& code, const QString& message);
    void protocol_message_received(const QJsonObject& message);
    void geometry_received(const QJsonObject& geometry);
    void cache_decision_received(const QJsonObject& event);
    void layout_transition_received(const QJsonObject& event);
    void history_path_changed(const QString& path);
    void history_error(const QString& message);
    void history_line_dropped(qint64 total_dropped_lines);

private slots:
    void on_socket_connected();
    void on_socket_disconnected();
    void on_socket_ready_read();
    void on_socket_error();
    void on_connect_timeout();

private:
    QLocalSocket* socket;
    QTimer connect_timeout_timer;
    QByteArray pending_record;
    QString current_endpoint;
    QString history_output_directory;
    connection_state current_state;
    QJsonObject current_identity;
    QHash<QString, QJsonObject> metric_catalog;
    QHash<QString, double> numeric_metrics;
    monitor_history_writer history_writer;
    bool history_is_enabled;
    bool accepted_hello;
    bool accepted_capabilities;
    bool accepted_goodbye;

    void consume_bytes(const QByteArray& bytes);
    bool process_record(const QByteArray& compact_json_line);
    static bool validate_envelope(
        const QJsonObject& message, QString* family, QJsonObject* identity,
        QString* error_code, QString* error_message
    );
    static bool validate_payload(
        const QString& family, const QJsonObject& message, QString* error_code,
        QString* error_message
    );
    bool accept_hello(
        const QJsonObject& identity, const QByteArray& compact_json_line
    );
    bool validate_catalog_references(
        const QString& family, const QJsonObject& message, QString* error_code,
        QString* error_message
    ) const;
    void
    update_model_and_emit(const QString& family, const QJsonObject& message);
    void reject_protocol(const QString& code, const QString& message);
    void cleanup_socket();
    void reset_session_state();
    static bool
    identities_match(const QJsonObject& expected, const QJsonObject& incoming);
    static QString default_history_directory();
};

Q_DECLARE_METATYPE(telemetry_session::connection_state)

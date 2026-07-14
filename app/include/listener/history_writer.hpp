#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>

#include <deque>

class QThread;

class monitor_history_writer : public QObject {
    Q_OBJECT

public:
    struct limits {
        qint64 maximum_queued_bytes = 4 * 1024 * 1024;
        qint64 maximum_file_bytes = 64 * 1024 * 1024;
        qint64 maximum_retained_bytes = 256 * 1024 * 1024;
        qint64 maximum_batch_bytes = 256 * 1024;
        int maximum_retained_files = 16;
        qint64 maximum_file_age_ms = 15 * 60 * 1000;
        qint64 maximum_retained_age_ms = 7 * 24 * 60 * 60 * 1000;
        int flush_interval_ms = 50;
    };

    explicit monitor_history_writer(QObject* parent = nullptr);
    explicit monitor_history_writer(
        const limits& writer_limits, QObject* parent = nullptr
    );
    ~monitor_history_writer() override;

    bool start_session(
        const QString& output_directory, const QString& session_id,
        const QByteArray& hello_line, QString* error_message = nullptr
    );
    bool enqueue_line(const QByteArray& compact_json_line);
    bool flush_one_batch(QString* error_message = nullptr);
    bool close_session(QString* error_message = nullptr);

    [[nodiscard]] QString current_path() const;
    [[nodiscard]] qint64 queued_bytes() const;
    [[nodiscard]] qint64 dropped_lines() const;
    [[nodiscard]] qint64 written_lines() const;
    [[nodiscard]] QString last_error() const;
    [[nodiscard]] bool uses_dedicated_io_thread() const;

signals:
    void path_changed(const QString& path);
    void write_error(const QString& message);
    void line_dropped(qint64 total_dropped_lines);
    void io_state_changed();

private slots:
    void on_flush_timeout();

private:
    limits configured_limits;
    QThread* io_thread;
    QObject* io_worker;
    QString active_path;
    std::deque<QByteArray> pending_lines;
    qint64 pending_bytes;
    qint64 dropped_line_count;
    qint64 written_line_count;
    QTimer flush_timer;
    QString error_text;
    bool session_is_open;
    bool start_is_in_flight;
    bool batch_is_in_flight;
    bool close_is_in_flight;
    bool last_batch_succeeded;
    qint64 in_flight_bytes;
    qsizetype in_flight_line_count;
    quint64 session_generation;

    bool dispatch_one_batch();
    [[nodiscard]] bool wait_for_io_state(int timeout_ms) const;
    void complete_batch(
        quint64 generation, bool success, qsizetype source_lines_written,
        qint64 protocol_lines_written, const QString& path,
        const QString& error_message, const QString& warning_message
    );
    void complete_start(
        quint64 generation, bool success, qint64 protocol_lines_written,
        const QString& path, const QString& error_message,
        const QString& warning_message
    );
    void complete_close(
        quint64 generation, bool success, const QString& error_message,
        const QString& warning_message
    );
    void report_discarded_lines(qint64 line_count, const QString& reason);
    void set_error(const QString& message);
    static QString sanitize_session_token(const QString& session_id);
};

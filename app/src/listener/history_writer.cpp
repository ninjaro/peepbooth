#include "listener/history_writer.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QThread>

#include <algorithm>

namespace {

constexpr int maximum_shutdown_flush_batches = 2;
constexpr int explicit_batch_wait_ms = 500;
constexpr int close_wait_ms = 250;
constexpr int worker_stop_wait_ms = 250;

monitor_history_writer::limits
normalize_limits(const monitor_history_writer::limits& requested) {
    return monitor_history_writer::limits {
        .maximum_queued_bytes
        = std::max<qint64>(64 * 1024, requested.maximum_queued_bytes),
        .maximum_file_bytes
        = std::max<qint64>(64 * 1024, requested.maximum_file_bytes),
        .maximum_retained_bytes = std::max(
            std::max<qint64>(64 * 1024, requested.maximum_file_bytes),
            requested.maximum_retained_bytes
        ),
        .maximum_batch_bytes
        = std::max<qint64>(4 * 1024, requested.maximum_batch_bytes),
        .maximum_retained_files = std::max(1, requested.maximum_retained_files),
        .maximum_file_age_ms
        = std::max<qint64>(1, requested.maximum_file_age_ms),
        .maximum_retained_age_ms
        = std::max<qint64>(1, requested.maximum_retained_age_ms),
        .flush_interval_ms = std::max(1, requested.flush_interval_ms),
    };
}

struct history_io_result {
    bool success = true;
    qsizetype source_lines_written = 0;
    qint64 protocol_lines_written = 0;
    QString active_path;
    QString error_message;
    QString warning_message;
};

class history_io_worker final : public QObject {
public:
    explicit history_io_worker(const monitor_history_writer::limits& limits)
        : configured_limits(limits)
        , output_file(this) { }

    history_io_result start_session(
        const QString& output_directory, const QString& session_token,
        const QByteArray& hello_line
    ) {
        Q_ASSERT(output_file.thread() == QThread::currentThread());
        history_io_result result = close_session();
        if (!result.success) {
            return result;
        }

        base_output_directory
            = QDir(output_directory)
                  .filePath(QStringLiteral("monitor_%1")
                                .arg(QCoreApplication::applicationPid()));
        active_session_token = session_token;
        session_hello_line = hello_line;
        segment_index = 0;
        QDir directory;
        if (!directory.mkpath(base_output_directory)) {
            result.success = false;
            result.error_message
                = QStringLiteral("unable to create history directory: %1")
                      .arg(base_output_directory);
            return result;
        }
        QFile process_directory(base_output_directory);
        if (!process_directory.setPermissions(
                QFileDevice::ReadOwner | QFileDevice::WriteOwner
                | QFileDevice::ExeOwner
            )) {
            result.success = false;
            result.error_message
                = QStringLiteral(
                      "unable to restrict history directory permissions: %1"
                )
                      .arg(base_output_directory);
            return result;
        }

        result = open_next_segment(false);
        if (!result.success) {
            return result;
        }
        const history_io_result hello_result = write_source_line(hello_line);
        merge_result(&result, hello_result);
        if (result.success && !output_file.flush()) {
            result.success = false;
            result.error_message
                = QStringLiteral("unable to flush history file '%1': %2")
                      .arg(active_path, output_file.errorString());
        }
        if (result.success) {
            history_io_result retention_result;
            retention_result.warning_message = enforce_retention();
            merge_result(&result, retention_result);
        }
        result.active_path = active_path;
        return result;
    }

    history_io_result write_batch(const QVector<QByteArray>& lines) {
        Q_ASSERT(output_file.thread() == QThread::currentThread());
        history_io_result result;
        result.active_path = active_path;
        if (!output_file.isOpen()) {
            result.success = false;
            result.error_message = QStringLiteral("history file is not open");
            return result;
        }

        for (const QByteArray& line : lines) {
            const history_io_result line_result = write_source_line(line);
            merge_result(&result, line_result);
            if (!line_result.success) {
                break;
            }
            ++result.source_lines_written;
        }
        if (result.success && !output_file.flush()) {
            result.success = false;
            result.error_message
                = QStringLiteral("unable to flush history file '%1': %2")
                      .arg(active_path, output_file.errorString());
        }
        if (result.success) {
            history_io_result retention_result;
            retention_result.warning_message = enforce_retention();
            merge_result(&result, retention_result);
        }
        result.active_path = active_path;
        return result;
    }

    history_io_result close_session() {
        Q_ASSERT(output_file.thread() == QThread::currentThread());
        history_io_result result;
        result.active_path = active_path;
        if (output_file.isOpen() && !output_file.flush()) {
            result.success = false;
            result.error_message
                = QStringLiteral("unable to flush history file '%1': %2")
                      .arg(active_path, output_file.errorString());
        }
        output_file.close();
        active_path.clear();
        base_output_directory.clear();
        active_session_token.clear();
        session_hello_line.clear();
        segment_index = 0;
        segment_opened_utc_ms = 0;
        return result;
    }

private:
    monitor_history_writer::limits configured_limits;
    QFile output_file;
    QString base_output_directory;
    QString active_session_token;
    QByteArray session_hello_line;
    QString active_path;
    int segment_index = 0;
    qint64 segment_opened_utc_ms = 0;

    static void
    merge_result(history_io_result* target, const history_io_result& update) {
        if (target == nullptr) {
            return;
        }
        target->success = target->success && update.success;
        target->protocol_lines_written += update.protocol_lines_written;
        if (!update.active_path.isEmpty()) {
            target->active_path = update.active_path;
        }
        if (!update.error_message.isEmpty()) {
            target->error_message = update.error_message;
        }
        if (!update.warning_message.isEmpty()) {
            if (!target->warning_message.isEmpty()) {
                target->warning_message += QLatin1Char('\n');
            }
            target->warning_message += update.warning_message;
        }
    }

    history_io_result open_next_segment(bool repeat_hello) {
        history_io_result result;
        if (output_file.isOpen()) {
            if (!output_file.flush()) {
                result.success = false;
                result.error_message
                    = QStringLiteral("unable to flush history file '%1': %2")
                          .arg(active_path, output_file.errorString());
                return result;
            }
            output_file.close();
        }

        constexpr int maximum_filename_attempts = 100;
        bool opened = false;
        for (int attempt = 0; attempt < maximum_filename_attempts; ++attempt) {
            const QString timestamp = QDateTime::currentDateTimeUtc().toString(
                QStringLiteral("yyyyMMdd_hhmmss_zzz")
            );
            const QString filename
                = QStringLiteral("%1_%2_%3.jsonl")
                      .arg(active_session_token, timestamp)
                      .arg(segment_index++, 3, 10, QLatin1Char('0'));
            active_path = QDir(base_output_directory).filePath(filename);
            output_file.setFileName(active_path);
            if (output_file.open(
                    QIODevice::WriteOnly | QIODevice::NewOnly | QIODevice::Text
                )) {
                opened = true;
                segment_opened_utc_ms = QDateTime::currentMSecsSinceEpoch();
                break;
            }
            if (!QFileInfo::exists(active_path)) {
                break;
            }
        }
        if (!opened) {
            result.success = false;
            result.error_message
                = QStringLiteral("unable to open history file '%1': %2")
                      .arg(active_path, output_file.errorString());
            return result;
        }
        if (!output_file.setPermissions(
                QFileDevice::ReadOwner | QFileDevice::WriteOwner
            )) {
            result.success = false;
            result.error_message
                = QStringLiteral("unable to restrict history permissions: %1")
                      .arg(active_path);
            output_file.close();
            QFile::remove(active_path);
            return result;
        }

        result.warning_message = enforce_retention();
        result.active_path = active_path;
        if (repeat_hello && !session_hello_line.isEmpty()) {
            const history_io_result hello_result
                = write_raw_line(session_hello_line);
            merge_result(&result, hello_result);
        }
        return result;
    }

    history_io_result write_source_line(const QByteArray& line) {
        history_io_result result;
        const qint64 required_bytes = static_cast<qint64>(line.size()) + 1;
        const qint64 segment_age_ms
            = QDateTime::currentMSecsSinceEpoch() - segment_opened_utc_ms;
        const bool size_limit_reached = output_file.size() > 0
            && output_file.size() + required_bytes
                > configured_limits.maximum_file_bytes;
        const bool time_limit_reached = output_file.size() > 0
            && segment_age_ms >= configured_limits.maximum_file_age_ms;
        if (size_limit_reached || time_limit_reached) {
            result = open_next_segment(true);
            if (!result.success) {
                return result;
            }
        }
        const history_io_result write_result = write_raw_line(line);
        merge_result(&result, write_result);
        result.active_path = active_path;
        return result;
    }

    history_io_result write_raw_line(const QByteArray& line) {
        history_io_result result;
        QByteArray record = line;
        record.append('\n');
        const qint64 written = output_file.write(record);
        if (written != static_cast<qint64>(record.size())) {
            result.success = false;
            result.error_message
                = QStringLiteral("short write to history file '%1': %2")
                      .arg(active_path, output_file.errorString());
            return result;
        }
        result.protocol_lines_written = 1;
        result.active_path = active_path;
        return result;
    }

    [[nodiscard]] QString enforce_retention() const {
        QDir directory(base_output_directory);
        const QFileInfoList files = directory.entryInfoList(
            QStringList() << QStringLiteral("*.jsonl"), QDir::Files,
            QDir::Time | QDir::Reversed
        );
        qint64 retained_bytes = 0;
        for (const QFileInfo& file : files) {
            retained_bytes += std::max<qint64>(0, file.size());
        }
        qsizetype retained_count = files.size();
        const QDateTime oldest_allowed
            = QDateTime::currentDateTimeUtc().addMSecs(
                -configured_limits.maximum_retained_age_ms
            );
        QStringList failures;
        for (const QFileInfo& file : files) {
            const QString path = file.absoluteFilePath();
            const bool exceeds_count
                = retained_count > configured_limits.maximum_retained_files;
            const bool exceeds_bytes
                = retained_bytes > configured_limits.maximum_retained_bytes;
            const bool exceeds_age
                = file.lastModified().toUTC() < oldest_allowed;
            if (path == active_path
                || (!exceeds_count && !exceeds_bytes && !exceeds_age)) {
                continue;
            }
            if (QFileInfo::exists(path) && !QFile::remove(path)
                && QFileInfo::exists(path)) {
                failures.push_back(path);
                continue;
            }
            retained_bytes -= std::max<qint64>(0, file.size());
            --retained_count;
        }
        return failures.isEmpty()
            ? QString()
            : QStringLiteral("unable to remove retained history file(s): %1")
                  .arg(failures.join(QStringLiteral(", ")));
    }
};

history_io_worker* worker_from(QObject* object) {
    return dynamic_cast<history_io_worker*>(object);
}

} // namespace

monitor_history_writer::monitor_history_writer(QObject* parent)
    : monitor_history_writer(limits {}, parent) { }

monitor_history_writer::monitor_history_writer(
    const limits& writer_limits, QObject* parent
)
    : QObject(parent)
    , configured_limits(normalize_limits(writer_limits))
    , io_thread(new QThread(this))
    , io_worker(new history_io_worker(configured_limits))
    , active_path()
    , pending_lines()
    , pending_bytes(0)
    , dropped_line_count(0)
    , written_line_count(0)
    , flush_timer(this)
    , error_text()
    , session_is_open(false)
    , start_is_in_flight(false)
    , batch_is_in_flight(false)
    , close_is_in_flight(false)
    , last_batch_succeeded(true)
    , in_flight_bytes(0)
    , in_flight_line_count(0)
    , session_generation(0) {
    io_worker->moveToThread(io_thread);
    QObject::connect(
        io_thread, &QThread::finished, io_worker, &QObject::deleteLater
    );
    io_thread->start();
    flush_timer.setSingleShot(false);
    flush_timer.setInterval(configured_limits.flush_interval_ms);
    QObject::connect(
        &flush_timer, &QTimer::timeout, this,
        &monitor_history_writer::on_flush_timeout
    );
}

monitor_history_writer::~monitor_history_writer() {
    close_session();
    io_thread->quit();
    if (!io_thread->wait(worker_stop_wait_ms)) {
        set_error(QStringLiteral(
            "history worker did not stop within the bounded deadline"
        ));
        // Do not terminate a thread inside QFile. Detach the rare stuck worker
        // so this QObject can be destroyed without destroying a live QThread;
        // it will delete itself cooperatively if the filesystem call returns.
        io_thread->setParent(nullptr);
        QObject::connect(
            io_thread, &QThread::finished, io_thread, &QObject::deleteLater,
            Qt::QueuedConnection
        );
    }
}

bool monitor_history_writer::start_session(
    const QString& output_directory, const QString& session_id,
    const QByteArray& hello_line, QString* error_message
) {
    close_session();
    dropped_line_count = 0;
    written_line_count = 0;
    error_text.clear();
    const QString directory = output_directory.trimmed();
    const QString token = sanitize_session_token(session_id);
    if (directory.isEmpty()) {
        set_error(QStringLiteral("history output directory is empty"));
        if (error_message != nullptr) {
            *error_message = error_text;
        }
        return false;
    }
    if (hello_line.isEmpty() || hello_line.contains('\n')) {
        set_error(QStringLiteral("history hello record is invalid"));
        if (error_message != nullptr) {
            *error_message = error_text;
        }
        return false;
    }

    ++session_generation;
    close_is_in_flight = false;
    start_is_in_flight = true;
    session_is_open = true;
    last_batch_succeeded = true;
    QPointer<monitor_history_writer> owner(this);
    const quint64 generation = session_generation;
    const bool invoked = QMetaObject::invokeMethod(
        io_worker,
        [worker = worker_from(io_worker), owner, generation, directory, token,
         hello_line]() {
            const history_io_result result
                = worker->start_session(directory, token, hello_line);
            if (owner.isNull()) {
                return;
            }
            QMetaObject::invokeMethod(
                owner,
                [owner, generation, result]() {
                    if (!owner.isNull()) {
                        owner->complete_start(
                            generation, result.success,
                            result.protocol_lines_written, result.active_path,
                            result.error_message, result.warning_message
                        );
                    }
                },
                Qt::QueuedConnection
            );
        },
        Qt::QueuedConnection
    );
    if (!invoked) {
        start_is_in_flight = false;
        session_is_open = false;
        set_error(QStringLiteral("unable to start history worker"));
        if (error_message != nullptr) {
            *error_message = error_text;
        }
        return false;
    }

    flush_timer.start();
    return true;
}

bool monitor_history_writer::enqueue_line(const QByteArray& compact_json_line) {
    if (!session_is_open || compact_json_line.isEmpty()
        || compact_json_line.contains('\n')) {
        return false;
    }
    const qint64 line_bytes = static_cast<qint64>(compact_json_line.size()) + 1;
    if (line_bytes > configured_limits.maximum_queued_bytes
        || pending_bytes
            > configured_limits.maximum_queued_bytes - line_bytes) {
        ++dropped_line_count;
        emit line_dropped(dropped_line_count);
        return false;
    }
    pending_lines.push_back(compact_json_line);
    pending_bytes += line_bytes;
    if (!flush_timer.isActive()) {
        flush_timer.start();
    }
    return true;
}

bool monitor_history_writer::flush_one_batch(QString* error_message) {
    if (!session_is_open) {
        return true;
    }
    if ((start_is_in_flight || batch_is_in_flight)
        && !wait_for_io_state(explicit_batch_wait_ms)) {
        set_error(QStringLiteral("history batch flush timed out"));
        if (error_message != nullptr) {
            *error_message = error_text;
        }
        return false;
    }
    if (!dispatch_one_batch()) {
        if (error_message != nullptr && !error_text.isEmpty()) {
            *error_message = error_text;
        }
        return !batch_is_in_flight && last_batch_succeeded;
    }
    if (!wait_for_io_state(explicit_batch_wait_ms)) {
        set_error(QStringLiteral("history batch flush timed out"));
        if (error_message != nullptr) {
            *error_message = error_text;
        }
        return false;
    }
    if (!last_batch_succeeded && error_message != nullptr) {
        *error_message = error_text;
    }
    return last_batch_succeeded;
}

bool monitor_history_writer::close_session(QString* error_message) {
    flush_timer.stop();
    if (!session_is_open && !start_is_in_flight && !batch_is_in_flight
        && pending_lines.empty()) {
        return true;
    }

    bool success = true;
    QElapsedTimer deadline;
    deadline.start();
    auto remaining_wait = [&]() {
        return std::max(
            0, close_wait_ms - static_cast<int>(deadline.elapsed())
        );
    };
    if ((start_is_in_flight || batch_is_in_flight)
        && !wait_for_io_state(remaining_wait())) {
        success = false;
    }
    for (int batch = 0; success && !pending_lines.empty()
         && batch < maximum_shutdown_flush_batches;
         ++batch) {
        if (!dispatch_one_batch() || !wait_for_io_state(remaining_wait())) {
            success = false;
            break;
        }
        success = last_batch_succeeded;
    }

    auto discarded_lines = static_cast<qint64>(pending_lines.size());
    if (batch_is_in_flight) {
        discarded_lines += static_cast<qint64>(in_flight_line_count);
    }
    if (discarded_lines > 0) {
        report_discarded_lines(
            discarded_lines,
            QStringLiteral(
                "history close deadline/batch bound left queued records"
            )
        );
        success = false;
    }
    pending_lines.clear();
    pending_bytes = 0;
    batch_is_in_flight = false;
    in_flight_bytes = 0;
    in_flight_line_count = 0;

    const quint64 closing_generation = session_generation;
    close_is_in_flight = true;
    QPointer<monitor_history_writer> owner(this);
    const bool close_invoked = QMetaObject::invokeMethod(
        io_worker,
        [worker = worker_from(io_worker), owner, closing_generation]() {
            const history_io_result result = worker->close_session();
            if (owner.isNull()) {
                return;
            }
            QMetaObject::invokeMethod(
                owner,
                [owner, closing_generation, result]() {
                    if (!owner.isNull()) {
                        owner->complete_close(
                            closing_generation, result.success,
                            result.error_message, result.warning_message
                        );
                    }
                },
                Qt::QueuedConnection
            );
        },
        Qt::QueuedConnection
    );
    if (!close_invoked) {
        close_is_in_flight = false;
        set_error(QStringLiteral("unable to queue history close"));
        success = false;
    } else if (!wait_for_io_state(remaining_wait())) {
        set_error(
            QStringLiteral("history worker close exceeded the bounded deadline")
        );
        success = false;
    } else if (!error_text.isEmpty() && !last_batch_succeeded) {
        success = false;
    }

    session_is_open = false;
    start_is_in_flight = false;
    active_path.clear();
    ++session_generation;
    emit path_changed(QString());
    if (!success && error_message != nullptr) {
        *error_message = error_text;
    }
    return success;
}

QString monitor_history_writer::current_path() const { return active_path; }

qint64 monitor_history_writer::queued_bytes() const { return pending_bytes; }

qint64 monitor_history_writer::dropped_lines() const {
    return dropped_line_count;
}

qint64 monitor_history_writer::written_lines() const {
    return written_line_count;
}

QString monitor_history_writer::last_error() const { return error_text; }

bool monitor_history_writer::uses_dedicated_io_thread() const {
    return io_worker != nullptr && io_thread != nullptr
        && io_worker->thread() == io_thread && io_thread != thread()
        && io_thread->isRunning();
}

void monitor_history_writer::on_flush_timeout() { dispatch_one_batch(); }

bool monitor_history_writer::dispatch_one_batch() {
    if (!session_is_open || start_is_in_flight || batch_is_in_flight
        || pending_lines.empty()) {
        return false;
    }
    QVector<QByteArray> batch;
    qint64 batch_bytes = 0;
    while (!pending_lines.empty()) {
        const QByteArray& line = pending_lines.front();
        const qint64 line_bytes = static_cast<qint64>(line.size()) + 1;
        if (batch_bytes > 0
            && batch_bytes + line_bytes
                > configured_limits.maximum_batch_bytes) {
            break;
        }
        batch.push_back(line);
        pending_lines.pop_front();
        batch_bytes += line_bytes;
    }
    if (batch.isEmpty()) {
        return false;
    }

    batch_is_in_flight = true;
    last_batch_succeeded = true;
    in_flight_bytes = batch_bytes;
    in_flight_line_count = batch.size();
    const quint64 generation = session_generation;
    QPointer<monitor_history_writer> owner(this);
    const bool invoked = QMetaObject::invokeMethod(
        io_worker,
        [worker = worker_from(io_worker), owner, generation,
         batch = std::move(batch)]() {
            const history_io_result result = worker->write_batch(batch);
            if (owner.isNull()) {
                return;
            }
            QMetaObject::invokeMethod(
                owner,
                [owner, generation, result]() {
                    if (!owner.isNull()) {
                        owner->complete_batch(
                            generation, result.success,
                            result.source_lines_written,
                            result.protocol_lines_written, result.active_path,
                            result.error_message, result.warning_message
                        );
                    }
                },
                Qt::QueuedConnection
            );
        },
        Qt::QueuedConnection
    );
    if (!invoked) {
        batch_is_in_flight = false;
        pending_bytes -= in_flight_bytes;
        report_discarded_lines(
            static_cast<qint64>(in_flight_line_count),
            QStringLiteral("unable to queue history batch")
        );
        in_flight_bytes = 0;
        in_flight_line_count = 0;
        last_batch_succeeded = false;
        return false;
    }
    return true;
}

bool monitor_history_writer::wait_for_io_state(int timeout_ms) const {
    if ((!start_is_in_flight && !batch_is_in_flight && !close_is_in_flight)
        || timeout_ms <= 0) {
        return !start_is_in_flight && !batch_is_in_flight
            && !close_is_in_flight;
    }
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(
        this, &monitor_history_writer::io_state_changed, &loop,
        &QEventLoop::quit
    );
    timeout.start(timeout_ms);
    loop.exec();
    return !start_is_in_flight && !batch_is_in_flight && !close_is_in_flight;
}

void monitor_history_writer::complete_start(
    quint64 generation, bool success, qint64 protocol_lines_written,
    const QString& path, const QString& error_message,
    const QString& warning_message
) {
    if (generation != session_generation || !start_is_in_flight) {
        return;
    }
    start_is_in_flight = false;
    session_is_open = success;
    last_batch_succeeded = success;
    written_line_count = protocol_lines_written;
    if (success) {
        active_path = path;
        emit path_changed(active_path);
    } else {
        const auto discarded = static_cast<qint64>(pending_lines.size());
        pending_lines.clear();
        pending_bytes = 0;
        report_discarded_lines(
            discarded, QStringLiteral("history session could not be opened")
        );
    }
    if (!error_message.isEmpty()) {
        set_error(error_message);
    }
    if (!warning_message.isEmpty()) {
        set_error(warning_message);
    }
    emit io_state_changed();
}

void monitor_history_writer::complete_batch(
    quint64 generation, bool success, qsizetype source_lines_written,
    qint64 protocol_lines_written, const QString& path,
    const QString& error_message, const QString& warning_message
) {
    if (generation != session_generation || !batch_is_in_flight) {
        return;
    }
    pending_bytes = std::max<qint64>(0, pending_bytes - in_flight_bytes);
    const qint64 unwritten = static_cast<qint64>(in_flight_line_count)
        - static_cast<qint64>(source_lines_written);
    written_line_count += protocol_lines_written;
    batch_is_in_flight = false;
    in_flight_bytes = 0;
    in_flight_line_count = 0;
    last_batch_succeeded = success && unwritten == 0;
    if (!path.isEmpty() && path != active_path) {
        active_path = path;
        emit path_changed(active_path);
    }
    if (unwritten > 0) {
        report_discarded_lines(
            unwritten, QStringLiteral("history worker did not write the batch")
        );
    }
    if (!error_message.isEmpty()) {
        set_error(error_message);
    }
    if (!warning_message.isEmpty()) {
        set_error(warning_message);
    }
    emit io_state_changed();
}

void monitor_history_writer::complete_close(
    quint64 generation, bool success, const QString& error_message,
    const QString& warning_message
) {
    if (generation != session_generation || !close_is_in_flight) {
        return;
    }
    close_is_in_flight = false;
    last_batch_succeeded = last_batch_succeeded && success;
    if (!error_message.isEmpty()) {
        set_error(error_message);
    }
    if (!warning_message.isEmpty()) {
        set_error(warning_message);
    }
    emit io_state_changed();
}

void monitor_history_writer::report_discarded_lines(
    qint64 line_count, const QString& reason
) {
    if (line_count <= 0) {
        return;
    }
    dropped_line_count += line_count;
    emit line_dropped(dropped_line_count);
    set_error(
        QStringLiteral("%1: discarded %2 record(s)").arg(reason).arg(line_count)
    );
}

void monitor_history_writer::set_error(const QString& message) {
    if (message.isEmpty()) {
        return;
    }
    error_text = message;
    emit write_error(error_text);
}

QString
monitor_history_writer::sanitize_session_token(const QString& session_id) {
    QString token = session_id.trimmed();
    if (token.isEmpty()) {
        token = QStringLiteral("session");
    }
    for (QChar& character : token) {
        if (!character.isLetterOrNumber() && character != QLatin1Char('-')
            && character != QLatin1Char('_')) {
            character = QLatin1Char('_');
        }
    }
    return token.left(48);
}

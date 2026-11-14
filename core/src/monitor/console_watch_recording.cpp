#include "console_watch_internal.hpp"
#include "monitor/console_watch.hpp"

#include "monitor/watchdog.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <utility>

#if defined(__linux__) && !defined(__ANDROID__)
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace monitor {
namespace console_detail {
    // Encoding stops before growing beyond the caller's frame budget. No JSON
    // DOM or retained recording history; the existing bounded history owns
    // evidence.
    class recording_json {
    public:
        explicit recording_json(const std::size_t limit)
            : limit_(limit) { }

        recording_json& operator<<(const std::string_view value) {
            if (value.size() > limit_ - data_.size())
                throw std::length_error("recording frame limit");
            data_.append(value);
            return *this;
        }

        template <typename Number>
            requires std::is_arithmetic_v<Number>
        recording_json& operator<<(const Number value) {
            std::array<char, 128> buffer {};
            if constexpr (std::is_floating_point_v<Number>) {
                if (!std::isfinite(value))
                    throw std::invalid_argument("non-finite recording metric");
            }
            const auto result = std::to_chars(
                buffer.data(), buffer.data() + buffer.size(), value
            );
            if (result.ec != std::errc {})
                throw std::invalid_argument("unencodable recording metric");
            return *this << std::string_view(buffer.data(), result.ptr);
        }

        void quoted(const std::string_view text) {
            *this << "\"";
            constexpr std::string_view hex = "0123456789abcdef";
            for (const char character : text) {
                const auto byte = static_cast<unsigned char>(character);
                if (byte == '"' || byte == '\\') {
                    *this << "\\" << std::string_view(&character, 1);
                } else if (byte < 0x20U || byte >= 0x7fU) {
                    *this << "\\u00" << hex.substr(byte >> 4U, 1)
                          << hex.substr(byte & 15U, 1);
                } else {
                    *this << std::string_view(&character, 1);
                }
            }
            *this << "\"";
        }

        [[nodiscard]] const std::string& data() const noexcept { return data_; }

    private:
        std::size_t limit_;
        std::string data_;
    };

    template <typename Value, typename Write>
    void write_optional(
        recording_json& out, const std::optional<Value>& value, Write write
    ) {
        if (value)
            write(*value);
        else
            out << "null";
    }

    void write_identity(
        recording_json& out, const platform::process_identity identity
    ) {
        out << "{\"pid\":" << identity.pid
            << ",\"start_ticks\":" << identity.start_ticks << "}";
    }

    void write_record(recording_json& out, const observation_record& record) {
        out << "{\"identity\":";
        write_identity(out, record.identity);
        out << ",\"observed_at_ns\":" << record.observed_at_ns
            << ",\"status\":";
        write_optional(out, record.status, [&](const auto value) {
            out.quoted(status_name(value));
        });
        out << ",\"memory\":";
        write_optional(out, record.memory, [&](const auto& value) {
            out << "{\"rss_bytes\":" << value.rss_bytes
                << ",\"high_water_bytes\":" << value.high_water_bytes
                << ",\"swap_bytes\":" << value.swap_bytes << "}";
        });
        out << ",\"cpu_percent\":";
        write_optional(out, record.cpu_percent, [&](const auto value) {
            out << value;
        });
        out << ",\"health\":";
        write_optional(out, record.responsiveness, [&](const auto value) {
            out.quoted(health_name(value));
        });
        out << ",\"sampled_at_ns\":";
        write_optional(out, record.sampled_at_ns, [&](const auto value) {
            out << value;
        });
        out << ",\"trend\":";
        write_optional(out, record.trend, [&](const auto& value) {
            out << "{\"duration_ns\":" << value.duration_ns
                << ",\"rss_delta_bytes\":" << value.rss_delta_bytes
                << ",\"bytes_per_second\":" << value.bytes_per_second
                << ",\"sustained_growth\":"
                << (value.sustained_growth ? "true" : "false") << "}";
        });
        out << ",\"progress\":";
        write_optional(out, record.progress, [&](const auto& value) {
            out << "{\"lifecycle\":"
                << static_cast<std::uint32_t>(value.process_lifecycle)
                << ",\"observer_since_ns\":" << value.observation_started_ns
                << ",\"stale_after_ns\":" << value.stale_after_ns
                << ",\"channels\":[";
            bool comma = false;
            for (const auto& item : value.channels) {
                if (comma)
                    out << ",";
                comma = true;
                out << "{\"id\":";
                out.quoted(channel_name(item.id));
                out << ",\"active\":" << (item.active ? "true" : "false")
                    << ",\"coherent\":" << (item.coherent ? "true" : "false")
                    << ",\"timestamp_ns\":" << item.timestamp_ns
                    << ",\"sequence\":" << item.sequence
                    << ",\"expected_since_ns\":" << item.expected_since_ns
                    << "}";
            }
            out << "]}";
        });
        out << "}";
    }

    void write_incident(recording_json& out, const diagnostic_incident& item) {
        out << "{\"kind\":";
        out.quoted(kind_name(item.kind));
        out << ",\"channel\":";
        write_optional(out, item.progress_channel, [&](const auto value) {
            out.quoted(channel_name(value));
        });
        out << ",\"state\":";
        out.quoted(state_name(item.state));
        out << ",\"ending\":";
        out.quoted(ending_name(item.ending));
        out << ",\"matching_observations\":" << item.matching_observations
            << ",\"first\":";
        write_record(out, item.first);
        out << ",\"last\":";
        write_record(out, item.last);
        out << ",\"closing\":";
        write_optional(out, item.closing, [&](const auto& value) {
            write_record(out, value);
        });
        out << "}";
    }

    std::uint32_t
    recording_crc(const std::uint32_t previous, const std::string_view bytes) {
        static constexpr auto table = [] {
            std::array<std::uint32_t, 256> values {};
            for (std::uint32_t index = 0; index < values.size(); ++index) {
                auto value = index;
                for (unsigned bit = 0; bit < 8; ++bit)
                    value = (value >> 1U) ^ ((value & 1U) ? 0xedb88320U : 0U);
                values[index] = value;
            }
            return values;
        }();
        auto crc = ~previous;
        for (const char byte : bytes)
            crc = table[(crc ^ static_cast<unsigned char>(byte)) & 255U]
                ^ (crc >> 8U);
        return ~crc;
    }

    const char* outcome_name(const recording_outcome outcome) {
        switch (outcome) {
        case recording_outcome::process_exited:
            return "process_exited";
        case recording_outcome::once:
            return "once";
        case recording_outcome::cancelled:
            return "cancelled";
        case recording_outcome::identity_changed:
            return "identity_changed";
        case recording_outcome::observation_error:
            return "observation_error";
        }
        throw std::invalid_argument("unknown recording outcome");
    }
} // namespace

using namespace console_detail;

class session_recorder::implementation final {
public:
    implementation(
        const std::string& path, const recording_session& session,
        const recording_limits limits
    )
        : path_(path)
        , partial_(path + ".partial")
        , session_(session)
        , limits_(limits) {
#if defined(__linux__) && !defined(__ANDROID__)
        if (path.empty() || path.ends_with(".partial"))
            throw std::invalid_argument(
                "recording path needs a fresh final name without .partial "
                "suffix"
            );
        if (session.target.pid == 0 || session.watcher.pid == 0
            || session.started_ns == 0 || session.started_utc_ns == 0
            || session.interval_ms <= 0 || session.stale_after_ns == 0)
            throw std::invalid_argument(
                "recording requires known process identities, clocks and "
                "observer configuration"
            );
        if (session.history_capacity == 0 || limits.bytes == 0
            || limits.checkpoints == 0 || limits.frame_bytes == 0)
            throw std::invalid_argument(
                "recording requires explicit nonzero history, byte, checkpoint "
                "and frame limits"
            );
        // Derive a worst-sized footer, including every integer's full width.
        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        reserve_ = footer(
                       "observation_error", "checkpoints", maximum, maximum,
                       maximum, std::numeric_limits<std::uint32_t>::max(),
                       maximum, maximum, {}
        )
                       .size();
        recording_json out(limits.frame_bytes);
        out << "{\"type\":\"session\",\"format\":\"peepbooth-observations\","
               "\"version\":1,\"target\":";
        write_identity(out, session.target);
        out << ",\"watcher\":";
        write_identity(out, session.watcher);
        out << ",\"started_ns\":" << session.started_ns
            << ",\"started_utc_ns\":" << session.started_utc_ns
            << ",\"interval_ms\":" << session.interval_ms
            << ",\"stale_after_ns\":" << session.stale_after_ns
            << ",\"history_capacity\":" << session.history_capacity
            << R"(,"limits":{"bytes":)" << limits.bytes
            << ",\"checkpoints\":" << limits.checkpoints
            << ",\"frame_bytes\":" << limits.frame_bytes
            << "},\"environment\":{";
        out << "\"boot_id\":";
        write_optional(out, session.boot_id, [&](const auto& value) {
            out.quoted(value);
        });
        out << ",\"system\":";
        write_optional(out, session.system, [&](const auto& value) {
            out.quoted(value);
        });
        out << ",\"release\":";
        write_optional(out, session.release, [&](const auto& value) {
            out.quoted(value);
        });
        out << ",\"machine\":";
        write_optional(out, session.machine, [&](const auto& value) {
            out.quoted(value);
        });
        out << "}}\n";
        if (reserve_ > limits.bytes
            || out.data().size() > limits.bytes - reserve_)
            throw std::invalid_argument(
                "recording byte limit cannot hold header and terminal record"
            );
        struct stat info {};
        if (::lstat(path.c_str(), &info) == 0 || errno != ENOENT)
            throw std::runtime_error(
                "recording destination already exists or is inaccessible"
            );
        descriptor_ = ::open(
            partial_.c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
            S_IRUSR | S_IWUSR
        );
        if (descriptor_ < 0)
            throw std::system_error(
                errno, std::generic_category(), "create recording partial file"
            );
        try {
            if (!write_frame(out.data(), true))
                throw std::runtime_error(error_);
            // Persist the partial name too, so an acknowledged initial prefix
            // does not depend on the later publication-directory sync.
            const auto parent = std::filesystem::path(path_).parent_path();
            const int directory = ::open(
                parent.empty() ? "." : parent.c_str(),
                O_RDONLY | O_DIRECTORY | O_CLOEXEC
            );
            if (directory < 0)
                throw std::system_error(
                    errno, std::generic_category(), "open recording directory"
                );
            const int synced = ::fsync(directory);
            const int sync_error = errno;
            ::close(directory);
            if (synced != 0)
                throw std::system_error(
                    sync_error, std::generic_category(),
                    "sync recording partial name"
                );
        } catch (...) {
            ::close(descriptor_);
            descriptor_ = -1;
            throw;
        }
#else
        throw std::runtime_error(
            "recording requires the supported Linux desktop watcher"
        );
#endif
    }

    ~implementation() {
#if defined(__linux__) && !defined(__ANDROID__)
        if (descriptor_ >= 0)
            ::close(descriptor_);
#endif
    }

    bool append(const observation_history& history) {
        if (state_ != recording_state::active)
            return false;
        if (history.records().size() > session_.history_capacity
            || history.incidents().closed().size() > session_.history_capacity)
            return fail("recording history exceeds declared capacity");
        if (history.records().empty())
            return fail("recording needs a retained observation");
        const auto& record = history.records().back();
        if (record.identity.pid != session_.target.pid
            || record.identity.start_ticks != session_.target.start_ticks
            || record.observed_at_ns < last_ns_
            || record.observed_at_ns < session_.started_ns)
            return fail("recording identity or monotonic time mismatch");
        if (count_ == limits_.checkpoints)
            return limit("checkpoints", record.observed_at_ns);
        try {
            recording_json out(limits_.frame_bytes);
            out << R"({"type":"checkpoint","sequence":)" << count_ + 1
                << ",\"observation\":";
            write_record(out, record);
            out << ",\"history_retained\":" << history.records().size()
                << ",\"history_overwritten\":" << history.overwritten()
                << ",\"incidents_overwritten\":"
                << history.incidents().overwritten() << ",\"active\":[";
            bool comma = false;
            for (const auto& incident : history.incidents().active()) {
                if (!incident)
                    continue;
                if (comma)
                    out << ",";
                comma = true;
                write_incident(out, *incident);
            }
            out << "],\"closed\":[";
            comma = false;
            for (const auto& incident : history.incidents().closed()) {
                if (comma)
                    out << ",";
                comma = true;
                write_incident(out, incident);
            }
            out << "]}\n";
            if (out.data().size() > limits_.bytes - used_ - reserve_)
                return limit("bytes", record.observed_at_ns);
            if (!write_frame(out.data(), true))
                return false;
            ++count_;
            last_ns_ = record.observed_at_ns;
            return true;
        } catch (const std::length_error&) {
            return limit("frame_size", record.observed_at_ns);
        } catch (const std::exception& exception) {
            return fail(exception.what());
        }
    }

    bool finish(
        const recording_outcome outcome, const int status,
        const std::uint64_t ended_ns
    ) {
        if (state_ != recording_state::active)
            return state_ == recording_state::finalized;
        if (ended_ns < std::max(last_ns_, session_.started_ns) || status < 0
            || status > 1)
            return fail(
                "invalid recording terminal time or observation exit status"
            );
        return finalize(
            outcome_name(outcome), "", ended_ns, {}, status,
            recording_state::finalized
        );
    }

    recording_state state_ { recording_state::active };
    std::string error_;
    std::uint64_t count_ {}, used_ {};

private:
    [[nodiscard]] std::string footer(
        const std::string_view outcome, const std::string_view reason,
        const std::uint64_t count, const std::uint64_t bytes,
        const std::uint64_t ended_ns, const std::uint32_t crc,
        const std::optional<std::uint64_t> missing,
        const std::uint64_t discarded, const std::optional<int> status
    ) const {
        recording_json out(limits_.frame_bytes);
        out << R"({"type":"end","outcome":)";
        out.quoted(outcome);
        out << ",\"limit\":";
        if (reason.empty())
            out << "null";
        else
            out.quoted(reason);
        out << ",\"checkpoints\":" << count << ",\"prefix_bytes\":" << bytes
            << ",\"crc32\":" << crc << ",\"ended_ns\":" << ended_ns
            << ",\"first_unrecorded_ns\":";
        write_optional(out, missing, [&](const auto value) { out << value; });
        out << ",\"discarded_lower_bound\":" << discarded
            << ",\"observation_exit_status\":";
        write_optional(out, status, [&](const auto value) { out << value; });
        out << "}\n";
        return out.data();
    }

    bool fail(const std::string_view message) {
        state_ = recording_state::failed;
        error_ = message;
        return false;
    }

    bool write_frame(const std::string& frame, const bool checksum) {
#if defined(__linux__) && !defined(__ANDROID__)
        std::size_t offset = 0;
        while (offset < frame.size()) {
            const auto written = ::write(
                descriptor_, frame.data() + offset, frame.size() - offset
            );
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0)
                return fail(
                    std::string("recording write: ") + std::strerror(errno)
                );
            offset += static_cast<std::size_t>(written);
            used_ += static_cast<std::uint64_t>(written);
        }
        if (::fsync(descriptor_) != 0)
            return fail(
                std::string("recording fsync: ") + std::strerror(errno)
            );
        if (checksum)
            crc_ = recording_crc(crc_, frame);
        return true;
#else
        static_cast<void>(frame);
        static_cast<void>(checksum);
        return fail("recording unavailable on this platform");
#endif
    }

    bool limit(const std::string_view reason, const std::uint64_t missing) {
        const bool result = finalize(
            "limit_reached", reason, missing, missing, {},
            recording_state::limited
        );
        if (result)
            error_ = std::string("recording limit reached: ")
                + std::string(reason) + "; subsequent evidence is not recorded";
        return false;
    }

    bool finalize(
        const std::string_view outcome, const std::string_view reason,
        const std::uint64_t ended_ns,
        const std::optional<std::uint64_t> missing,
        const std::optional<int> status, const recording_state final_state
    ) {
        try {
            if (!write_frame(
                    footer(
                        outcome, reason, count_, used_, ended_ns, crc_, missing,
                        missing ? 1 : 0, status
                    ),
                    false
                ))
                return false;
#if defined(__linux__) && !defined(__ANDROID__)
            const int closed = ::close(descriptor_);
            descriptor_ = -1;
            if (closed != 0)
                return fail(
                    std::string("recording close: ") + std::strerror(errno)
                );
            const auto parent = std::filesystem::path(path_).parent_path();
            const int directory = ::open(
                parent.empty() ? "." : parent.c_str(),
                O_RDONLY | O_DIRECTORY | O_CLOEXEC
            );
            if (directory < 0)
                return fail(
                    "cannot open recording directory for durability check"
                );
            // No-clobber publication; a killed writer normally leaves .partial.
            const auto published = ::syscall(
                SYS_renameat2, AT_FDCWD, partial_.c_str(), AT_FDCWD,
                path_.c_str(), RENAME_NOREPLACE
            );
            const int publication_error = errno;
            if (published != 0) {
                ::close(directory);
                return fail(
                    std::string("publish recording: ")
                    + std::strerror(publication_error)
                );
            }
            const int synced = ::fsync(directory);
            if (synced != 0) {
                const auto restored = ::syscall(
                    SYS_renameat2, AT_FDCWD, path_.c_str(), AT_FDCWD,
                    partial_.c_str(), RENAME_NOREPLACE
                );
                ::close(directory);
                return fail(
                    restored == 0
                        ? "directory sync failed; recording retained as partial"
                        : "recording published but directory durability is "
                          "unknown"
                );
            }
            ::close(directory);
#endif
            state_ = final_state;
            return true;
        } catch (const std::exception& exception) {
            return fail(exception.what());
        }
    }

    std::string path_, partial_;
    recording_session session_;
    recording_limits limits_;
    std::uint64_t reserve_ {}, last_ns_ {};
    std::uint32_t crc_ {};
    int descriptor_ { -1 };
};

session_recorder::session_recorder(
    const std::string& path, const recording_session& session,
    const recording_limits limits
)
    : implementation_(std::make_unique<implementation>(path, session, limits)) {
}

session_recorder::~session_recorder() = default;

bool session_recorder::append(const observation_history& history) {
    return implementation_->append(history);
}

bool session_recorder::finish(
    const recording_outcome outcome, const int status,
    const std::uint64_t ended_ns
) {
    return implementation_->finish(outcome, status, ended_ns);
}

recording_state session_recorder::state() const noexcept {
    return implementation_->state_;
}

const std::string& session_recorder::error() const noexcept {
    return implementation_->error_;
}

std::uint64_t session_recorder::checkpoints() const noexcept {
    return implementation_->count_;
}

std::uint64_t session_recorder::bytes() const noexcept {
    return implementation_->used_;
}

} // namespace monitor

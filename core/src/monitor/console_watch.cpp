#include "monitor/console_watch.hpp"

#include "monitor/observation_registry.hpp"
#include "monitor/observed_process.hpp"
#include "monitor/process_discovery.hpp"
#include "monitor/watchdog.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#if defined(__linux__) && !defined(__ANDROID__)
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif
#include <iomanip>
#include <optional>
#include <ostream>
#include <utility>
#include <vector>

namespace monitor {
namespace {

    template <typename Integer>
    bool parse_integer(const std::string_view text, Integer& output) noexcept {
        const auto result
            = std::from_chars(text.data(), text.data() + text.size(), output);
        return result.ec == std::errc {}
        && result.ptr == text.data() + text.size();
    }

    double mib(const std::uint64_t bytes) noexcept {
        return static_cast<double>(bytes) / (1024.0 * 1024.0);
    }

    const char* status_name(const observation_status value) noexcept {
        switch (value) {
        case observation_status::not_attached:
            return "not_attached";
        case observation_status::sampled:
            return "sampled";
        case observation_status::sample_unavailable:
            return "sample_unavailable";
        case observation_status::identity_changed:
            return "identity_changed";
        case observation_status::exited:
            return "exited";
        }
        return "unknown";
    }

    const char* kind_name(const incident_kind kind) noexcept {
        switch (kind) {
        case incident_kind::overdue_progress:
            return "overdue_progress";
        case incident_kind::sampled_rss_growth:
            return "sampled_rss_growth";
        case incident_kind::unavailable_evidence:
            return "unavailable_evidence";
        case incident_kind::process_exit:
            return "process_exit";
        }
        return "unknown";
    }

    const char* state_name(const incident_state state) noexcept {
        switch (state) {
        case incident_state::open:
            return "open";
        case incident_state::resolved:
            return "resolved";
        case incident_state::interrupted:
            return "interrupted";
        case incident_state::ended_unknown:
            return "ended_unknown";
        case incident_state::observed:
            return "observed";
        }
        return "unknown";
    }

    const char* ending_name(const incident_ending ending) noexcept {
        switch (ending) {
        case incident_ending::none:
            return "none";
        case incident_ending::condition_cleared:
            return "condition_cleared";
        case incident_ending::progress_observed:
            return "progress_observed";
        case incident_ending::expectation_retired:
            return "expectation_retired";
        case incident_ending::expectation_replaced:
            return "expectation_replaced";
        case incident_ending::observation_gap:
            return "observation_gap";
        case incident_ending::observer_detached:
            return "observer_detached";
        case incident_ending::reattached:
            return "reattached";
        case incident_ending::process_exit:
            return "process_exit";
        case incident_ending::identity_changed:
            return "identity_changed";
        case incident_ending::producer_stopped:
            return "producer_stopped";
        }
        return "unknown";
    }

    void write_evidence(
        std::ostream& output, const char* point,
        const observation_record& record,
        const std::optional<channel> progress_channel
    ) {
        output << "    evidence=" << point << " t_ns=" << record.observed_at_ns
               << " status="
               << (record.status ? status_name(*record.status) : "gap")
               << " cpu=";
        if (record.cpu_percent)
            output << *record.cpu_percent;
        else
            output << "unavailable";
        if (record.memory)
            output << " rss_bytes=" << record.memory->rss_bytes;
        if (record.sampled_at_ns)
            output << " sampled_at_ns=" << *record.sampled_at_ns;
        if (record.trend)
            output << " trend_ns=" << record.trend->duration_ns
                   << " trend_delta_bytes=" << record.trend->rss_delta_bytes
                   << " sampled_growth="
                   << (record.trend->sustained_growth ? "yes" : "no");
        if (record.progress) {
            const bool coherent = std::ranges::all_of(
                record.progress->channels,
                [](const auto& value) { return value.coherent; }
            );
            const bool known_metadata = std::ranges::all_of(
                record.progress->channels, [&](const auto& value) {
                    return !value.active
                        || (value.expected_since_ns != 0
                            && record.progress->observation_started_ns != 0);
                }
            );
            output << " observer_since_ns="
                   << record.progress->observation_started_ns
                   << " stale_after_ns=" << record.progress->stale_after_ns
                   << " producer_lifecycle="
                   << static_cast<std::uint32_t>(
                          record.progress->process_lifecycle
                      )
                   << " watchdog_coherent=" << (coherent ? "yes" : "no")
                   << " expectation_metadata="
                   << (known_metadata ? "known" : "unavailable");
            if (progress_channel) {
                const auto index = channel_index(*progress_channel);
                if (index < record.progress->channels.size()) {
                    const auto& value = record.progress->channels[index];
                    output << " coherent=" << value.coherent
                           << " active=" << value.active
                           << " expected_since_ns=" << value.expected_since_ns
                           << " progress_ns=" << value.timestamp_ns
                           << " sequence=" << value.sequence;
                }
            }
        }
        output << '\n';
    }

    void
    write_history(std::ostream& output, const observation_history& history) {
        output << "history retained=" << history.records().size()
               << " overwritten=" << history.overwritten() << '\n';
        for (const auto& record : history.records()) {
            output << "  t_ns=" << record.observed_at_ns
                   << " pid=" << record.identity.pid
                   << " start_ticks=" << record.identity.start_ticks
                   << " status="
                   << (record.status ? status_name(*record.status) : "gap");
            if (record.responsiveness)
                output << " state=" << health_name(*record.responsiveness);
            if (record.memory)
                output << " rss_bytes=" << record.memory->rss_bytes;
            output << " cpu=";
            if (record.cpu_percent)
                output << *record.cpu_percent;
            else
                output << "unavailable";
            output << '\n';
        }
        const auto& incidents = history.incidents();
        output << "incidents closed=" << incidents.closed().size() << " active="
               << std::ranges::count_if(
                      incidents.active(),
                      [](const auto& item) { return item.has_value(); }
                  )
               << " overwritten=" << incidents.overwritten() << '\n';
        for (const auto& incident : incidents.closed())
            write_console_incident(output, incident);
        for (const auto& incident : incidents.active())
            if (incident)
                write_console_incident(output, *incident);
        output.flush();
    }

} // namespace

namespace {
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

void write_console_incident(
    std::ostream& output, const diagnostic_incident& incident
) {
    const char* source = incident.kind == incident_kind::overdue_progress
        ? "watchdog"
        : incident.kind == incident_kind::sampled_rss_growth ? "resource_trend"
        : incident.kind == incident_kind::process_exit
        ? "process_lifetime"
        : "observation_uncertainty";
    output << "  incident=" << kind_name(incident.kind) << " source=" << source
           << " state=" << state_name(incident.state)
           << " ending=" << ending_name(incident.ending)
           << " pid=" << incident.first.identity.pid
           << " start_ticks=" << incident.first.identity.start_ticks
           << " first_seen_ns=" << incident.first.observed_at_ns
           << " last_seen_ns=" << incident.last.observed_at_ns
           << " matching_observations=" << incident.matching_observations;
    if (incident.progress_channel)
        output << " channel=" << channel_name(*incident.progress_channel);
    output << '\n';
    write_evidence(output, "first", incident.first, incident.progress_channel);
    write_evidence(output, "last", incident.last, incident.progress_channel);
    if (incident.closing)
        write_evidence(
            output, "closing", *incident.closing, incident.progress_channel
        );
}

void write_console_observation(
    std::ostream& output, const process_observation& observation
) {
    if (observation.status != observation_status::sampled || !observation.sample
        || !observation.responsiveness) {
        return;
    }
    const auto& state = observation.watchdog;
    const auto& sample = *observation.sample;
    const auto& trend = observation.memory;
    const auto now = observation.observed_at_ns;
    const auto health = *observation.responsiveness;
    output << (state.process_name.empty() ? "process" : state.process_name)
           << '[' << observation.identity.pid
           << "] state=" << health_name(health) << " cpu=" << std::fixed
           << std::setprecision(1);
    if (observation.cpu_percent)
        output << *observation.cpu_percent << '%';
    else
        output << "unavailable";
    output << " rss=" << std::setprecision(1) << mib(sample.memory.rss_bytes)
           << "MiB hwm=" << mib(sample.memory.high_water_bytes)
           << "MiB swap=" << mib(sample.memory.swap_bytes) << "MiB";
    if (trend.duration_ns != 0) {
        const double signed_delta = trend.rss_delta_bytes >= 0
            ? mib(static_cast<std::uint64_t>(trend.rss_delta_bytes))
            : -mib(static_cast<std::uint64_t>(-trend.rss_delta_bytes));
        output << " rss_trend=" << std::showpos << signed_delta
               << std::noshowpos << "MiB sustained_growth="
               << (trend.sustained_growth ? "yes" : "no");
    }
    output << '\n';

    output << "  heartbeat:";
    bool has_expectation = false;
    for (const auto& heartbeat : state.heartbeats) {
        if (!heartbeat.coherent) {
            has_expectation = true;
            output << ' ' << channel_name(heartbeat.id) << "=unavailable";
            continue;
        }
        if (!heartbeat.active) {
            continue;
        }
        has_expectation = true;
        const double age = heartbeat.timestamp_ns == 0
            ? -1.0
            : static_cast<double>(
                  now >= heartbeat.timestamp_ns ? now - heartbeat.timestamp_ns
                                                : 0
              ) / 1e9;
        output << ' ' << channel_name(heartbeat.id) << '=';
        if (age < 0) {
            output << "pending";
        } else {
            output << std::setprecision(3) << age << 's';
        }
    }
    if (!has_expectation)
        output << " none (no progress coverage; not a claim of application "
                  "health)";
    output << '\n';

    const std::size_t start
        = state.breadcrumbs.size() > 8 ? state.breadcrumbs.size() - 8 : 0;
    if (start < state.breadcrumbs.size()) {
        output << "  recent:";
        for (std::size_t index = start; index < state.breadcrumbs.size();
             ++index) {
            const auto& item = state.breadcrumbs[index];
            output << ' ' << event_name(item.event_id)
                   << "(source=" << item.source_id << ",arg0=" << item.arg0
                   << ",arg1=" << item.arg1 << ')';
        }
        output << '\n';
    }
    output.flush();
}

parsed_command_line
parse_command_line(const std::span<const std::string_view> arguments) noexcept {
    parsed_command_line parsed;
    if (arguments.size() == 1 && arguments.front() == "--list") {
        parsed.action = command_line_action::list;
        return parsed;
    }
    parsed.action = command_line_action::run;
    bool selected = false;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::string_view argument = arguments[index];
        if (argument == "--help" || argument == "-h") {
            parsed.action = command_line_action::help;
            return parsed;
        }
        if (argument == "--once") {
            parsed.options.once = true;
            continue;
        }
        if (argument == "--all" && !selected) {
            parsed.options.all = true;
            selected = true;
            continue;
        }
        if (++index >= arguments.size()) {
            parsed.action = command_line_action::error;
            return parsed;
        }
        const std::string_view value = arguments[index];
        if (argument == "--pid" && !selected
            && parse_integer(value, parsed.options.pid)
            && parsed.options.pid != 0) {
            selected = true;
            continue;
        }
        if (argument == "--name" && !selected && !value.empty()) {
            parsed.options.name = value;
            selected = true;
            continue;
        }
        if (argument == "--interval-ms"
            && parse_integer(value, parsed.options.interval_ms)
            && parsed.options.interval_ms >= 50
            && parsed.options.interval_ms <= 60'000) {
            continue;
        }
        if (argument == "--history"
            && parse_integer(value, parsed.options.history_capacity)
            && parsed.options.history_capacity != 0) {
            continue;
        }
        if (argument == "--record" && parsed.options.recording_path.empty()
            && !value.empty()) {
            parsed.options.recording_path = value;
            continue;
        }
        if (argument == "--record-bytes"
            && parse_integer(value, parsed.options.recording.bytes)
            && parsed.options.recording.bytes != 0)
            continue;
        if (argument == "--record-checkpoints"
            && parse_integer(value, parsed.options.recording.checkpoints)
            && parsed.options.recording.checkpoints != 0)
            continue;
        if (argument == "--record-frame-bytes"
            && parse_integer(value, parsed.options.recording.frame_bytes)
            && parsed.options.recording.frame_bytes != 0)
            continue;
        if (argument == "--stale-ms"
            && parse_integer(value, parsed.options.stale_ms)
            && parsed.options.stale_ms >= 100) {
            continue;
        }
        parsed.action = command_line_action::error;
        return parsed;
    }
    const bool record = !parsed.options.recording_path.empty();
    const auto limits = parsed.options.recording;
    const bool any_limit = limits.bytes != 0 || limits.checkpoints != 0
        || limits.frame_bytes != 0;
    const bool all_limits = limits.bytes != 0 && limits.checkpoints != 0
        && limits.frame_bytes != 0;
    if ((record
         && (parsed.options.pid == 0 || parsed.options.history_capacity == 0
             || !all_limits))
        || (!record && any_limit))
        parsed.action = command_line_action::error;
    if (!selected
        || (parsed.options.history_capacity != 0 && parsed.options.pid == 0)) {
        parsed.action = command_line_action::error;
    }
    return parsed;
}

void write_console_usage(std::ostream& output) {
    output << "usage: monitor (--pid PID | --name NAME | --all) "
              "[--interval-ms N] [--stale-ms N] [--once]\n"
              "       monitor --pid PID --history N (last N observations and "
              "closed incidents)\n"
              "       monitor --pid PID --history N --record PATH "
              "--record-bytes N "
              "--record-checkpoints N --record-frame-bytes N\n"
              "       monitor --list\n";
}

int run_console_list(std::ostream& output, std::ostream& error) {
    const auto inventory = discover_processes();
    if (inventory.available) {
        output << "PID\tSTART_TICKS\tNAME\n";
        for (const auto& process : inventory.processes) {
            auto name = process.name;
            for (auto& character : name) {
                const auto byte = static_cast<unsigned char>(character);
                if (byte < 0x20U || byte == 0x7fU)
                    character = '?';
            }
            output << process.identity.pid << '\t'
                   << process.identity.start_ticks << '\t' << std::quoted(name)
                   << '\n';
        }
    }
    for (const auto& diagnostic : inventory.diagnostics)
        error << "monitor: " << diagnostic << '\n';
    return inventory.available && inventory.diagnostics.empty() ? 0 : 1;
}

int run_console_watch_group(
    const console_options& options, const process_discovery& inventory,
    const std::atomic_bool& stop_requested, std::ostream& output,
    std::ostream& error
) {
    if (stop_requested.load(std::memory_order_relaxed))
        return 0;
    if (options.pid != 0 || options.all == !options.name.empty()
        || options.history_capacity != 0 || !options.recording_path.empty()) {
        error << "monitor: group observation requires exactly one of --all or "
                 "--name; --history requires a single --pid\n";
        return 1;
    }
    if (!inventory.available || !inventory.diagnostics.empty()) {
        error << "monitor: process discovery is incomplete\n";
        for (const auto& diagnostic : inventory.diagnostics)
            error << "monitor: " << diagnostic << '\n';
        return 1;
    }
    std::vector<platform::process_identity> selected;
    if (options.all) {
        for (const auto& endpoint : inventory.processes)
            selected.push_back(endpoint.identity);
    } else {
        const auto named = select_process_name(inventory, options.name);
        if (!named.identity) {
            error << "monitor: " << named.error << '\n';
            return 1;
        }
        selected.push_back(*named.identity);
    }
    if (selected.empty()) {
        error << "monitor: no compatible processes discovered\n";
        return 1;
    }
    observation_registry registry(
        static_cast<std::uint64_t>(options.stale_ms)
        * std::uint64_t { 1'000'000 }
    );
    for (const auto identity : selected) {
        if (stop_requested.load(std::memory_order_relaxed))
            return 0;
        if (!registry.attach(identity)) {
            error << "monitor: PID " << identity.pid
                  << " start_ticks=" << identity.start_ticks
                  << " could not attach to the selected process incarnation\n";
            return 1;
        }
    }
    while (!stop_requested.load(std::memory_order_relaxed)) {
        for (const auto& observation : registry.poll()) {
            if (observation.status == observation_status::exited) {
                output << "PID " << observation.identity.pid
                       << " start_ticks=" << observation.identity.start_ticks
                       << " exited; attached-process exit code/signal "
                          "unavailable lifecycle="
                       << static_cast<std::uint32_t>(
                              observation.watchdog.process_lifecycle
                          )
                       << '\n';
                registry.detach(observation.identity);
                continue;
            }
            if (observation.status != observation_status::sampled) {
                error << "monitor: PID " << observation.identity.pid
                      << " start_ticks=" << observation.identity.start_ticks
                      << (observation.status
                                  == observation_status::identity_changed
                              ? " process identity changed\n"
                              : " resource sample unavailable\n");
                return 1;
            }
            write_console_observation(output, observation);
        }
        if (options.once || registry.size() == 0)
            return 0;
        static_cast<void>(registry.wait_for_exit(options.interval_ms));
    }
    return 0;
}

int run_console_watch(
    const console_options& options, const std::atomic_bool& stop_requested,
    std::ostream& output, std::ostream& error
) {
    if (options.all || !options.name.empty())
        return run_console_watch_group(
            options, discover_processes(), stop_requested, output, error
        );
    auto process = platform::process_handle::attach(options.pid);
    if (!process) {
        error << "monitor: PID " << options.pid
              << " does not identify a running process\n";
        return 1;
    }

    observed_process observer(
        std::move(*process),
        static_cast<std::uint64_t>(options.stale_ms)
            * std::uint64_t { 1'000'000 }
    );
    bool attached = false;
    for (int attempt = 0; attempt < 50
         && !stop_requested.load(std::memory_order_relaxed) && observer.alive();
         ++attempt) {
        attached = observer.try_attach();
        if (attached || observer.wait_for_exit(100)) {
            break;
        }
    }
    if (stop_requested.load(std::memory_order_relaxed)) {
        return 0;
    }
    if (!attached) {
        error << "monitor: PID " << options.pid
              << " has no compatible watchdog state\n";
        return 1;
    }

    observation_history history(
        observer.latest().identity, options.history_capacity
    );
    std::unique_ptr<session_recorder> recorder;
    if (!options.recording_path.empty()) {
        try {
            const auto watcher = platform::read_process_identity(
                platform::current_process_id()
            );
            if (!watcher)
                throw std::runtime_error("watcher identity unavailable");
            recording_session session;
            session.target = observer.latest().identity;
            session.watcher = *watcher;
            session.started_ns = platform::monotonic_time_ns();
            session.started_utc_ns = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch()
                )
                    .count()
            );
            session.interval_ms = options.interval_ms;
            session.stale_after_ns
                = static_cast<std::uint64_t>(options.stale_ms) * 1'000'000ULL;
            session.history_capacity = options.history_capacity;
#if defined(__linux__) && !defined(__ANDROID__)
            struct utsname environment {};
            if (::uname(&environment) == 0) {
                session.system = environment.sysname;
                session.release = environment.release;
                session.machine = environment.machine;
            }
            std::ifstream boot("/proc/sys/kernel/random/boot_id");
            std::string boot_id;
            if (std::getline(boot, boot_id))
                session.boot_id = std::move(boot_id);
#endif
            recorder = std::make_unique<session_recorder>(
                std::string(options.recording_path), session, options.recording
            );
        } catch (const std::exception& exception) {
            error << "monitor: cannot start recording: " << exception.what()
                  << '\n';
            return 1;
        }
    }
    bool recording_reported = false;
    const auto checkpoint = [&] {
        if (recorder && recorder->state() == recording_state::active
            && !recorder->append(history) && !recording_reported) {
            error << "monitor: " << recorder->error() << '\n';
            recording_reported = true;
        }
    };
    const auto finish = [&](const int code, const recording_outcome outcome) {
        if (options.history_capacity != 0) {
            history.note_gap(platform::monotonic_time_ns());
            checkpoint();
            write_history(output, history);
        }
        if (recorder) {
            if (recorder->state() == recording_state::active
                && !recorder->finish(
                    outcome, code, platform::monotonic_time_ns()
                )
                && !recording_reported)
                error << "monitor: " << recorder->error() << '\n';
            if (recorder->state() != recording_state::finalized)
                return 1;
        }
        return code;
    };
    bool missing_samples = false;
    while (!stop_requested.load(std::memory_order_relaxed)) {
        const auto& observation = observer.poll();
        history.add(observation);
        checkpoint();
        if (observation.status == observation_status::exited) {
            break;
        }
        if (observation.status != observation_status::sampled) {
            error << "monitor: PID " << options.pid
                  << " observation=" << status_name(observation.status) << '\n';
            if (observation.status != observation_status::sample_unavailable)
                return finish(
                    1,
                    observation.status == observation_status::identity_changed
                        ? recording_outcome::identity_changed
                        : recording_outcome::observation_error
                );
            // Resource fields can disappear before the lifetime handle reports
            // exit. Keep observing without fabricating evidence or hiding the
            // missing sample in the eventual session result.
            missing_samples = true;
        } else {
            write_console_observation(output, observation);
        }
        if (options.once || observer.wait_for_exit(options.interval_ms)) {
            break;
        }
    }
    // Use the same lifetime observation for sampling and the terminal claim.
    // A later exit must not produce an outcome without corresponding evidence.
    const bool exited = !observer.alive();
    if (exited) {
        const auto& final = observer.poll();
        history.add(final);
        checkpoint();
        output
            << "process exited; attached-process exit code/signal unavailable"
            << " lifecycle="
            << static_cast<std::uint32_t>(final.watchdog.process_lifecycle)
            << '\n';
    }
    const auto outcome = exited ? recording_outcome::process_exited
        : stop_requested.load(std::memory_order_relaxed)
        ? recording_outcome::cancelled
        : recording_outcome::once;
    return finish(missing_samples ? 1 : 0, outcome);
}

} // namespace monitor

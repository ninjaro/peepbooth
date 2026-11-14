#include "console_watch_internal.hpp"
#include "monitor/console_watch.hpp"

#include "monitor/watchdog.hpp"

#include <algorithm>
#include <iomanip>
#include <ostream>

namespace monitor {
namespace console_detail {

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

} // namespace console_detail

using namespace console_detail;

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

} // namespace monitor

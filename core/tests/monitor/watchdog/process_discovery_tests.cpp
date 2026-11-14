#include "monitor/client.hpp"
#include "monitor/console_watch.hpp"
#include "monitor/observation_registry.hpp"
#include "monitor/observed_process.hpp"
#include "monitor/platform/process_watch.hpp"
#include "monitor/platform/shared_memory.hpp"
#include "monitor/process_discovery.hpp"
#include "watchdog_checks.hpp"

#include <algorithm>
#include <sstream>
#include <string>
#include <utility>

#if defined(__linux__) && !defined(__ANDROID__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace monitor_test {

void test_process_discovery() {
#if defined(__linux__) && !defined(__ANDROID__)
    auto mapping = monitor::platform::create_process_mapping("discovery\nname");
    check(
        mapping.has_value(),
        "create discovery endpoint independently of client build mode"
    );
    if (!mapping)
        return;
    const auto pid = monitor::platform::current_process_id();
    const auto identity = monitor::platform::read_process_identity(pid);
    check(identity.has_value(), "read discovery process identity");
    if (!identity)
        return;
    auto watcher = monitor::watchdog_session::attach(*identity);
    check(watcher.has_value(), "attach existing observer before discovery");
    if (!watcher)
        return;
    auto& state = *mapping->get();
    const auto lease = state.watcher_lease_until_ns.load();
    std::string reason;
    const auto inspected
        = monitor::platform::inspect_process_mapping(pid, &reason);
    check(
        inspected && reason.empty()
            && inspected->identity.start_ticks == identity->start_ticks
            && inspected->name == "discovery\nname",
        "read-only inspection retains exact identity and bounded name"
    );
    const auto original_ticks = state.process_start_ticks;
    state.process_start_ticks = original_ticks + 1;
    check(
        !monitor::platform::inspect_process_mapping(pid, &reason)
            && reason.find("stale process identity") != std::string::npos,
        "discovery rejects an old PID incarnation"
    );
    state.process_start_ticks = original_ticks;
    const auto magic = state.magic;
    state.magic = 0;
    check(
        !monitor::platform::inspect_process_mapping(pid, &reason),
        "discovery rejects an incompatible layout"
    );
    state.magic = magic;
    state.process_lifecycle.store(
        static_cast<std::uint32_t>(monitor::lifecycle::starting)
    );
    check(
        !monitor::platform::inspect_process_mapping(pid, &reason),
        "discovery does not read an unpublished initialization"
    );
    state.process_lifecycle.store(
        static_cast<std::uint32_t>(monitor::lifecycle::running)
    );
    const int descriptor = ::shm_open(
        monitor::platform::shared_memory_name(pid).c_str(), O_RDWR | O_CLOEXEC,
        0
    );
    check(
        descriptor >= 0,
        "open owned endpoint for read-only permission regression"
    );
    if (descriptor >= 0) {
        check(
            ::fchmod(descriptor, S_IRUSR) == 0, "make owned endpoint read-only"
        );
        check(
            monitor::platform::inspect_process_mapping(pid, &reason)
                .has_value(),
            "discovery works without writable endpoint access"
        );
        check(
            ::fchmod(descriptor, S_IRUSR | S_IWUSR) == 0,
            "restore owned endpoint permissions"
        );
        ::close(descriptor);
    }
    const auto name = monitor::platform::shared_memory_name(pid);
    for (const auto& rejected :
         { name + "-malformed",
           name.substr(0, name.rfind('-') + 1) + "0" + std::to_string(pid),
           std::string("/monitor-watchdog-")
               + std::to_string(monitor::platform::current_user_id() + 1U) + "-"
               + std::to_string(pid) }) {
        const int file = ::shm_open(
            rejected.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC,
            S_IRUSR | S_IWUSR
        );
        check(file >= 0, "create exclusively owned discovery-name fixture");
        if (file < 0)
            continue;
        const auto inventory = monitor::discover_processes();
        const auto filename = rejected.substr(1);
        const bool reported = std::ranges::any_of(
            inventory.diagnostics, [&](const auto& message) {
                return message.starts_with(filename + ":");
            }
        );
        const bool foreign = filename.starts_with(
            "monitor-watchdog-"
            + std::to_string(monitor::platform::current_user_id() + 1U) + "-"
        );
        check(
            inventory.available && reported != foreign,
            "malformed current-user names are diagnosed; foreign names are "
            "ignored"
        );
        ::close(file);
        check(
            ::shm_unlink(rejected.c_str()) == 0,
            "only the fixture owner removes its rejected endpoint"
        );
    }
    std::ostringstream text, errors;
    static_cast<void>(monitor::run_console_list(text, errors));
    check(
        text.str().find("PID\tSTART_TICKS\tNAME\n") == 0
            && text.str().find("discovery?name") != std::string::npos,
        "list output identifies incarnations and cannot inject rows through "
        "names"
    );
    check(
        state.watcher_present.load() == 1
            && state.watcher_lease_until_ns.load() == lease,
        "inspection and enumeration neither renew nor clear another observer "
        "lease"
    );
    check(
        monitor::platform::open_process_mapping(pid).has_value(),
        "listing does not unlink the producer endpoint"
    );

    int ready[2], finish[2];
    if (::pipe(ready) != 0) {
        check(false, "create discovery ready pipe");
        return;
    }
    if (::pipe(finish) != 0) {
        ::close(ready[0]);
        ::close(ready[1]);
        check(false, "create discovery finish pipe");
        return;
    }
    const auto child = ::fork();
    if (child == 0) {
        ::close(ready[0]);
        ::close(finish[1]);
        auto producer
            = monitor::platform::create_process_mapping("discovery\nname");
        const char started = producer ? '1' : '0';
        static_cast<void>(::write(ready[1], &started, 1));
        char stop {};
        static_cast<void>(::read(finish[0], &stop, 1));
        // This raw platform producer also runs in Release, where the public
        // client is inert. Publish the same final lifecycle as client::stop().
        if (producer)
            producer->get()->process_lifecycle.store(
                static_cast<std::uint32_t>(monitor::lifecycle::clean_exit),
                std::memory_order_release
            );
        producer.reset();
        ::_exit(0);
    }
    ::close(ready[1]);
    ::close(finish[0]);
    if (child < 0) {
        ::close(ready[0]);
        ::close(finish[1]);
        check(false, "fork second discovery producer");
        return;
    }
    char started {};
    check(
        ::read(ready[0], &started, 1) == 1 && started == '1',
        "second real endpoint is published before enumeration"
    );
    ::close(ready[0]);
    const auto child_identity = monitor::platform::read_process_identity(
        static_cast<std::uint32_t>(child)
    );
    if (child_identity) {
        auto child_watcher = monitor::watchdog_session::attach(*child_identity);
        auto child_mapping
            = monitor::platform::open_process_mapping(child_identity->pid);
        const auto inventory = monitor::discover_processes();
        const auto same_names = std::ranges::count_if(
            inventory.processes, [&](const auto& process) {
                return process.name == "discovery\nname"
                    && (process.identity.pid == pid
                        || process.identity.pid == child_identity->pid);
            }
        );
        check(
            same_names == 2,
            "equal names retain two distinct process identities without "
            "arbitrary selection"
        );
        check(
            std::ranges::is_sorted(
                inventory.processes, {},
                [](const auto& process) { return process.identity.pid; }
            ),
            "discovery order is deterministic"
        );
        check(
            child_watcher && child_mapping,
            "attach independent observer to second process"
        );
        if (child_watcher && child_mapping) {
            const auto child_lease
                = child_mapping->get()->watcher_lease_until_ns.load();
            watcher->release();
            static_cast<void>(monitor::discover_processes());
            check(
                child_mapping->get()->watcher_present.load() == 1
                    && child_mapping->get()->watcher_lease_until_ns.load()
                        == child_lease,
                "detaching A and listing cannot disturb B's lease"
            );
        }
    } else {
        check(false, "second producer identity is available");
    }
    monitor::observation_registry registry;
    std::optional<monitor::platform::shared_mapping> retained_child;
    if (child_identity) {
        check(
            registry.attach(*identity) && registry.attach(*child_identity),
            "registry owns two live exact process incarnations"
        );
        retained_child
            = monitor::platform::open_process_mapping(child_identity->pid);
        check(
            registry.size() == 2 && retained_child,
            "registry stores one owner per selection without a process cap"
        );
        auto stale = *identity;
        ++stale.start_ticks;
        check(
            !registry.attach(stale) && registry.size() == 2,
            "a stale incarnation cannot replace the selected process"
        );
        check(
            !registry.detach(stale) && registry.size() == 2,
            "detaching an unmatched incarnation cannot erase the current one"
        );
        const auto lease_a = state.watcher_lease_until_ns.load();
        check(
            registry.attach(*identity) && registry.size() == 2
                && state.watcher_lease_until_ns.load() == lease_a,
            "repeated attachment reuses its owner without renewing a lease"
        );
        auto selections = registry.identities();
        check(
            selections.size() == 2
                && std::ranges::is_sorted(
                    selections, {}, [](const auto& item) { return item.pid; }
                ),
            "registry identities have deterministic order"
        );
        const auto snapshot = registry.poll();
        check(
            snapshot.size() == 2
                && std::ranges::all_of(
                    snapshot,
                    [](const auto& observation) {
                        return observation.status
                            == monitor::observation_status::sampled
                            && observation.sample && !observation.cpu_percent;
                    }
                ),
            "each registry owner establishes its own first CPU baseline"
        );
        static_cast<void>(registry.poll());
        check(
            snapshot.size() == 2 && !snapshot[0].cpu_percent
                && !snapshot[1].cpu_percent,
            "returned value observations survive later polling"
        );
        monitor::observation_registry moved(std::move(registry));
        registry = std::move(moved);
        if (retained_child) {
            const auto lease_b
                = retained_child->get()->watcher_lease_until_ns.load();
            check(
                registry.detach(*identity) && registry.size() == 1
                    && state.watcher_present.load() == 0
                    && retained_child->get()->watcher_present.load() == 1
                    && retained_child->get()->watcher_lease_until_ns.load()
                        == lease_b,
                "moving owners and detaching A preserve B's exact lease"
            );
        }
        monitor::process_discovery names;
        names.available = true;
        names.processes
            = { { *identity, "same" }, { *child_identity, "same" } };
        const auto ambiguous = monitor::select_process_name(names, "same");
        check(
            !ambiguous.identity
                && ambiguous.error.find("ambiguous") != std::string::npos,
            "equal names require explicit identity selection"
        );
        names.processes.pop_back();
        const auto unique = monitor::select_process_name(names, "same");
        check(
            unique.identity && unique.error.empty()
                && unique.identity->pid == identity->pid
                && unique.identity->start_ticks == identity->start_ticks,
            "unique exact names retain the selected incarnation"
        );
        check(
            !monitor::select_process_name(names, "sam").identity
                && !monitor::select_process_name(names, "").identity,
            "name selection neither matches prefixes nor accepts empty names"
        );
        names.diagnostics.emplace_back("an endpoint could not be inspected");
        const auto incomplete = monitor::select_process_name(names, "same");
        check(
            !incomplete.identity
                && incomplete.error.find("incomplete") != std::string::npos,
            "incomplete discovery cannot prove a name unambiguous"
        );
        names.diagnostics.clear();
        names.available = false;
        check(
            !monitor::select_process_name(names, "same").identity,
            "unavailable discovery cannot select a friendly name"
        );
    }
    if (child_identity) {
        registry.clear();
        check(
            registry.wait_for_exit(0),
            "empty registry has no pending lifetime wait"
        );
        monitor::process_discovery cohort;
        cohort.available = true;
        cohort.processes = { { *identity, "discovery\nname" },
                             { *child_identity, "discovery\nname" } };
        monitor::console_options options;
        options.all = true;
        options.once = true;
        std::atomic_bool stop { false };
        std::ostringstream out, err;
        check(
            monitor::run_console_watch_group(options, cohort, stop, out, err)
                    == 0
                && err.str().empty(),
            "all/once uses real independent owners for the supplied live cohort"
        );
        const auto parent_label = "[" + std::to_string(identity->pid) + "]";
        const auto child_label
            = "[" + std::to_string(child_identity->pid) + "]";
        check(
            out.str().find(parent_label) != std::string::npos
                && out.str().find(child_label) != std::string::npos,
            "group output contains both actual process observations"
        );
        check(
            state.watcher_present.load() == 0 && retained_child
                && retained_child->get()->watcher_present.load() == 0,
            "once completion releases every cohort lease without unlinking "
            "endpoints"
        );
        options.all = false;
        options.name = "discovery\nname";
        out.str("");
        err.str("");
        check(
            monitor::run_console_watch_group(options, cohort, stop, out, err)
                    == 1
                && err.str().find("ambiguous") != std::string::npos
                && out.str().empty(),
            "CLI name ambiguity fails before observation"
        );
        cohort.processes.pop_back();
        out.str("");
        err.str("");
        check(
            monitor::run_console_watch_group(options, cohort, stop, out, err)
                    == 0
                && err.str().empty()
                && out.str().find(parent_label) != std::string::npos
                && out.str().find(child_label) == std::string::npos,
            "unique-name once selection observes exactly its real process"
        );
        options.name = {};
        options.all = true;
        auto stale_child = *child_identity;
        ++stale_child.start_ticks;
        cohort.processes.push_back({ stale_child, "discovery\nname" });
        auto independent = monitor::watchdog_session::attach(*child_identity);
        if (independent && retained_child) {
            const auto independent_lease
                = retained_child->get()->watcher_lease_until_ns.load();
            out.str("");
            err.str("");
            check(
                monitor::run_console_watch_group(
                    options, cohort, stop, out, err
                ) == 1
                    && err.str().find("selected process incarnation")
                        != std::string::npos,
                "a stale cohort member produces an explicit attach failure"
            );
            check(
                state.watcher_present.load() == 0
                    && retained_child->get()->watcher_present.load() == 1
                    && retained_child->get()->watcher_lease_until_ns.load()
                        == independent_lease,
                "partial attach failure releases A without clearing an "
                "unrelated B lease"
            );
        } else
            check(
                false,
                "independent lease is available for partial-attach regression"
            );
        independent.reset();
        cohort.diagnostics.emplace_back("owned fixture inspection unavailable");
        out.str("");
        err.str("");
        check(
            monitor::run_console_watch_group(options, cohort, stop, out, err)
                    == 1
                && err.str().find("owned fixture inspection unavailable")
                    != std::string::npos,
            "incomplete cohort discovery fails with its diagnostic"
        );
        cohort.diagnostics.clear();
        cohort.processes.clear();
        out.str("");
        err.str("");
        check(
            monitor::run_console_watch_group(options, cohort, stop, out, err)
                    == 1
                && err.str().find("no compatible processes")
                    != std::string::npos,
            "an empty observation cohort is reported explicitly"
        );
        stop.store(true);
        out.str("");
        err.str("");
        check(
            monitor::run_console_watch_group(options, cohort, stop, out, err)
                    == 0
                && err.str().empty(),
            "cancellation before group startup does not acquire leases"
        );
        check(
            registry.attach(*child_identity),
            "restore child owner for retained-exit regression"
        );
        check(
            !registry.wait_for_exit(0),
            "live cohort supports a nonblocking lifetime wait"
        );
    }
    ::close(finish[1]);
    int status {};
    check(
        ::waitpid(child, &status, 0) == child && WIFEXITED(status)
            && WEXITSTATUS(status) == 0,
        "discovery child shuts down and removes its own endpoint"
    );
    if (child_identity) {
        const auto exited = registry.poll();
        check(
            exited.size() == 1 && exited[0].identity.pid == child_identity->pid
                && exited[0].status == monitor::observation_status::exited
                && exited[0].watchdog.process_lifecycle
                    == monitor::lifecycle::clean_exit,
            "registry retains final lifecycle evidence until explicit removal"
        );
        check(
            !registry.attach(*child_identity) && registry.size() == 1,
            "a repeated selection cannot revive an exited process"
        );
        registry.clear();
        check(
            registry.size() == 0 && retained_child
                && retained_child->get()->watcher_present.load() == 0,
            "clearing the registry releases only its owned remaining lease"
        );
    }
    const auto final = monitor::discover_processes();
    check(
        std::ranges::none_of(
            final.processes,
            [&](const auto& process) {
                return process.identity.pid
                    == static_cast<std::uint32_t>(child);
            }
        ),
        "exited producer is absent from the next inventory"
    );
#else
    const auto inventory = monitor::discover_processes();
    check(
        !inventory.available && !inventory.diagnostics.empty(),
        "unsupported discovery is explicit"
    );
#endif
}

} // namespace monitor_test

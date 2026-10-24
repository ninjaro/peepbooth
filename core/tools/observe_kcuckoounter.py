"""Observe a disposable real Kcuckoounter process through the current Monitor SDK.

Linux/offscreen validation only. The preload driver invokes ordinary Qt controls;
it never publishes heartbeats. All settings, recordings and builds stay in output.
No production application flag, artwork or rendering policy is changed.
"""

import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import time

import inspect_recording


def events(path):
    if not path.exists():
        return []
    # The producer may be between its JSON write and newline write.
    lines = path.read_text().split("\n")[:-1]
    return [json.loads(line) for line in lines]


def require(value, message):
    if not value:
        raise RuntimeError(message)


def wait_child(child, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        pid, status, usage = os.wait4(child.pid, os.WNOHANG)
        if pid:
            child.returncode = os.waitstatus_to_exitcode(status)
            return dict(cpu_seconds=usage.ru_utime + usage.ru_stime,
                        peak_rss_kib=usage.ru_maxrss)
        time.sleep(0.02)
    raise TimeoutError("child did not finish")


def validate_events(rows, instrumented):
    require(rows and rows[0]["event"] == "ready", "no actual main window")
    require(all(row.get("found", True) for row in rows), "UI control not found")
    require(not any(row["event"].startswith("error") for row in rows), "driver error")
    require(any(row["event"] == "window_destroyed" for row in rows), "no normal teardown")
    require(all(row["monitor_mapping"] == instrumented for row in rows),
            "producer availability disagrees with requested configuration")


def validate_observations(observations):
    for item in observations:
        if item["progress"] is not None:
            require(all(not channel["active"] for channel in item["progress"]["channels"]
                        if channel["id"] != "gui"), "unexpected background progress claim")
        if item["status"] == "sample_unavailable":
            require(item["memory"] is None and item["cpu_percent"] is None,
                    "unavailable resources became fabricated values")


def busy_progress(rows):
    starts = []
    deltas = []
    for row in rows:
        if row["event"] == "raster_busy":
            if row["busy"]:
                starts.append(row)
            elif starts:
                start = starts.pop()
                deltas.append(row.get("gui_sequence", 0) - start.get("gui_sequence", 0))
    return dict(completed_intervals=len(deltas),
                intervals_with_gui_progress=sum(delta > 0 for delta in deltas))


def summarize_recording(path):
    # Use the existing independent reader before deriving any integration facts.
    result = inspect_recording.inspect(path, max_bytes=16 * 1024 * 1024, max_frame_bytes=256 * 1024)
    require(result["status"] == "finalized", "recording did not finalize")
    checkpoints = [json.loads(line) for line in path.read_text().splitlines()
                   if json.loads(line)["type"] == "checkpoint"]
    observations = [item["observation"] for item in checkpoints]
    validate_observations(observations)
    episodes = {(item["kind"], item["channel"], item["first"]["observed_at_ns"]): item for point in checkpoints
                for item in point["active"] + point["closed"]}
    return dict(status=result["status"], checkpoints=len(checkpoints),
                sampled=sum(item["status"] == "sampled" for item in observations),
                unavailable=sum(item["status"] == "sample_unavailable" for item in observations),
                cpu_samples=sum(item["cpu_percent"] is not None for item in observations),
                health=sorted({item["health"] for item in observations if item["health"]}),
                incidents=list(episodes.values()), terminal=result["terminal"],
                last_checkpoint=result["last_checkpoint"])


def build_driver(output, sdk):
    source = output / "driver-source"
    source.mkdir()
    fixture = Path(__file__).parent / "fixtures/kcuckoounter_driver.cpp.in"
    (source / "kcuckoounter_driver.cpp").write_bytes(fixture.read_bytes())
    # Only Qt and the installed current shared layout; no second producer.
    (source / "CMakeLists.txt").write_text("""cmake_minimum_required(VERSION 3.20)
project(kcuckoounter_driver LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(Qt6 REQUIRED COMPONENTS Core Widgets)
add_library(kcuckoounter_driver SHARED kcuckoounter_driver.cpp)
target_link_libraries(kcuckoounter_driver PRIVATE Qt6::Core Qt6::Widgets)
target_include_directories(kcuckoounter_driver PRIVATE "${MONITOR_INCLUDE}")
target_include_directories(kcuckoounter_driver SYSTEM PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/kcuckoounter_driver_autogen/include")
target_compile_options(kcuckoounter_driver PRIVATE -Wall -Wextra -Werror)
""")
    build = output / "driver-build"
    with (output / "driver-build.log").open("w") as log:
        subprocess.run(["cmake", "-S", str(source), "-B", str(build), "-G", "Ninja",
                        "-DCMAKE_CXX_COMPILER=clang++", f"-DMONITOR_INCLUDE={sdk / 'include'}"],
                       stdout=log, stderr=subprocess.STDOUT, check=True)
        subprocess.run(["cmake", "--build", str(build), "--parallel", "2"],
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    return build / "libkcuckoounter_driver.so"


def run(args):
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    driver = args.driver.resolve() if args.driver else build_driver(output, args.sdk.resolve())
    for name in ("config", "data", "cache", "state", "runtime", "tmp"):
        (output / name).mkdir(mode=0o700)
    fifo = output / "commands"
    os.mkfifo(fifo, 0o600)
    trace = output / "events.jsonl"
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen", QT_SCALE_FACTOR=str(args.scale),
               LD_PRELOAD=str(driver), KC_DRIVER_LOG=str(trace), KC_DRIVER_FIFO=str(fifo),
               XDG_RUNTIME_DIR=str(output / "runtime"), TMPDIR=str(output / "tmp"),
               LC_ALL="C", LANGUAGE="C")
    for key in ("CONFIG", "DATA", "CACHE", "STATE"):
        env[f"XDG_{key}_HOME"] = str(output / key.lower())
    watcher = None
    commands = None
    app = None
    suspended = False
    start = time.monotonic()
    watcher_usage = None
    try:
        with (output / "application.log").open("w") as app_log, \
                (output / "watcher.log").open("w") as watch_log:
            app = subprocess.Popen([str(args.application.resolve())], env=env,
                                   stdout=app_log, stderr=subprocess.STDOUT)

            def wait_for(predicate):
                deadline = time.monotonic() + args.timeout
                while not predicate():
                    require(app.poll() is None, "application exited before the requested action")
                    require(time.monotonic() < deadline, "validation action timed out")
                    time.sleep(0.02)

            wait_for(lambda: bool(events(trace)))
            commands = fifo.open("w")
            command_id = 0

            def send(action, **values):
                nonlocal command_id
                command_id += 1
                commands.write(json.dumps(dict(action=action, id=command_id, **values)) + "\n")
                commands.flush()
                if action != "close":
                    wait_for(lambda: any(row["id"] == command_id for row in events(trace)))
                    require(all(row.get("found", True) for row in events(trace)), "UI control absent")

            if args.mode != "disconnected":
                discovery = subprocess.run([str(args.watchdog.resolve()), "--list"],
                                           capture_output=True, text=True, check=True)
                (output / "discovery.log").write_text(discovery.stdout + discovery.stderr)
                require(any(line.split() and line.split()[0] == str(app.pid)
                            for line in discovery.stdout.splitlines()), "target not discovered")
                command = [str(args.watchdog.resolve()), "--pid", str(app.pid),
                           "--interval-ms", "50", "--history", "16"]
                if args.mode == "record":
                    command += ["--record", str(output / "session.ndjson"),
                                "--record-bytes", "16777216", "--record-checkpoints", "1000",
                                "--record-frame-bytes", "262144"]
                watcher = subprocess.Popen(command, stdout=watch_log, stderr=subprocess.STDOUT)
                wait_for(lambda: "HEALTHY" in (output / "watcher.log").read_text())
            time.sleep(args.dwell)
            send("mark", label="healthy_idle")
            send("button", text="Continue")
            send("trigger", text="Start")
            time.sleep(args.dwell)
            send("trigger", text="Settings")  # normal application pause-for-dialog
            send("theme", source="cards_42.svg")
            send("button", text="Save")
            send("resize", width=1100, height=850)
            time.sleep(args.dwell)
            send("mark", label="complex_source")
            send("theme", source="cards_4.svg")
            send("button", text="Save and close")
            send("resize", width=900, height=700)
            time.sleep(args.dwell)
            send("mark", label="source_switch")
            if args.suspend:
                require(args.mode == "record", "suspension requires a recorded observer")
                send("mark", label="whole_process_suspend")
                os.kill(app.pid, signal.SIGSTOP)
                suspended = True
                # Observe the existing timeout policy; no injected heartbeat or new timeout.
                partial = output / "session.ndjson.partial"
                wait_for(lambda: '"kind":"overdue_progress"' in partial.read_text())
                os.kill(app.pid, signal.SIGCONT)
                suspended = False
                time.sleep(args.dwell)
                send("mark", label="whole_process_resumed")
            if args.detach:
                require(watcher is not None, "detachment requires a watcher")
                watcher.send_signal(signal.SIGINT)
                watcher_usage = wait_child(watcher, args.timeout)
                time.sleep(args.dwell)
                send("mark", label="watcher_detached_application_alive")
            send("close")
            app_usage = wait_child(app, args.timeout)
            if watcher is not None and watcher.returncode is None:
                watcher_usage = wait_child(watcher, args.timeout)
        validate_events(events(trace), not args.release)
        require(app.returncode == 0, f"application exit {app.returncode}")
        summary = dict(application_returncode=app.returncode,
                       watcher_returncode=watcher.returncode if watcher else None,
                       mode=args.mode, scale=args.scale, release=args.release,
                       dwell_seconds=args.dwell, elapsed_seconds=time.monotonic() - start,
                       application_usage=app_usage, watcher_usage=watcher_usage,
                       busy_progress=busy_progress(events(trace)))
        if args.mode == "record":
            summary["recording"] = summarize_recording(output / "session.ndjson")
            require(summary["recording"]["terminal"]["observation_exit_status"] == watcher.returncode,
                    "console and recording outcomes disagree")
        (output / "result.json").write_text(json.dumps(summary, indent=2) + "\n")
        print(json.dumps({key: value for key, value in summary.items() if key != "recording"}))
    finally:
        if app is not None and suspended:
            os.kill(app.pid, signal.SIGCONT)
        if commands is not None:
            commands.close()
        for child in (watcher, app):
            if child is not None and child.poll() is None:
                child.terminate()
                child.wait(timeout=args.timeout)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--application", type=Path, required=True)
    parser.add_argument("--watchdog", type=Path, required=True)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--sdk", type=Path)
    group.add_argument("--driver", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--mode", choices=("disconnected", "connected", "record"), default="record")
    parser.add_argument("--scale", type=int, choices=(1, 2), default=1)
    parser.add_argument("--dwell", type=float, required=True, help="test pacing, not watchdog policy")
    parser.add_argument("--timeout", type=float, default=60, help="validation safety limit")
    parser.add_argument("--detach", action="store_true")
    parser.add_argument("--suspend", action="store_true", help="suspend entire owned child, not just GUI")
    parser.add_argument("--release", action="store_true")
    args = parser.parse_args()
    if args.dwell <= 0 or args.timeout <= 0:
        parser.error("dwell and timeout must be positive")
    if args.release and args.mode != "disconnected":
        parser.error("Release has no watcher endpoint; use disconnected")
    if args.detach and args.mode == "disconnected":
        parser.error("detachment requires a watcher")
    if args.suspend and args.mode != "record":
        parser.error("suspension requires a recorded observer")
    run(args)


if __name__ == "__main__":
    main()

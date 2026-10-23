"""Validate current Peepbooth NDJSON recordings; bounded streaming, no replay.

Exit 0: finalized capture; 2: valid partial/limit-ended capture; 1: invalid/error.
Input limits are mandatory even when the untrusted header declares its own.
"""
import argparse
import json
import math
from pathlib import Path
import sys
import zlib

CHANNELS = ("main", "gui", "core", "worker")
STATUSES = ("not_attached", "sampled", "sample_unavailable", "identity_changed", "exited")
HEALTH = ("AWAITING_HEARTBEAT", "IDLE", "EVIDENCE_UNAVAILABLE", "HEALTHY", "DEGRADED", "GUI_STALLED", "STALLED_IDLE", "STALLED_BUSY")
KINDS = ("overdue_progress", "sampled_rss_growth", "unavailable_evidence", "process_exit")
STATES = ("open", "resolved", "interrupted", "ended_unknown", "observed")
ENDINGS = ("none", "condition_cleared", "progress_observed", "expectation_retired", "expectation_replaced", "observation_gap", "observer_detached", "reattached", "process_exit", "identity_changed", "producer_stopped")
OUTCOMES = ("process_exited", "once", "cancelled", "identity_changed", "observation_error", "limit_reached")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def keys(value, names):
    require(isinstance(value, dict) and set(value) == set(names.split()), "incorrect object fields")


def integer(value, minimum=0, maximum=(1 << 64) - 1):
    require(type(value) is int and minimum <= value <= maximum, "integer outside current format")


def number(value):
    require(type(value) in (int, float) and math.isfinite(value), "non-finite or invalid metric")


def identity(value):
    keys(value, "pid start_ticks")
    integer(value["pid"], 1, (1 << 32) - 1)
    integer(value["start_ticks"])


def observation(value, target):
    keys(value, "identity observed_at_ns status memory cpu_percent health sampled_at_ns trend progress")
    identity(value["identity"])
    require(value["identity"] == target, "foreign process incarnation")
    integer(value["observed_at_ns"])
    require(value["status"] is None or value["status"] in STATUSES, "invalid observation status")
    require(value["health"] is None or value["health"] in HEALTH, "invalid health")
    if value["cpu_percent"] is not None:
        number(value["cpu_percent"])
        require(value["cpu_percent"] >= 0, "negative CPU")
    if value["sampled_at_ns"] is not None:
        integer(value["sampled_at_ns"])
        require(value["sampled_at_ns"] <= value["observed_at_ns"], "sample from future")
    if value["memory"] is not None:
        keys(value["memory"], "rss_bytes high_water_bytes swap_bytes")
        for item in value["memory"].values():
            integer(item)
    if value["trend"] is not None:
        trend = value["trend"]
        keys(trend, "duration_ns rss_delta_bytes bytes_per_second sustained_growth")
        integer(trend["duration_ns"])
        integer(trend["rss_delta_bytes"], -(1 << 63), (1 << 63) - 1)
        number(trend["bytes_per_second"])
        require(type(trend["sustained_growth"]) is bool, "invalid trend flag")
    if value["status"] != "sampled":
        require(all(value[name] is None for name in ("memory", "cpu_percent", "health", "sampled_at_ns", "trend")), "unavailable observation contains fabricated samples")
    else:
        require(value["memory"] is not None and value["sampled_at_ns"] is not None, "sampled observation lacks sample")
    if value["progress"] is not None:
        progress = value["progress"]
        keys(progress, "lifecycle observer_since_ns stale_after_ns channels")
        integer(progress["lifecycle"], 1, 4)
        integer(progress["observer_since_ns"])
        integer(progress["stale_after_ns"])
        require(isinstance(progress["channels"], list) and len(progress["channels"]) == 4, "incorrect fixed channels")
        for item, channel in zip(progress["channels"], CHANNELS):
            keys(item, "id active coherent timestamp_ns sequence expected_since_ns")
            require(item["id"] == channel, "invalid channel identity/order")
            require(type(item["active"]) is bool and type(item["coherent"]) is bool, "invalid channel flags")
            for name in ("timestamp_ns", "sequence", "expected_since_ns"):
                integer(item[name])
    if value["status"] is None:
        require(value["progress"] is None, "gap contains producer evidence")


def incident(value, target, now, active):
    keys(value, "kind channel state ending matching_observations first last closing")
    require(value["kind"] in KINDS and value["state"] in STATES and value["ending"] in ENDINGS, "invalid incident enum")
    require(value["channel"] in CHANNELS if value["kind"] == "overdue_progress" else value["channel"] is None, "invalid incident domain")
    integer(value["matching_observations"], 1)
    for name in ("first", "last"):
        observation(value[name], target)
    first, last = value["first"]["observed_at_ns"], value["last"]["observed_at_ns"]
    require(first <= last <= now, "incident evidence outside checkpoint")
    require((value["state"] == "open") == active, "incorrect incident collection")
    if active:
        require(value["closing"] is None and value["ending"] == "none", "open incident has ending")
    else:
        require(value["closing"] is not None and value["ending"] != "none", "closed incident lacks ending")
        observation(value["closing"], target)
        require(last <= value["closing"]["observed_at_ns"] <= now, "invalid ending evidence time")


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, "duplicate JSON key")
        result[key] = value
    return result


def inspect(path, *, max_bytes, max_frame_bytes):
    """Return a compact summary, preserving unknown tails and last evidence.

    Memory is bounded by the caller's frame limit, not recording length.
    A finalized limit record describes only the recorded prefix, not later live
    observation. CRC32 detects corruption; it is not an authenticity signature.
    """
    integer(max_bytes, 1)
    integer(max_frame_bytes, 1)
    header = terminal = last = None
    total = crc = count = previous_time = previous_history_drop = previous_incident_drop = 0
    samples = gaps = unknown_cpu = 0
    last_status = None
    torn = False
    with Path(path).open("rb") as stream:
        while True:
            # Enforce independent limits before reading/decoding untrusted data.
            line = stream.readline(min(max_frame_bytes, max_bytes - total) + 1)
            if not line:
                break
            require(len(line) <= max_frame_bytes and total + len(line) <= max_bytes, "reader input limit exceeded")
            require(terminal is None, "data after terminal record")
            total += len(line)
            if header is not None:
                require(total <= header["limits"]["bytes"] and len(line) <= header["limits"]["frame_bytes"], "declared recording limit exceeded")
            if not line.endswith(b"\n"):
                torn = True
                break
            try:
                value = json.loads(line, object_pairs_hook=unique_object, parse_constant=lambda _: (_ for _ in ()).throw(ValueError("non-finite JSON")))
            except (UnicodeError, json.JSONDecodeError, RecursionError) as error:
                raise ValueError("malformed complete frame") from error
            require(isinstance(value, dict), "frame must be object")
            if header is None:
                keys(value, "type format version target watcher started_ns started_utc_ns interval_ms stale_after_ns history_capacity limits environment")
                require(value["type"] == "session" and value["format"] == "peepbooth-observations" and type(value["version"]) is int and value["version"] == 1, "unsupported recording format")
                identity(value["target"])
                identity(value["watcher"])
                for name in ("started_ns", "started_utc_ns", "interval_ms", "stale_after_ns", "history_capacity"):
                    integer(value[name], 1)
                keys(value["limits"], "bytes checkpoints frame_bytes")
                for item in value["limits"].values():
                    integer(item, 1)
                keys(value["environment"], "boot_id system release machine")
                require(all(item is None or isinstance(item, str) for item in value["environment"].values()), "invalid environment")
                header = value
                previous_time = header["started_ns"]
            elif value.get("type") == "checkpoint":
                keys(value, "type sequence observation history_retained history_overwritten incidents_overwritten active closed")
                integer(value["sequence"], 1)
                require(value["sequence"] == count + 1 and value["sequence"] <= header["limits"]["checkpoints"], "missing or excess checkpoint")
                observation(value["observation"], header["target"])
                now = value["observation"]["observed_at_ns"]
                require(now >= previous_time, "backwards observation time")
                integer(value["history_retained"], 1, header["history_capacity"])
                for name in ("history_overwritten", "incidents_overwritten"):
                    integer(value[name])
                require(value["history_overwritten"] >= previous_history_drop and value["incidents_overwritten"] >= previous_incident_drop, "backwards rollover counts")
                require(isinstance(value["active"], list) and len(value["active"]) <= 6, "unbounded active incidents")
                require(isinstance(value["closed"], list) and len(value["closed"]) <= header["history_capacity"], "unbounded closed incidents")
                domains = set()
                for item in value["active"]:
                    incident(item, header["target"], now, True)
                    domain = (item["kind"], item["channel"])
                    require(domain not in domains, "duplicate active incident")
                    domains.add(domain)
                for item in value["closed"]:
                    incident(item, header["target"], now, False)
                if value["observation"]["status"] is not None:
                    last_status = value["observation"]["status"]
                samples += value["observation"]["status"] == "sampled"
                gaps += value["observation"]["status"] is None
                unknown_cpu += value["observation"]["cpu_percent"] is None
                previous_time, previous_history_drop, previous_incident_drop = now, value["history_overwritten"], value["incidents_overwritten"]
                count += 1
                last = value
            elif value.get("type") == "end":
                keys(value, "type outcome limit checkpoints prefix_bytes crc32 ended_ns first_unrecorded_ns discarded_lower_bound observation_exit_status")
                for name in ("checkpoints", "prefix_bytes", "crc32", "ended_ns", "discarded_lower_bound"):
                    integer(value[name])
                require(value["outcome"] in OUTCOMES and (value["observation_exit_status"] is None or type(value["observation_exit_status"]) is int and value["observation_exit_status"] in (0, 1)), "invalid terminal outcome")
                require(value["checkpoints"] == count and value["prefix_bytes"] == total - len(line) and value["crc32"] == crc, "recording checksum/count mismatch")
                require(value["ended_ns"] >= previous_time, "backwards terminal time")
                if value["outcome"] == "limit_reached":
                    require(value["limit"] in ("bytes", "checkpoints", "frame_size") and value["discarded_lower_bound"] == 1 and value["observation_exit_status"] is None, "invalid limit ending")
                    integer(value["first_unrecorded_ns"], previous_time, value["ended_ns"])
                else:
                    require(type(value["observation_exit_status"]) is int and value["limit"] is None and value["first_unrecorded_ns"] is None and value["discarded_lower_bound"] == 0, "unexpected missing evidence")
                    require(last is not None and last["observation"]["status"] is None and not last["active"], "finalized observation lacks detach boundary")
                if value["outcome"] in ("process_exited", "identity_changed"):
                    require(last_status == ("exited" if value["outcome"] == "process_exited" else "identity_changed"), "terminal lifetime outcome lacks observation")
                if value["outcome"] == "observation_error":
                    require(value["observation_exit_status"] == 1, "observation failure cannot report success")
                terminal = value
            else:
                raise ValueError("unsupported frame type")
            require(total <= header["limits"]["bytes"] and len(line) <= header["limits"]["frame_bytes"], "declared recording limit exceeded")
            if terminal is None:
                crc = zlib.crc32(line, crc)
    require(header is not None, "no valid session header")
    complete = terminal is not None and not str(path).endswith(".partial")
    return {"status": "finalized" if complete and terminal["outcome"] != "limit_reached" else "limited" if complete else "partial", "session": header, "terminal": terminal, "checkpoints": count, "sampled": samples, "gaps": gaps, "unavailable_cpu": unknown_cpu, "torn_tail": torn, "last_checkpoint": last, "bytes_read": total}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", type=Path)
    parser.add_argument("--max-bytes", type=int, required=True)
    parser.add_argument("--max-frame-bytes", type=int, required=True)
    args = parser.parse_args()
    try:
        result = inspect(args.path, max_bytes=args.max_bytes, max_frame_bytes=args.max_frame_bytes)
        print(json.dumps(result, indent=2, allow_nan=False))
        return 0 if result["status"] == "finalized" else 2
    except (OSError, ValueError, OverflowError, RecursionError) as error:
        print(f"recording invalid: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())

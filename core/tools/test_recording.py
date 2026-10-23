"""Deterministic current-format reader checks, plus actual C++ writer fixtures."""
import json
import os
from pathlib import Path
import tempfile
import unittest
import zlib

from inspect_recording import inspect


def header():
    return dict(type="session", format="peepbooth-observations", version=1,
                target=dict(pid=42, start_ticks=99), watcher=dict(pid=43, start_ticks=100),
                started_ns=1, started_utc_ns=1, interval_ms=50, stale_after_ns=100,
                history_capacity=2, limits=dict(bytes=1048576, checkpoints=100, frame_bytes=65536),
                environment=dict(boot_id=None, system="Linux", release=None, machine=None))


def record(time=2, status="sampled"):
    return dict(identity=header()["target"], observed_at_ns=time, status=status,
                memory=dict(rss_bytes=4096, high_water_bytes=8192, swap_bytes=0) if status == "sampled" else None,
                cpu_percent=None, health="IDLE" if status == "sampled" else None,
                sampled_at_ns=time if status == "sampled" else None, trend=None, progress=None)


def checkpoint(time=2):
    return dict(type="checkpoint", sequence=1, observation=record(time), history_retained=1,
                history_overwritten=0, incidents_overwritten=0, active=[], closed=[])


def encode(values, outcome="once"):
    data = b"".join((json.dumps(value, separators=(",", ":"), allow_nan=False) + "\n").encode() for value in values)
    footer = dict(type="end", outcome=outcome, limit=None, checkpoints=len(values) - 1,
                  prefix_bytes=len(data), crc32=zlib.crc32(data), ended_ns=100,
                  first_unrecorded_ns=None, discarded_lower_bound=0, observation_exit_status=0)
    return data + (json.dumps(footer) + "\n").encode()


class RecordingTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="monitor-reader-")
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "session.ndjson"
        self.values = [header(), checkpoint()]
        self.values[1]["observation"] = record(status=None)

    def read(self, data=None, **kwargs):
        self.path.write_bytes(encode(self.values) if data is None else data)
        return inspect(self.path, max_bytes=kwargs.get("max_bytes", 1048576), max_frame_bytes=kwargs.get("max_frame_bytes", 65536))

    def test_complete(self):
        value = self.read()
        self.assertEqual(value["status"], "finalized")
        self.assertEqual(value["gaps"], 1)
        self.assertEqual(value["unavailable_cpu"], 1)

    def test_empty_and_missing_header(self):
        for data in (b"", b"{", b'{}\n'):
            with self.subTest(data=data), self.assertRaises(ValueError):
                self.read(data)

    def test_partial_and_torn_tail(self):
        complete = encode(self.values)
        prefix = complete[:complete.rfind(b'{"type": "end"')]
        for suffix in (b"", b'{"type":"end",'):
            value = self.read(prefix + suffix)
            self.assertEqual(value["status"], "partial")
            self.assertEqual(value["checkpoints"], 1)
            self.assertEqual(value["torn_tail"], bool(suffix))

    def test_partial_name_never_finalized(self):
        self.path = self.path.with_suffix(".ndjson.partial")
        self.assertEqual(self.read()["status"], "partial")

    def test_crc_and_trailing_frames(self):
        data = encode(self.values)
        for broken in (data.replace(b'"crc32": ', b'"crc32": 1'), data + b'{}\n', data.replace(b'"pid":42', b'"pid":44')):
            with self.subTest(data=broken), self.assertRaises(ValueError):
                self.read(broken)

    def test_torn_tail_still_obeys_limits(self):
        self.values[0]["limits"]["frame_bytes"] = 4096
        data = (json.dumps(self.values[0]) + "\n").encode() + b"x" * 5000
        with self.assertRaises(ValueError):
            self.read(data)

    def test_unknown_versions_and_fields(self):
        for version in (0, 2, True, "1"):
            self.values[0]["version"] = version
            with self.subTest(version=version), self.assertRaises(ValueError):
                self.read()
        self.values[0] = header()
        self.values[1]["payload"] = "forbidden"
        with self.assertRaises(ValueError):
            self.read()

    def test_identity_and_sequence(self):
        for name, changed in (("observation", dict(record(status=None), identity=dict(pid=42, start_ticks=100))), ("sequence", 2)):
            old = self.values[1][name]
            self.values[1][name] = changed
            with self.subTest(name=name), self.assertRaises(ValueError):
                self.read()
            self.values[1][name] = old

    def test_no_fabricated_unavailable(self):
        for status in (None, "sample_unavailable", "exited"):
            self.values[1]["observation"] = record(status=status)
            self.values[1]["observation"]["cpu_percent"] = 0
            with self.subTest(status=status), self.assertRaises(ValueError):
                self.read()

    def test_independent_reader_limits(self):
        for args in (dict(max_bytes=100), dict(max_frame_bytes=100)):
            with self.subTest(args=args), self.assertRaises(ValueError):
                self.read(**args)

    def test_declared_limits(self):
        for name, value in (("bytes", 100), ("frame_bytes", 100), ("checkpoints", 0)):
            self.values[0] = header()
            self.values[0]["limits"][name] = value
            with self.subTest(name=name), self.assertRaises(ValueError):
                self.read()

    def test_malformed_duplicate_nonfinite(self):
        for data in (b'{"type":"session","type":"session"}\n', b'{"metric":NaN}\n', b'{bad}\n', b'[]\n', b'\xff\n'):
            with self.subTest(data=data), self.assertRaises(ValueError):
                self.read(data)

    def test_backwards_time_and_rollover(self):
        prior = checkpoint(10)
        prior["history_overwritten"] = 3
        self.values.insert(1, prior)
        self.values[2]["sequence"] = 2
        with self.assertRaises(ValueError):
            self.read()
        self.values[2]["observation"]["observed_at_ns"] = 20
        with self.assertRaises(ValueError):
            self.read()
        self.values[2]["history_overwritten"] = 4
        self.assertEqual(self.read()["checkpoints"], 2)

    def test_invalid_incident(self):
        self.values[1]["active"] = [dict(kind="process_exit")]
        with self.assertRaises(ValueError):
            self.read()

    def test_terminal_needs_lifetime_evidence(self):
        for outcome in ("process_exited", "identity_changed", "observation_error"):
            with self.subTest(outcome=outcome), self.assertRaises(ValueError):
                self.read(encode(self.values, outcome))

    def test_limit_terminal(self):
        data = encode(self.values)
        prefix, ending = data.rsplit(b"\n", 2)[:2]
        footer = json.loads(ending)
        footer.update(outcome="limit_reached", limit="checkpoints", first_unrecorded_ns=100, discarded_lower_bound=1, observation_exit_status=None)
        self.assertEqual(self.read(prefix + b"\n" + json.dumps(footer).encode() + b"\n")["status"], "limited")
        footer["first_unrecorded_ns"] = None
        with self.assertRaises(ValueError):
            self.read(prefix + b"\n" + json.dumps(footer).encode() + b"\n")

    @unittest.skipUnless(os.environ.get("MONITOR_RECORDING_FIXTURES"), "run native writer with MONITOR_RECORDING_FIXTURES first")
    def test_actual_native_writer(self):
        directory = Path(os.environ["MONITOR_RECORDING_FIXTURES"])
        def read(name):
            return inspect(directory / name, max_bytes=1048576, max_frame_bytes=65536)
        value = read("complete.ndjson")
        self.assertEqual(value["status"], "finalized")
        self.assertEqual(value["checkpoints"], 6)
        self.assertEqual(value["session"]["environment"]["boot_id"], 'test"boot\n')
        self.assertEqual(value["last_checkpoint"]["history_overwritten"], 4)
        self.assertEqual(len(value["last_checkpoint"]["closed"]), 2)
        incident = value["last_checkpoint"]["closed"][0]
        self.assertEqual(incident["matching_observations"], 2)
        self.assertEqual(incident["ending"], "observer_detached")
        rollover = read("rollover.ndjson")["last_checkpoint"]
        self.assertEqual(rollover["history_overwritten"], 15)
        self.assertEqual(rollover["incidents_overwritten"], 6)
        self.assertEqual(len(rollover["closed"]), 2)
        interrupted = read("active-interrupted.ndjson.partial")
        self.assertEqual(interrupted["status"], "partial")
        self.assertEqual(len(interrupted["last_checkpoint"]["active"]), 1)
        self.assertEqual(interrupted["last_checkpoint"]["active"][0]["state"], "open")
        self.assertIsNone(interrupted["terminal"])
        for name, reason in (("count-limit", "checkpoints"), ("byte-limit", "bytes"), ("frame-limit", "frame_size")):
            value = read(name + ".ndjson")
            self.assertEqual(value["status"], "limited")
            self.assertEqual(value["terminal"]["limit"], reason)
        for name in ("interrupted", "foreign", "nonfinite", "publish-conflict", "io-error"):
            self.assertEqual(read(name + ".ndjson.partial")["status"], "partial")


if __name__ == "__main__":
    unittest.main()

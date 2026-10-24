"""Guards against false acceptance of real-application integration evidence."""

import tempfile
from pathlib import Path
import unittest

from observe_kcuckoounter import busy_progress, events, validate_events, validate_observations


class ApplicationEvidenceTests(unittest.TestCase):
    def test_release_has_no_endpoint(self):
        rows = [dict(event=name, monitor_mapping=False)
                for name in ("ready", "window_destroyed")]
        validate_events(rows, False)
        with self.assertRaisesRegex(RuntimeError, "availability"):
            validate_events(rows, True)

    def test_missing_control_or_teardown_is_not_success(self):
        rows = [dict(event="ready", monitor_mapping=True),
                dict(event="theme", found=False, monitor_mapping=True)]
        with self.assertRaisesRegex(RuntimeError, "not found"):
            validate_events(rows, True)
        with self.assertRaisesRegex(RuntimeError, "teardown"):
            validate_events(rows[:1], True)

    def test_unavailable_samples_cannot_be_zero_filled(self):
        sample = dict(progress=None, status="sample_unavailable", memory=None, cpu_percent=None)
        validate_observations([sample])
        with self.assertRaisesRegex(RuntimeError, "fabricated"):
            validate_observations([dict(sample, cpu_percent=0)])

    def test_worker_activity_is_not_an_owned_progress_domain(self):
        gui = dict(id="gui", active=True)
        worker = dict(id="worker", active=False)
        sample = dict(progress=dict(channels=[gui, worker]), status="sampled")
        validate_observations([sample])
        with self.assertRaisesRegex(RuntimeError, "background progress"):
            validate_observations([dict(sample, progress=dict(channels=[gui, dict(worker, active=True)]))])

    def test_busy_does_not_imply_gui_progress(self):
        rows = [dict(event="raster_busy", busy=True, gui_sequence=2),
                dict(event="raster_busy", busy=False, gui_sequence=2)]
        self.assertEqual(busy_progress(rows)["intervals_with_gui_progress"], 0)
        rows[-1]["gui_sequence"] = 3
        self.assertEqual(busy_progress(rows)["intervals_with_gui_progress"], 1)

    def test_partial_driver_line_is_not_a_completed_event(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "events"
            path.write_text('{"event":"ready"}\n{"event":')
            self.assertEqual(events(path), [dict(event="ready")])


if __name__ == "__main__":
    unittest.main()

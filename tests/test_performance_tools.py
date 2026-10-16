import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools/performance"))
from codec import score, perform, timing_range

spec = importlib.util.spec_from_file_location("piano_download", ROOT / "tools/piano/download.py")
piano = importlib.util.module_from_spec(spec); spec.loader.exec_module(piano)


class PerformanceTests(unittest.TestCase):
    def source(self):
        return dict(schema=1, bpm=120, bars=2, ticks_per_beat=480, sharps=-1, minor=0, gm_program=0,
                    notes=[dict(pitch=60, velocity=80, tick=0, duration=240),
                           dict(pitch=64, velocity=80, tick=480, duration=240),
                           dict(pitch=60, velocity=80, tick=960, duration=240)])

    def test_reference_token_units(self):
        notes, tokens = score(self.source())
        self.assertEqual(tokens[:8], [65, 261, 213, 511, 5261, 5261, 5261, 5261])
        self.assertEqual(tokens[9], 761)  # 500 ms at 120 BPM.
        self.assertEqual(len(notes), 3)

    def test_fixed_tempo_learned_events_preserved(self):
        source = self.source(); notes, tokens = score(source)
        tokens[10] = 133 + 45; tokens[9] += 25; tokens[17] -= 25
        tokens[4:8] = [5261 + 127] * 4
        result, report = perform(source, notes, tokens)
        self.assertEqual(result["bpm"], 120); self.assertEqual(result["sharps"], -1)
        self.assertEqual(result["notes"][1]["onset_offset_us"], 25000)
        self.assertEqual(result["notes"][1]["velocity"], 45)
        self.assertEqual(result["pedals"][0]["value"], 127)
        self.assertTrue(report["pitches_preserved"])

    def test_resolve_release_once_for_both_exports(self):
        source = self.source(); notes, tokens = score(source)
        tokens[3] = 261 + 1500
        result, report = perform(source, notes, tokens)
        self.assertEqual(report["resolved_same_pitch_releases"], 1)
        self.assertEqual(result["notes"][0]["duration"], 960)

    def test_tempo_constraint_bounds_cumulative_onset(self):
        source = self.source(); notes, _ = score(source)
        self.assertEqual(timing_range(notes, 0, source, 0), (261, 262))
        low, high = timing_range(notes, 1, source, 40)
        self.assertEqual((40 + low - 261, 40 + high - 262), (410, 590))
        notes.insert(2, dict(notes[1], pitch=67))
        low, high = timing_range(notes, 1, source, 40)
        self.assertEqual((40 + low - 261, 40 + high - 262), (455, 545))
        with self.assertRaises(ValueError): timing_range(notes, 1, source, 600)

    def test_tempo_policy_does_not_stretch_model_durations(self):
        source = self.source(); notes, tokens = score(source)
        tokens[9] += 20
        result, report = perform(source, notes, tokens, "tempo")
        self.assertEqual(report["global_time_scale"], 1)
        self.assertEqual(result["notes"][1]["onset_offset_us"], 20000)

    def test_model_cannot_change_pitch_or_emit_zero_velocity(self):
        source = self.source(); notes, tokens = score(source)
        changed = list(tokens); changed[0] += 1
        with self.assertRaises(ValueError): perform(source, notes, changed)
        changed = list(tokens); changed[2] = 133
        with self.assertRaises(ValueError): perform(source, notes, changed)

    def test_rejects_unbounded_or_ambiguous_input(self):
        for field, value in (("bpm", True), ("bars", 0), ("ticks_per_beat", 2**63), ("gm_program", 40)):
            source = self.source(); source[field] = value
            with self.assertRaises(ValueError): score(source)
        source = self.source(); source["notes"].append(dict(source["notes"][0]))
        with self.assertRaises(ValueError): score(source)
        source = self.source(); source["notes"][0]["onset_offset_us"] = 20
        with self.assertRaises(ValueError): score(source)

    def test_excessive_rubato_rejected(self):
        source = self.source(); notes, tokens = score(source)
        tokens[9] = 261; tokens[17] = 261 + 500
        # Move the middle score onset near the end so its learned onset is >500 ms early.
        source["notes"][1]["tick"] = 700
        notes, _ = score(source)
        with self.assertRaisesRegex(ValueError, "rubato"): perform(source, notes, tokens)

    def test_instrument_manifest_complete_and_unique(self):
        entries = piano.entries()
        self.assertEqual(len(entries), 643)
        self.assertEqual(sum(i["bytes"] for i in entries), 748421477)

    def test_download_failure_preserves_existing_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); (root / "Samples").mkdir()
            target = root / "Samples/C4v8.flac"; target.write_bytes(b"user data")
            item = dict(path="Samples/C4v8.flac", bytes=9, sha256="0" * 64)
            with self.assertRaises(ValueError): piano.fetch(root, item, None)
            self.assertEqual(target.read_bytes(), b"user data")
            target.unlink(); partial = target.with_name(target.name + ".part"); partial.write_bytes(b"partial")
            with self.assertRaises(ValueError): piano.fetch(root, item, None)
            self.assertEqual(partial.read_bytes(), b"partial")

    def test_download_truncation_never_publishes_asset(self):
        import io
        class Response(io.BytesIO):
            status = 200
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            item = dict(path="Samples/C4v8.flac", bytes=10, sha256="0" * 64)
            with patch.object(piano.urllib.request, "urlopen", return_value=Response(b"short")), patch.object(piano.shutil, "disk_usage", return_value=type("Disk", (), {"free": 10 * 1024**3})()):
                with self.assertRaises(ValueError): piano.fetch(root, item, None)
            self.assertFalse((root / item["path"]).exists())
            self.assertFalse((root / "Samples/C4v8.flac.part").exists())


if __name__ == "__main__":
    unittest.main()

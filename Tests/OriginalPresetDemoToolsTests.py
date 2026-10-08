#!/usr/bin/env python3
"""Fast checks for the portable original-preset demo package (no audio render)."""

import copy
import hashlib
import importlib.util
import json
import math
import struct
import sys
import tempfile
import unittest
import wave
import zipfile
from pathlib import Path
from unittest import mock


TOOL_PATH = Path(__file__).resolve().parents[1] / "Tools" / "MakeOriginalPresetDemos.py"
SPEC = importlib.util.spec_from_file_location("original_preset_demo_tools", TOOL_PATH)
TOOLS = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = TOOLS
SPEC.loader.exec_module(TOOLS)


def product_bank():
    """A bank fixture independent of the tool's composition table."""
    bass_names = ("Round Sub", "Pulse Pluck", "Solid Saw", "Rubber Bass",
                  "Octave Weight", "Short PWM", "Glide Mono", "Hollow Square")
    pad_names = ("Warm Ensemble", "Slow Horizon", "Glass Halo", "Velvet PWM",
                 "Hollow Choir", "Brass Cloud", "Resonant Mist", "Dark Motion")
    bank = []
    for prefix, names in (("YB", bass_names), ("YP", pad_names)):
        for number, name in enumerate(names, 1):
            slot = f"{prefix}{number}"
            bank.append({"slot": slot, "name": name,
                         "category": "Bass" if prefix == "YB" else "Pad",
                         "offset": -12 if slot in {"YB1", "YB3", "YB6", "YB8"}
                         else 12 if slot == "YP3" else 0,
                         "release_seconds": 0.5 if prefix == "YB" else 3.0,
                         "attack_seconds": 0.0 if prefix == "YB" else 1.0,
                         "polyphony": (2 if slot == "YB5" else 1)
                         if prefix == "YB" else 6,
                         "key_mode": "Unison" if prefix == "YB" else "Poly1"})
    return bank


def read_midi(path):
    """Decode Standard MIDI File framing, VLQs and note events independently."""
    data = path.read_bytes()
    if data[:4] != b"MThd" or len(data) < 14:
        raise AssertionError("Missing MIDI header")
    header_length = int.from_bytes(data[4:8], "big")
    file_format, track_count, division = struct.unpack(">HHH", data[8:14])
    if file_format not in (0, 1) or not division or division & 0x8000:
        raise AssertionError("Expected beat-based MIDI type 0 or 1")
    cursor = 8 + header_length
    events, tempos, track_ends = [], [], []

    for track_number in range(track_count):
        if data[cursor:cursor + 4] != b"MTrk":
            raise AssertionError("Missing MIDI track")
        length = int.from_bytes(data[cursor + 4:cursor + 8], "big")
        cursor += 8
        track_end = cursor + length
        if track_end > len(data):
            raise AssertionError("Truncated MIDI track")
        tick, running_status, saw_end = 0, None, False

        def variable_length():
            nonlocal cursor
            value = 0
            for _ in range(4):
                if cursor >= track_end:
                    raise AssertionError("Truncated variable-length quantity")
                byte = data[cursor]
                cursor += 1
                value = (value << 7) | (byte & 0x7f)
                if not byte & 0x80:
                    return value
            raise AssertionError("Invalid MIDI variable-length quantity")

        while cursor < track_end:
            tick += variable_length()
            status = data[cursor]
            if status & 0x80:
                cursor += 1
                running_status = status if status < 0xf0 else None
            elif running_status is not None:
                status = running_status
            else:
                raise AssertionError("Data byte without MIDI running status")
            if status == 0xff:
                kind = data[cursor]
                cursor += 1
                size = variable_length()
                payload = data[cursor:cursor + size]
                cursor += size
                if kind == 0x51:
                    if size != 3:
                        raise AssertionError("Invalid MIDI tempo event")
                    tempos.append((tick, int.from_bytes(payload, "big")))
                elif kind == 0x2f:
                    if size or cursor != track_end:
                        raise AssertionError("Invalid MIDI end-of-track placement")
                    saw_end = True
            elif status in (0xf0, 0xf7):
                size = variable_length()
                cursor += size
            elif 0x80 <= status <= 0xef:
                size = 1 if status & 0xf0 in (0xc0, 0xd0) else 2
                payload = data[cursor:cursor + size]
                cursor += size
                if len(payload) != size or any(byte & 0x80 for byte in payload):
                    raise AssertionError("Invalid MIDI channel data")
                if status & 0xf0 in (0x80, 0x90):
                    key, velocity = payload
                    on = status & 0xf0 == 0x90 and velocity != 0
                    events.append((tick, track_number, status & 0x0f, key, on, velocity))
            else:
                raise AssertionError("Unsupported MIDI system event")
            if cursor > track_end:
                raise AssertionError("MIDI event crosses track boundary")
        if not saw_end:
            raise AssertionError("MIDI track has no end-of-track event")
        track_ends.append(tick)
    if cursor != len(data):
        raise AssertionError("Bytes after final MIDI track")
    return division, events, tempos, track_ends


class OriginalPresetDemoToolsTests(unittest.TestCase):
    def setUp(self):
        self.bank = product_bank()
        self.performances = TOOLS.make_performances(self.bank)

    def test_bank_has_complete_distinct_playable_performances(self):
        self.assertEqual([p["slot"] for p in self.performances],
                         [p["slot"] for p in self.bank])
        fingerprints = set()
        for performance, preset in zip(self.performances, self.bank):
            with self.subTest(slot=preset["slot"]):
                self.assertEqual(performance["name"], preset["name"])
                self.assertEqual(performance["category"], preset["category"])
                self.assertTrue(performance["title"].strip())
                self.assertTrue(math.isfinite(performance["bpm"]))
                self.assertGreater(performance["bpm"], 0)
                self.assertTrue(math.isfinite(performance["beats"]))
                self.assertGreater(performance["beats"], 0)
                self.assertTrue(performance["notes"])
                edges = []
                fingerprint = []
                for note in performance["notes"]:
                    self.assertIsInstance(note["key"], int)
                    self.assertIsInstance(note["velocity"], int)
                    self.assertGreaterEqual(note["key"], 0)
                    self.assertLessEqual(note["key"], 127)
                    self.assertGreaterEqual(note["velocity"], 1)
                    self.assertLessEqual(note["velocity"], 127)
                    self.assertTrue(math.isfinite(note["beat"]))
                    self.assertTrue(math.isfinite(note["duration"]))
                    self.assertGreaterEqual(note["beat"], 0)
                    self.assertGreater(note["duration"], 0)
                    end = note["beat"] + note["duration"]
                    self.assertLessEqual(end, performance["beats"] + 1e-9)
                    edges.extend([(note["beat"], 1, note["key"]),
                                  (end, 0, note["key"])])
                    fingerprint.append((note["beat"], note["duration"],
                                        note["key"], note["velocity"]))
                held = set()
                for _, on, key in sorted(edges):
                    if on:
                        self.assertNotIn(key, held, "Repeated key has an overlapping gate")
                        held.add(key)
                        # Unison may retain several different held keys for
                        # last-note priority and legato on a single voice group.
                        if preset["key_mode"] != "Unison":
                            self.assertLessEqual(len(held), preset["polyphony"])
                    else:
                        self.assertIn(key, held, "Note-off without a matching note-on")
                        held.remove(key)
                self.assertFalse(held)
                fingerprints.add(tuple(sorted(fingerprint)))
                TOOLS.validate_performance(performance, preset)
        self.assertEqual(len(fingerprints), 16, "Each preset needs its own performance")

    def test_dco_range_compensation_preserves_sounding_pitch(self):
        shifted_bank = copy.deepcopy(self.bank)
        for preset in shifted_bank:
            preset["offset"] += 12
        shifted = TOOLS.make_performances(shifted_bank)
        for before, after in zip(self.performances, shifted):
            with self.subTest(slot=before["slot"]):
                self.assertEqual(len(before["notes"]), len(after["notes"]))
                for old, new in zip(before["notes"], after["notes"]):
                    self.assertEqual(old["key"] - 12, new["key"])
                    for field in ("beat", "duration", "velocity"):
                        self.assertEqual(old[field], new[field])

    def test_slow_pad_voices_have_time_to_reach_sustain(self):
        slow_bank = copy.deepcopy(self.bank)
        for preset in slow_bank:
            if preset["category"] == "Pad":
                preset["attack_seconds"] = 3.0
        slow_scores = TOOLS.make_performances(slow_bank)
        for fast, slow, preset in zip(self.performances, slow_scores, slow_bank):
            with self.subTest(slot=preset["slot"]):
                if preset["category"] != "Pad":
                    self.assertEqual(fast, slow)
                    continue
                self.assertGreater(slow["beats"], fast["beats"])
                for note in slow["notes"]:
                    self.assertGreaterEqual(note["duration"] * 60 / slow["bpm"],
                                            preset["attack_seconds"] + 0.8)

    def assert_midi_matches(self, directory, performance):
        path = directory / (performance["slot"] + ".mid")
        TOOLS.write_midi(path, performance)
        division, events, tempos, track_ends = read_midi(path)
        self.assertEqual(len(tempos), 1)
        self.assertEqual(tempos[0][0], 0)
        self.assertAlmostEqual(tempos[0][1], 60_000_000 / performance["bpm"], delta=1)
        held, completed = {}, []
        for tick, _, channel, key, on, velocity in sorted(events, key=lambda event: event[0]):
            identity = (channel, key)
            if on:
                self.assertNotIn(identity, held, "MIDI reordered same-key off/on boundary")
                held[identity] = (tick, velocity)
            else:
                self.assertIn(identity, held, "MIDI note-off without note-on")
                start, attack_velocity = held.pop(identity)
                self.assertGreater(tick, start)
                completed.append((key, start / division, tick / division, attack_velocity))
        self.assertFalse(held, "MIDI leaves notes held")
        expected = sorted((n["key"], n["beat"], n["beat"] + n["duration"], n["velocity"])
                          for n in performance["notes"])
        self.assertEqual(len(completed), len(expected))
        for actual, note in zip(sorted(completed), expected):
            self.assertEqual(actual[0], note[0])
            self.assertEqual(actual[3], note[3])
            self.assertAlmostEqual(actual[1], note[1], delta=1 / division)
            self.assertAlmostEqual(actual[2], note[2], delta=1 / division)
        self.assertGreaterEqual(max(track_ends), max(e[0] for e in events))

    def test_all_exported_midi_preserves_composition_and_releases(self):
        with tempfile.TemporaryDirectory() as temporary:
            for performance in self.performances:
                with self.subTest(slot=performance["slot"]):
                    self.assert_midi_matches(Path(temporary), performance)

    def test_midi_handles_shuffled_events_boundary_and_long_rest(self):
        performance = {"slot": "TEST", "name": "Boundary", "title": "MIDI timing",
                       "bpm": 113, "beats": 4098, "notes": [
                           {"beat": 4096.125, "duration": 0.375, "key": 127, "velocity": 127},
                           {"beat": 0.5, "duration": 0.5, "key": 0, "velocity": 1},
                           {"beat": 0, "duration": 0.5, "key": 0, "velocity": 91},
                           {"beat": 1 / 3, "duration": 2 / 3, "key": 60, "velocity": 64}]}
        with tempfile.TemporaryDirectory() as temporary:
            self.assert_midi_matches(Path(temporary), performance)

    def test_validator_rejects_unplayable_or_invalid_scores(self):
        preset = self.bank[0]
        valid = {"slot": preset["slot"], "bpm": 120, "beats": 4, "notes": [
            {"beat": 0, "duration": 1, "key": 60, "velocity": 100}]}
        TOOLS.validate_performance(valid, preset)
        invalid = []
        for field, value in (("key", -1), ("key", 128), ("key", 60.5),
                             ("velocity", 0), ("velocity", 128), ("velocity", 90.5),
                             ("beat", -1), ("beat", float("nan")),
                             ("duration", 0), ("duration", float("inf"))):
            score = copy.deepcopy(valid)
            score["notes"][0][field] = value
            invalid.append((f"{field}={value}", score))
        score = copy.deepcopy(valid)
        score["notes"].append({"beat": 0.5, "duration": 1,
                               "key": 60, "velocity": 100})
        invalid.append(("same-key overlap", score))
        for field, value in (("bpm", 0), ("bpm", float("nan")), ("beats", 0.5)):
            score = copy.deepcopy(valid)
            score[field] = value
            invalid.append((f"{field}={value}", score))
        for label, score in invalid:
            with self.subTest(invalid=label), self.assertRaises(ValueError):
                TOOLS.validate_performance(score, preset)
        legato = copy.deepcopy(valid)
        legato["notes"].append({"beat": 0.5, "duration": 1,
                                "key": 61, "velocity": 100})
        TOOLS.validate_performance(legato, preset)
        chord = copy.deepcopy(valid)
        chord["slot"] = self.bank[8]["slot"]
        chord["notes"] = [{"beat": 0, "duration": 1, "key": key, "velocity": 100}
                          for key in range(60, 67)]
        with self.subTest(invalid="over polyphony"), self.assertRaises(ValueError):
            TOOLS.validate_performance(chord, self.bank[8])

    def test_player_and_manifest_are_utf8_under_cp1252_default(self):
        unicode_name = "Měsíční → pad 🎹"
        manifest = {"performances": [{
            "slot": "YP1", "name": unicode_name, "title": "D♭ & <quiet>",
            "category": "Pad", "description": "Soft → full; café",
            "bpm": 80, "seconds": 12.5, "wav": "YP1.wav", "midi": "YP1.mid"}]}
        original_open = Path.open

        def cp1252_open(path, mode="r", buffering=-1, encoding=None,
                        errors=None, newline=None):
            if "b" not in mode and encoding in (None, "locale"):
                encoding = "cp1252"
            return original_open(path, mode=mode, buffering=buffering,
                                 encoding=encoding, errors=errors, newline=newline)

        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            with mock.patch.object(Path, "open", cp1252_open):
                TOOLS.write_player(directory, manifest)
                TOOLS.write_json(directory / "manifest.json", manifest)
            page = (directory / "index.html").read_bytes().decode("utf-8")
            saved_json = (directory / "manifest.json").read_bytes().decode("utf-8")
            self.assertIn(unicode_name, page)
            self.assertIn("Soft → full; café", page)
            self.assertIn(unicode_name, saved_json)
            self.assertEqual(json.loads(saved_json), manifest)
            self.assertIn("YP1.wav", page)
            self.assertIn("YP1.mid", page)
            self.assertIn("utf-8", page.lower())
            self.assertNotIn("<quiet>", page, "Preset metadata must be HTML escaped")

    def test_package_accepts_commit_change_but_rejects_changed_inputs(self):
        source_digest = "1" * 64
        identity = {"sha256": source_digest, "git_commit": "new-commit",
                    "tracked_sources_modified": False}
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "collection"
            scratch = Path(temporary) / "receipts"
            (output / "MIDI").mkdir(parents=True)
            scratch.mkdir()
            receipts = []
            for number, performance in enumerate(self.performances, 1):
                stem = f'{number:02d}-{performance["slot"]}'
                wav, midi = output / f"{stem}.wav", output / "MIDI" / f"{stem}.mid"
                with wave.open(str(wav), "wb") as audio:
                    audio.setparams((2, 3, 96000, 0, "NONE", "not compressed"))
                    audio.writeframes(b"\0" * 6)
                TOOLS.write_midi(midi, performance)
                receipt = {key: value for key, value in performance.items() if key != "notes"}
                receipt.update(seconds=1 / 96000, wav=wav.name, midi=f"MIDI/{midi.name}",
                               provenance={"source_sha256": source_digest},
                               wav_sha256=hashlib.sha256(wav.read_bytes()).hexdigest(),
                               midi_sha256=hashlib.sha256(midi.read_bytes()).hexdigest())
                TOOLS.write_json(scratch / f"{stem}.finished.json", receipt)
                receipts.append(receipt)
            previous_identity = dict(identity, git_commit="previous-commit")
            with mock.patch.object(TOOLS, "source_identity", return_value=previous_identity), \
                    mock.patch("builtins.print"):
                TOOLS.package(output, scratch)
            self.assertEqual(json.loads((output / "manifest.json").read_text(encoding="utf-8"))
                             ["source"]["git_commit"], "previous-commit")
            with mock.patch.object(TOOLS, "source_identity", return_value=identity), \
                    mock.patch("builtins.print"):
                TOOLS.package(output, scratch)
            manifest_path = output / "manifest.json"
            saved_manifest = manifest_path.read_bytes()
            manifest = json.loads(saved_manifest.decode("utf-8"))
            self.assertEqual(manifest["source"], identity)
            self.assertEqual(manifest["performances"], receipts)
            files = ["index.html", "manifest.json", "YouKnow-Original-Presets.m3u"]
            files += [receipt[kind] for receipt in receipts for kind in ("wav", "midi")]
            archive_path = output.parent / "YouKnow-Original-Sessions.zip"
            saved_archive = archive_path.read_bytes()
            with zipfile.ZipFile(archive_path) as archive:
                self.assertEqual(set(archive.namelist()),
                                 {f"YouKnow-Original-Sessions/{name}" for name in files})
                for name in files:
                    self.assertEqual(archive.read(f"YouKnow-Original-Sessions/{name}"),
                                     (output / name).read_bytes())
            changed_source = dict(identity, sha256="2" * 64)
            with mock.patch.object(TOOLS, "source_identity", return_value=changed_source), \
                    self.assertRaises(ValueError):
                TOOLS.package(output, scratch)
            for kind in ("wav", "midi"):
                artifact = output / receipts[0][kind]
                original = artifact.read_bytes()
                artifact.write_bytes(original + b"changed")
                try:
                    with self.subTest(changed=kind), \
                            mock.patch.object(TOOLS, "source_identity", return_value=identity), \
                            self.assertRaises(ValueError):
                        TOOLS.package(output, scratch)
                finally:
                    artifact.write_bytes(original)
            self.assertEqual(manifest_path.read_bytes(), saved_manifest)
            self.assertEqual(archive_path.read_bytes(), saved_archive)


if __name__ == "__main__":
    unittest.main()

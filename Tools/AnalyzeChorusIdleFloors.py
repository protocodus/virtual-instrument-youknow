#!/usr/bin/env python3
"""Chorus hiss against notes in the April 2026 factory-bank captures of #439522 (OQ-03).

Protocol. Lewis Francis's Juno-106 #439522 (original DCO chips and chorus
board, Borish replacement VCF/VCA cards) played each 106_calibration.zip bank
sequence at one recording gain: per patch a chord, five one-second notes C1-C5
one second apart, then two seconds before the next patch. For every patch this
measures its idle floor from 1.2 s after its last note-off to 0.1 s before the
next patch, and its C4 note from 0.2 to 0.9 s after onset, as 20 Hz-20 kHz
Hann-periodogram band RMS per channel, unweighted and A-weighted. Idle minus
note cancels the recording gain; a model render of the same MIDI gives the same
difference, and model minus hardware is the hiss error. A-weighting is the
hiss level, as in Panasonic's MN3009 noise row and the engine's HISS-100
normalization. The unweighted band also counts the hardware's chorus-on
energy below 200 Hz, which is not hiss. The window must taper: the hardware's
idle floors carry a large sub-20 Hz drift while the chorus runs, and a boxcar
leaks it into the band (+1.1 dB there, 0.08 dB on the model). Patches with
RELEASE above 40 or any NOISE are reported but excluded from the summary: their
windows hold release tails or the main noise source rather than the idle
floor. Chorus-off patches give the dry floor the chorus lifts.

Limits. One serviced unit, not an original-unit population. The hiss level
follows the clock sweep -- about 8 dB inside one 0.7 s window -- so single
windows scatter by a few dB and only the mean over patches is reported as a
level. The recording's dry floor includes hum and interface noise, so the
chorus-on/off lift is a lower bound on the line's own contribution below 2 kHz.

Sources: https://www.lewisfrancis.com/nwio/Juno-10-test-audio-96K.zip
(892,109,573 bytes; each member below can be fetched alone by HTTP range from
its local-header offset and inflated) and https://kayrock.org/kr106/106_calibration.zip.
The recordings' identity is checked by hash; unknown audio is rejected.

  python3 Tools/AnalyzeChorusIdleFloors.py --midi-zip 106_calibration.zip --export-events out/idle
  ./build/YouKnowRenderCalibrationEvents out/idle/events-A2x.txt out/idle/model-A2x.wav \\
      1 product 1 owner-blend fixed-service serviced439522 96000 2
  python3 Tools/AnalyzeChorusIdleFloors.py --midi-zip 106_calibration.zip \\
      --captures CAPTURE_DIR --models out/idle --output report.json
  python3 Tools/AnalyzeChorusIdleFloors.py --self-test

CAPTURE_DIR holds bank_<name>_bip.aif as extracted; model renders are
model-<name>.wav. The exported events restate each bank's 0x31 dumps as 0x30
program dumps: the sequences carry the program number where the manual message
documents a zero marker, which the engine's codec rejects, and the tone bytes
are identical either way.
"""
import argparse
import hashlib
import json
import struct
import zipfile
from pathlib import Path

import numpy as np
from scipy.io import wavfile
from scipy.signal import periodogram

BAND_HZ = (20.0, 20000.0)
MAX_RELEASE = 40
# offset/compressed: the member's local header and deflated size in the archive.
BANKS = {
    "A1x": {"offset": 109350436, "compressed": 49311337,
            "aiff": "b235ba2236c1a509627ce1e84fa35004b0d7c3e99eb36899c4c5de63cc668662",
            "pcm": "1bd816ca226c1a4cb8b1229fc790c5a091c6c2d630804fb4c62c9bdae360d49b",
            "midi": "ef7ff68b6059a913a30a70acbea54d1ef982884ed229f25d8684642295a5e511"},
    "A2x": {"offset": 466267030, "compressed": 43423018,
            "aiff": "1e97d34ed535fc6de4c678adad05634b89ac758a3badb0671c0c7bf057f3f0e3",
            "pcm": "7570472109215fbe634fa295cd8f3d5a6792f5c73b8b02e96df24c8696ad04e0",
            "midi": "c327d966858736269eaaac5786e2c83172ade45050933abdaa722a6905d67b24"},
    "B3x": {"offset": 265511160, "compressed": 48862705,
            "aiff": "cfc6e32244b78ac409787e934501e3ff398bfe9793f8d85202763c79388fe621",
            "pcm": "ff21f00a5f15639023a059ac6e4a0eeac2a969b285913bab172dfac2e9d3e6e6",
            "midi": "7a821f38a08316c2d4c111a45659b407a69b054e3c1002e922d91fbd30213775"},
    "B4x": {"offset": 57708693, "compressed": 51641362,
            "aiff": "d0f7953ea437eec31c5151e4090842f9010282f1b91c07f12c86601a07a10bde",
            "pcm": "78f4e23676118dae3df5e2a46b01e5768804dc9791298fd3cfb7bb0c575ccf5a",
            "midi": "f30671297e229885176faf2d7e43136cdd8d982a865292160cbf7dae6356ccaf"},
}


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def varlen(data, i):
    value = 0
    while True:
        byte = data[i]
        i += 1
        value = (value << 7) | (byte & 0x7F)
        if not byte & 0x80:
            return value, i


def timeline(data):
    """(seconds, status or 'sysex', payload) for every channel and SysEx event."""
    ppq = struct.unpack(">H", data[12:14])[0]
    i, tempo, events = 14, 500000, []
    while i < len(data):
        length = struct.unpack(">I", data[i + 4:i + 8])[0]
        j, end, ticks, running = i + 8, i + 8 + length, 0, None
        while j < end:
            delta, j = varlen(data, j)
            ticks += delta
            status = data[j]
            if status == 0xFF:
                size, k = varlen(data, j + 2)
                if data[j + 1] == 0x51:
                    tempo = int.from_bytes(data[k:k + size], "big")
                j = k + size
            elif status in (0xF0, 0xF7):
                size, k = varlen(data, j + 1)
                events.append((ticks * tempo / ppq / 1e6, "sysex", data[k:k + size]))
                j = k + size
            else:
                if status & 0x80:
                    running = status
                    j += 1
                count = 1 if running & 0xF0 in (0xC0, 0xD0) else 2
                events.append((ticks * tempo / ppq / 1e6, running, data[j:j + count]))
                j += count
        i = end
    return events


def patches(events, end_seconds):
    """Per patch: chorus state, RELEASE, NOISE, idle window and C4 note onset."""
    dumps = [(t, v) for t, k, v in events if k == "sysex"]
    rows = []
    for index, (start, dump) in enumerate(dumps):
        tone, switches = dump[4:20], dump[20]
        stop = dumps[index + 1][0] if index + 1 < len(dumps) else end_seconds
        inside = [(t, k, v) for t, k, v in events if k != "sysex" and start < t < stop]
        offs = [t for t, k, v in inside if k & 0xF0 == 0x80 or (k & 0xF0 == 0x90 and v[1] == 0)]
        c4 = [t for t, k, v in inside if k & 0xF0 == 0x90 and v[0] == 60 and v[1] and t > start + 1.0]
        chorus = "off" if switches >> 5 & 1 else ("I" if switches >> 6 & 1 else "II")
        # The last patch can run into the end of the capture.
        idle = [max(offs) + 1.2, stop - 0.1] if offs else None
        rows.append({"patch": dump[3], "chorus": chorus,
                     "release": tone[14], "noise": tone[4],
                     "idle_seconds": idle if idle and idle[1] - idle[0] >= 0.3 else None,
                     "note_seconds": [c4[0] + 0.2, c4[0] + 0.9] if c4 else None})
    return rows


def read_aiff(path):
    data = Path(path).read_bytes()
    position, rate, channels, frames = 12, None, None, None
    while position < len(data):
        chunk, size = data[position:position + 4], struct.unpack(">I", data[position + 4:position + 8])[0]
        body = position + 8
        if chunk == b"COMM":
            channels, frames, bits = struct.unpack(">hIh", data[body:body + 8])
            if bits != 24:
                raise ValueError("expected 24-bit AIFF")
            exponent = struct.unpack(">H", data[body + 8:body + 10])[0] & 0x7FFF
            mantissa = struct.unpack(">Q", data[body + 10:body + 18])[0]
            rate = int(round(mantissa * 2.0 ** (exponent - 16383 - 63)))
        elif chunk == b"SSND":
            offset = struct.unpack(">I", data[body:body + 4])[0]
            raw = np.frombuffer(data[body + 8 + offset:body + 8 + offset + frames * channels * 3], np.uint8)
            triplets = raw.reshape(-1, 3).astype(np.int32)
            value = triplets[:, 0] << 16 | triplets[:, 1] << 8 | triplets[:, 2]
            value = np.where(value & 0x800000, value - (1 << 24), value)
            return rate, (value / float(1 << 23)).reshape(-1, channels)
        position = body + size + (size & 1)
    raise ValueError("AIFF has no sound data")


def read_wav(path):
    rate, samples = wavfile.read(path)
    if samples.dtype.kind == "i":
        samples = samples / float(1 << (8 * samples.dtype.itemsize - 1))
    return rate, np.asarray(samples, np.float64).reshape(len(samples), -1)


def checked_capture(path, bank):
    rate, audio = read_aiff(path) if path.suffix.lower() in (".aif", ".aiff") else read_wav(path)
    if sha256(np.ascontiguousarray(audio, "<f8").tobytes()) != BANKS[bank]["pcm"]:
        raise ValueError(f"{path}: unknown PCM; cannot attribute it to #439522 bank {bank}")
    return rate, audio


def a_weighting(frequencies):
    """IEC 61672 A-weighting as a power gain, 0 dB at 1 kHz."""
    f2 = np.asarray(frequencies, dtype=float) ** 2
    ra = 12194.0 ** 2 * f2 ** 2 / ((f2 + 20.6 ** 2) * np.sqrt((f2 + 107.7 ** 2) * (f2 + 737.9 ** 2))
                                  * (f2 + 12194.0 ** 2))
    return (ra / 0.79434639) ** 2


def band_db(samples, rate, weighted=False):
    frequencies, power = periodogram(samples, rate, window="hann", detrend="constant")
    band = (frequencies >= BAND_HZ[0]) & (frequencies <= BAND_HZ[1])
    gain = a_weighting(frequencies[band]) if weighted else 1.0
    return float(10 * np.log10(np.sum(power[band] * gain) * (frequencies[1] - frequencies[0]) + 1e-30))


def levels(recording, window, weighted=False):
    rate, audio = recording
    begin, end = (round(t * rate) for t in window)
    if begin < 0 or end > len(audio) or end - begin < round(0.3 * rate):
        raise ValueError("measurement window outside the recording")
    return [band_db(audio[begin:end, c], rate, weighted) for c in range(audio.shape[1])]


def measure(rows, capture, model):
    result = []
    for row in rows:
        if not row["idle_seconds"] or not row["note_seconds"]:
            continue
        entry = dict(row, used=row["release"] <= MAX_RELEASE and row["noise"] == 0)
        for name, recording in (("hardware", capture), ("model", model)):
            if recording is None:
                continue
            idle, note = levels(recording, row["idle_seconds"]), levels(recording, row["note_seconds"])
            idle_a = levels(recording, row["idle_seconds"], weighted=True)
            entry[name] = {"idle_dbfs": idle, "idle_a_weighted_dbfs": idle_a, "note_dbfs": note,
                           "idle_minus_note_db": [i - n for i, n in zip(idle, note)],
                           "idle_a_weighted_minus_note_db": [i - n for i, n in zip(idle_a, note)]}
        if "hardware" in entry and "model" in entry:
            for key, source in (("model_minus_hardware_db", "idle_minus_note_db"),
                                ("model_minus_hardware_a_weighted_db", "idle_a_weighted_minus_note_db")):
                entry[key] = [m - h for m, h in zip(entry["model"][source], entry["hardware"][source])]
        result.append(entry)
    return result


def summarize(entries):
    summary = {}
    for chorus in ("I", "II"):
        used = [e for e in entries
                if e["used"] and e["chorus"] == chorus and "model_minus_hardware_db" in e]
        if used:
            errors = np.asarray([e["model_minus_hardware_db"] for e in used])
            weighted = np.asarray([e["model_minus_hardware_a_weighted_db"] for e in used])
            summary[f"chorus_{chorus}"] = {"patches": len(errors),
                                           "mean_model_minus_hardware_db": errors.mean(0).tolist(),
                                           "range_db": [errors.min(0).tolist(), errors.max(0).tolist()],
                                           "mean_model_minus_hardware_a_weighted_db":
                                               weighted.mean(0).tolist()}
    for name in ("hardware", "model"):
        floors = {c: [e[name]["idle_dbfs"] for e in entries if e["used"] and e["chorus"] == c and name in e]
                  for c in ("off", "I")}
        if floors["off"] and floors["I"]:
            summary[f"{name}_mode_I_lift_over_off_db"] = (
                np.mean(floors["I"], 0) - np.mean(floors["off"], 0)).tolist()
    return summary


def export_events(midi_zip, directory):
    directory.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(midi_zip) as archive:
        for bank, pins in BANKS.items():
            data = archive.read(f"106_calibration/bank_midi_106/bank_{bank}.mid")
            if sha256(data) != pins["midi"]:
                raise ValueError(f"bank {bank}: unexpected MIDI sequence")
            lines = []
            for seconds, kind, payload in timeline(data):
                if kind == "sysex":
                    dump = bytearray(b"\xf0" + payload)
                    if dump[2] == 0x31:
                        dump[2] = 0x30
                    text = dump.hex()
                else:
                    text = f"{kind:02x}{payload.hex()}"
                lines.append(f"{seconds:.9f}\t{text}\n")
            (directory / f"events-{bank}.txt").write_text("".join(lines))


def run(args):
    report = {"analyzer_sha256": sha256(Path(__file__).read_bytes()), "band_hz": BAND_HZ,
              "max_release": MAX_RELEASE, "banks": {}}
    all_entries = []
    with zipfile.ZipFile(args.midi_zip) as archive:
        for bank, pins in BANKS.items():
            data = archive.read(f"106_calibration/bank_midi_106/bank_{bank}.mid")
            if sha256(data) != pins["midi"]:
                raise ValueError(f"bank {bank}: unexpected MIDI sequence")
            capture = checked_capture(args.captures / f"bank_{bank}_bip.aif", bank)
            model_path = args.models / f"model-{bank}.wav" if args.models else None
            model = read_wav(model_path) if model_path else None
            end = len(capture[1]) / capture[0]
            if model is not None:
                end = min(end, len(model[1]) / model[0])
            entries = measure(patches(timeline(data), end), capture, model)
            report["banks"][bank] = entries
            all_entries += entries
    report["summary"] = summarize(all_entries)
    args.output.write_text(json.dumps(report, indent=1, allow_nan=False) + "\n")
    print(json.dumps(report["summary"], indent=1))


def self_test():
    rate = 48000
    rng = np.random.default_rng(1)
    time = np.arange(8 * rate) / rate
    audio = np.zeros((len(time), 2))
    note = (time >= 1.0) & (time < 2.0)
    audio[note] = 0.1 * np.sin(2 * np.pi * 261.6 * time[note])[:, None]
    idle = (time >= 3.0) & (time < 6.0)
    audio[idle] = 1e-3 * rng.standard_normal((idle.sum(), 2))
    rows = [{"chorus": "I", "release": 0, "noise": 0, "idle_seconds": [3.2, 5.8], "note_seconds": [1.2, 1.9]}]
    measured = measure(rows, (rate, audio), (rate, audio * 3.0))[0]
    # White noise keeps (20000 - 20) / 24000 of its power inside the band.
    expected = 10 * np.log10(1e-6 * (BAND_HZ[1] - BAND_HZ[0]) / (rate / 2) / (0.1 ** 2 / 2))
    assert abs(measured["hardware"]["idle_minus_note_db"][0] - expected) < 0.2
    assert max(abs(e) for e in measured["model_minus_hardware_db"]) < 1e-9
    # A 1 kHz tone is unchanged by A-weighting.
    tone = 1e-3 * np.sin(2 * np.pi * 1000.0 * time[:rate])
    assert abs(band_db(tone, rate, weighted=True) - band_db(tone, rate)) < 0.05
    # A large sub-audio drift under the idle floor must not leak into the band.
    drifting = audio.copy()
    drifting[idle] += 0.02 * (time[idle] - 3.0)[:, None]
    leaked = measure(rows, (rate, drifting), None)[0]["hardware"]["idle_minus_note_db"][0]
    assert abs(leaked - measured["hardware"]["idle_minus_note_db"][0]) < 0.1
    tail = measure([dict(rows[0], release=90)], (rate, audio), None)[0]
    assert not tail["used"]
    events = [(0.0, "sysex", bytes([0x41, 0x31, 0, 3] + [0] * 14 + [5, 0] + [0x51, 0x11, 0xF7])),
              (0.25, 0x90, bytes([60, 100])), (1.5, 0x90, bytes([60, 100])), (2.5, 0x80, bytes([60, 0]))]
    row = patches(events, 6.0)[0]
    assert row["chorus"] == "I" and row["release"] == 5 and row["patch"] == 3
    assert row["idle_seconds"] == [3.7, 5.9] and row["note_seconds"] == [1.7, 2.4]
    print("chorus idle-floor analyzer self-check passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--midi-zip", type=Path)
    parser.add_argument("--export-events", type=Path)
    parser.add_argument("--captures", type=Path)
    parser.add_argument("--models", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    if not args.midi_zip:
        parser.error("--midi-zip is required")
    if args.export_events:
        export_events(args.midi_zip, args.export_events)
    if args.captures:
        if not args.output:
            parser.error("--captures requires --output")
        if args.output.exists():
            parser.error("output exists; preserve previous evidence")
        run(args)
    elif not args.export_events:
        parser.error("supply --export-events and/or --captures")


if __name__ == "__main__":
    main()

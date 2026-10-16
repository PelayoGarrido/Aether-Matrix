#!/usr/bin/env python3
"""Verify weights, supervise the native evaluator and independently inspect MIDI.

No Python ML packages, model code execution, credentials or cloud inference.
"""
import argparse
from collections import Counter
import json
import os
from pathlib import Path
import signal
import struct
import subprocess
import sys
import time

from download_candidate import FILES, ROOT, digest, ensure_space


def inspect_midi(path):
    """Parse our SMF output independently of the C++ writer, including note pairs."""
    with path.open("rb") as stream:
        data = stream.read(1024**2 + 1)
    if len(data) < 22 or len(data) > 1024**2 or data[:4] != b"MThd":
        raise ValueError("Invalid or oversized MIDI")
    header_size, fmt, tracks, division = struct.unpack(">IHHH", data[4:14])
    if (header_size, fmt, tracks, division) != (6, 0, 1, 480) or data[14:18] != b"MTrk":
        raise ValueError("Unexpected MIDI header")
    length = struct.unpack(">I", data[18:22])[0]
    if length != len(data) - 22:
        raise ValueError("MIDI track length mismatch")
    index, tick, ended = 22, 0, False
    notes, active, metadata = [], {}, {}

    def variable():
        nonlocal index
        result = 0
        for _ in range(4):
            if index >= len(data):
                raise ValueError("Truncated MIDI delta")
            byte = data[index]
            index += 1
            result = (result << 7) | (byte & 127)
            if byte < 128:
                return result
        raise ValueError("Oversized MIDI delta")

    while index < len(data):
        tick += variable()
        if index >= len(data):
            raise ValueError("Truncated MIDI event")
        status = data[index]
        index += 1
        if status == 0xFF:
            if index >= len(data):
                raise ValueError("Truncated MIDI metadata")
            kind = data[index]
            index += 1
            size = variable()
            payload = data[index:index + size]
            index += size
            if len(payload) != size:
                raise ValueError("Truncated MIDI metadata")
            if kind == 0x51:
                if size != 3:
                    raise ValueError("Invalid tempo")
                metadata["tempo_us"] = int.from_bytes(payload, "big")
            elif kind == 0x59:
                if size != 2:
                    raise ValueError("Invalid key")
                metadata["sharps"] = struct.unpack("b", payload[:1])[0]
                metadata["minor"] = payload[1]
            elif kind == 0x2F:
                if size or index != len(data) or active:
                    raise ValueError("Invalid end-of-track or hanging notes")
                ended = True
        elif status == 0xC0:
            if index >= len(data) or data[index] > 127:
                raise ValueError("Invalid MIDI program")
            metadata["gm_program"] = data[index]
            index += 1
        elif status in (0x90, 0x80):
            if index + 2 > len(data):
                raise ValueError("Truncated MIDI note")
            pitch, velocity = data[index:index + 2]
            index += 2
            if pitch > 127 or velocity > 127:
                raise ValueError("Out-of-range MIDI note")
            if status == 0x90 and velocity:
                if pitch in active:
                    raise ValueError("Ambiguous same-pitch overlap")
                active[pitch] = (tick, velocity)
            else:
                if pitch not in active:
                    raise ValueError("Unmatched note-off")
                start, velocity = active.pop(pitch)
                if tick <= start:
                    raise ValueError("Zero-length note")
                notes.append({"tick": start, "duration": tick - start, "pitch": pitch, "velocity": velocity})
        else:
            raise ValueError("Unexpected MIDI event/channel")
    if not ended:
        raise ValueError("Missing end-of-track")
    metadata["end_tick"] = tick
    return notes, metadata


def validate(prefix):
    if prefix.with_suffix(".json").stat().st_size > 1024**2:
        raise ValueError("Performance exceeds 1 MiB")
    report = json.loads(prefix.with_suffix(".json").read_text())
    notes, metadata = inspect_midi(prefix.with_suffix(".mid"))
    sort_key = lambda note: (note["tick"], note["pitch"], note["duration"], note["velocity"])
    if sorted(notes, key=sort_key) != sorted(report["notes"], key=sort_key):
        raise ValueError("MIDI and shared performance JSON disagree")
    for name in ("gm_program", "sharps", "minor"):
        if metadata[name] != report[name]:
            raise ValueError(f"MIDI {name} mismatch")
    if metadata["tempo_us"] != 60000000 // report["bpm"] or metadata["end_tick"] != report["bars"] * 4 * 480:
        raise ValueError("MIDI timing mismatch")
    if not notes or any(n["tick"] < 0 or n["tick"] + n["duration"] > metadata["end_tick"] for n in notes):
        raise ValueError("Performance outside requested timeline")
    # Major signature tonic is 7*sf mod 12; relative minor is nine semitones above.
    tonic = (7 * report["sharps"] + (9 if report["minor"] else 0)) % 12
    intervals = (0, 2, 3, 5, 7, 8, 10) if report["minor"] else (0, 2, 4, 5, 7, 9, 11)
    scale = {(tonic + interval) % 12 for interval in intervals}
    total = sum(n["duration"] for n in notes)
    in_scale = sum(n["duration"] for n in notes if n["pitch"] % 12 in scale)
    edges = []
    for note in notes:
        edges.extend(((note["tick"], 1), (note["tick"] + note["duration"], -1)))
    current, peak = 0, 0
    for _, delta in sorted(edges):
        current += delta
        peak = max(current, peak)
    summary = {
        "schema": 1,
        "midi_matches_performance": True,
        "notes": len(notes),
        "first_note_tick": min(n["tick"] for n in notes),
        "last_note_end_tick": max(n["tick"] + n["duration"] for n in notes),
        "requested_end_tick": metadata["end_tick"],
        "duration_weighted_diatonic_fraction": in_scale / total,
        "peak_polyphony": peak,
        "velocity_values": sorted({n["velocity"] for n in notes}),
        "pitch_class_note_counts": dict(sorted(Counter(n["pitch"] % 12 for n in notes).items())),
        "load_seconds": report["load_seconds"],
        "generation_seconds": report["generation_seconds"],
        "sampled_peak_footprint_bytes": report["sampled_peak_footprint_bytes"],
        "stop_reason": report["stop_reason"],
        "quality_approved": False,
    }
    target = prefix.with_suffix(".validation.json")
    with target.open("x") as stream:
        json.dump(summary, stream, indent=2)
        stream.write("\n")
    print(json.dumps(summary, indent=2))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path, help="New prefix, with no extension")
    parser.add_argument("--events", type=int, default=64, choices=range(8, 513), metavar="8..512")
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--bpm", type=int, default=120)
    parser.add_argument("--bars", type=int, default=8)
    parser.add_argument("--program", type=int, default=0)
    parser.add_argument("--sharps", type=int, default=0)
    parser.add_argument("--minor", type=int, default=0, choices=(0, 1))
    args = parser.parse_args()
    prefix = args.output.resolve()
    if prefix.suffix:
        raise ValueError("Use an output prefix without an extension")
    prefix.parent.mkdir(parents=True, exist_ok=True)
    ensure_space(prefix.parent, 2 * 1024**2)
    for name, size, sha in FILES:
        path = ROOT / name
        if path.is_symlink() or not path.is_file() or path.stat().st_size != size or digest(path) != sha:
            raise ValueError("Candidate weights missing or changed; run download_candidate.py")
    project = Path(__file__).resolve().parents[2]
    executable = project / "build-midi" / "AetherMidiEvaluate"
    if sys.platform == "win32":
        executable = project / "build-midi" / "Release" / "AetherMidiEvaluate.exe"
    command = [str(executable), str(ROOT), str(prefix)] + [str(value) for value in
        (args.events, args.seed, args.bpm, args.bars, args.program, args.sharps, args.minor)]
    env = dict(os.environ, ORT_DISABLE_TELEMETRY="1")
    started = time.monotonic()
    peak_rss = 0
    with prefix.with_suffix(".log").open("x") as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, env=env)
        try:
            while process.poll() is None:
                if time.monotonic() - started > 150:
                    raise RuntimeError("Evaluator exceeded its 150-second process limit")
                ensure_space(prefix.parent)
                if sys.platform == "darwin":
                    usage = subprocess.run(["/bin/ps", "-o", "rss=", "-p", str(process.pid)],
                        capture_output=True, text=True, check=False, timeout=2)
                    if usage.stdout.strip():
                        peak_rss = max(peak_rss, int(usage.stdout.strip()) * 1024)
                        if peak_rss > 4 * 1024**3:
                            raise RuntimeError("Evaluator exceeded its 4 GiB resident-memory limit")
                time.sleep(0.25)
        finally:
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=3)
        if process.returncode:
            raise RuntimeError(f"Evaluator failed ({process.returncode}); inspect {prefix.name}.log")
    with prefix.with_suffix(".process.json").open("x") as stream:
        json.dump({"sampled_peak_rss_bytes": peak_rss if sys.platform == "darwin" else None,
                   "rss_sampling_interval_seconds": 0.25,
                   "process_wall_seconds": time.monotonic() - started,
                   "exit_code": process.returncode}, stream, indent=2)
        stream.write("\n")
    validate(prefix)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        sys.exit(str(error))

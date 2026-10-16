#!/usr/bin/env python3
"""Independently inspect the native renderer's PCM24 and SMF event timeline."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import wave


def small_json(path):
    with path.open("rb") as stream:
        data = stream.read(1024**2 + 1)
    if len(data) > 1024**2:
        raise ValueError("Oversized render metadata")
    return json.loads(data)


def midi_events(path):
    with path.open("rb") as stream:
        data = stream.read(1024**2 + 1)
    if len(data) > 1024**2 or data[:4] != b"MThd" or len(data) < 22:
        raise ValueError("Invalid SMF header")
    if struct.unpack(">IHHH", data[4:14]) != (6, 0, 1, 9600) or data[14:18] != b"MTrk":
        raise ValueError("Unexpected SMF format")
    if struct.unpack(">I", data[18:22])[0] != len(data)-22:
        raise ValueError("SMF length mismatch")
    position, tick, running, tempo, ended = 22, 0, None, None, False
    events, active, signatures = [], {}, {}
    def take(n):
        nonlocal position
        b = data[position:position+n]; position += n
        if len(b) != n: raise ValueError("Truncated SMF event")
        return b
    def vlq():
        n = 0
        for _ in range(4):
            b = take(1)[0]; n = (n << 7) | (b & 127)
            if b < 128: return n
        raise ValueError("Unbounded SMF delta")
    while position < len(data):
        tick += vlq()
        if position >= len(data): raise ValueError("Truncated SMF status")
        if data[position] >= 128:
            status = take(1)[0]
        elif running is not None:
            status = running
        else:
            raise ValueError("Invalid running status")
        if status == 0xFF:
            running = None; kind = take(1)[0]; payload = take(vlq())
            if kind == 0x51:
                if len(payload) != 3 or tempo is not None or tick != 0: raise ValueError("Unexpected tempo map")
                tempo = int.from_bytes(payload, "big")
            elif kind == 0x59:
                if len(payload) != 2: raise ValueError("Invalid key signature")
                signatures["key"] = (struct.unpack("b", payload[:1])[0], payload[1])
            elif kind == 0x2F:
                if payload or active or position != len(data): raise ValueError("Hanging notes or invalid track end")
                ended = True
        elif status in (0x80, 0x90, 0xB0, 0xC0):
            running = status; payload = take(1 if status == 0xC0 else 2)
            if any(v > 127 for v in payload) or tempo is None: raise ValueError("Invalid MIDI data")
            if status == 0xC0:
                if payload != b"\x00": raise ValueError("Expected piano program")
                continue
            a, b = payload
            if status == 0xB0:
                if a != 64: raise ValueError("Unexpected controller")
                events.append((1, 0, b, tick*tempo/9600))
            elif status == 0x90 and b:
                if a in active: raise ValueError("Ambiguous same-pitch voice")
                active[a] = tick; events.append((2, a, b, tick*tempo/9600))
            else:
                if a not in active or active.pop(a) >= tick: raise ValueError("Invalid note-off")
                events.append((0, a, 0, tick*tempo/9600))
        else:
            raise ValueError("Unexpected MIDI event/channel")
    if not ended or tempo is None: raise ValueError("Incomplete MIDI")
    return events, tempo, signatures


def verify(job):
    if job.is_symlink(): raise ValueError("Invalid job directory")
    report, performance = small_json(job/"render.json"), small_json(job/"performance.json")
    if report.get("completed") is not True or report.get("schema") != 1:
        raise ValueError("Unfinished render")
    for name, field in (("instrument.wav", "audio_sha256"), ("performance.mid", "midi_sha256")):
        path = job/name
        if path.is_symlink() or path.stat().st_size > (256 if name.endswith(".wav") else 1)*1024**2:
            raise ValueError("Linked or oversized render output")
        with (job/name).open("rb") as stream:
            if hashlib.file_digest(stream, "sha256").hexdigest() != report[field]: raise ValueError("Render digest mismatch")
    events, tempo, signatures = midi_events(job/"performance.mid")
    if tempo != 60000000 // performance["bpm"] or signatures.get("key") != (performance["sharps"], performance["minor"]):
        raise ValueError("Tempo/key mismatch")
    expected = report["events"]
    if not 1 <= len(performance["notes"]) <= 512 or len(performance["pedals"]) > 2048:
        raise ValueError("Unbounded performance")
    def micros(tick, offset=0):
        return math.floor(tick*60000000.0/(performance["bpm"]*performance["ticks_per_beat"])+0.5)+offset
    timeline = []
    for i, note in enumerate(performance["notes"]):
        timeline.extend(((micros(note["tick"], note["onset_offset_us"]), 2, i, 0),
                         (micros(note["tick"]+note["duration"], note["release_offset_us"]), 0, i, 0)))
    for p in performance["pedals"]: timeline.append((micros(p["tick"], p["offset_us"]), 1, 0, p["value"]))
    end = max(micros(performance["bars"]*4*performance["ticks_per_beat"]), max(e[0] for e in timeline))
    timeline.append((end, 1, 0, 0)); timeline.sort(key=lambda e: (e[0], e[1]))
    if len(timeline) != len(expected): raise ValueError("Performance/renderer event count differs")
    for native, computed in zip(expected, timeline):
        if (native["kind"], native["note"], native["value"]) != computed[1:] or abs(native["micros"]-computed[0]) > 1:
            raise ValueError("Renderer disagrees with canonical performance")
    if len(events) != len(expected): raise ValueError("Audio/MIDI event count differs")
    maximum_error = 0
    for actual, event in zip(events, expected):
        kind = event["kind"]; note = performance["notes"][event["note"]] if kind != 1 else None
        target = (kind, note["pitch"] if note else 0, note["velocity"] if kind == 2 else event["value"] if kind == 1 else 0)
        if actual[:3] != target: raise ValueError("Audio/MIDI event value/order differs")
        error = abs(actual[3] - event["micros"]); maximum_error = max(maximum_error, error)
        if error > tempo/9600 + 1 or abs(event["frame"] - event["micros"]*report["sample_rate"]/1e6) > 0.50001:
            raise ValueError("Audio/MIDI alignment exceeds quantisation bound")
    if (job/"instrument.wav").stat().st_size > 256*1024**2: raise ValueError("Oversized WAV")
    peak, square, count, clipped = 0, 0.0, 0, 0
    with wave.open(str(job/"instrument.wav"), "rb") as audio:
        if (audio.getnchannels(), audio.getsampwidth(), audio.getframerate(), audio.getnframes()) != (2, 3, int(report["sample_rate"]), report["frames"]):
            raise ValueError("PCM24 audio contract mismatch")
        while block := audio.readframes(8192):
            for i in range(0, len(block), 3):
                value = int.from_bytes(block[i:i+3], "little", signed=True)
                peak = max(peak, abs(value)); square += (value/8388608)**2; count += 1
                clipped += int(value in (-8388608, 8388607))
    if count != report["frames"]*2 or not peak or clipped: raise ValueError("Truncated, silent, empty or clipped audio")
    return dict(schema=1, hashes_match=True, sample_rate=report["sample_rate"], bits_per_sample=24,
                midi_audio_events_match=True, event_count=len(events), max_midi_time_error_us=maximum_error,
                sample_peak_dbfs=20*math.log10(peak/8388608), rms_dbfs=10*math.log10(square/count),
                clipped_samples=clipped, true_peak_measured=False, quality_approved=False)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument("job", type=Path)
    print(json.dumps(verify(parser.parse_args().job), indent=2))

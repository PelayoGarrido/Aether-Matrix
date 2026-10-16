#!/usr/bin/env python3
"""Audition a verified MIDI performance with Apple's existing system instrument.

For non-commercial candidate listening only, not a production audio renderer.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys

from evaluate import inspect_midi
from download_candidate import ensure_space


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("performance", type=Path, help="Generated performance .json")
    args = parser.parse_args()
    if sys.platform != "darwin":
        raise ValueError("The system-instrument audition is macOS-only")
    source = args.performance.resolve()
    if source.stat().st_size > 1024**2:
        raise ValueError("Performance exceeds 1 MiB")
    performance = json.loads(source.read_text())
    notes, metadata = inspect_midi(source.with_suffix(".mid"))
    key = lambda n: (n["tick"], n["pitch"], n["duration"], n["velocity"])
    if sorted(notes, key=key) != sorted(performance["notes"], key=key):
        raise ValueError("MIDI and performance do not agree")
    if metadata["gm_program"] != performance["gm_program"] or metadata["tempo_us"] != 60000000 // performance["bpm"]:
        raise ValueError("Performance instrument or tempo differs from MIDI")
    ensure_space(source.parent, 20 * 1024**2)
    header = f"{performance['bpm']} {performance['gm_program']} {metadata['end_tick']} {len(notes)}"
    lines = [header] + [f"{n['tick']} {n['duration']} {n['pitch']} {n['velocity']}" for n in notes]
    executable = Path(__file__).resolve().parents[2] / "build-midi" / "AetherMidiAudition"
    output = source.with_suffix(".wav")
    result = subprocess.run([str(executable), str(output)], input="\n".join(lines)+"\n",
        capture_output=True, text=True, check=True, timeout=60)
    report = json.loads(result.stdout)
    with source.with_suffix(".audio-validation.json").open("x") as stream:
        json.dump(report, stream, indent=2)
        stream.write("\n")
    print(result.stdout.strip())


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.SubprocessError) as error:
        sys.exit(str(error))

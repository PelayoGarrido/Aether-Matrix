#!/usr/bin/env python3
"""Developer-only native interpretation/render qualification; no neural Python runtime.

Uses existing pinned assets and independently inspects PCM24/SMF output. Does
not download, install, publish, infer musical acceptance or grant model rights.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/"tools/piano"))
from verify import verify
RESERVE = 3*1024**3


def write(path, value):
    with path.open("x") as stream: json.dump(value, stream, indent=2, allow_nan=False); stream.write("\n")


def run(command, log, directory, timeout):
    started, peak, cancelled = time.monotonic(), 0, False
    with log.open("x") as stream:
        process = subprocess.Popen([str(c) for c in command], stdout=stream, stderr=subprocess.STDOUT,
                                   env=dict(os.environ, HF_HUB_OFFLINE="1", HF_HUB_DISABLE_TELEMETRY="1"))
        try:
            while process.poll() is None:
                if time.monotonic()-started > timeout: raise RuntimeError("Native job deadline exceeded")
                if shutil.disk_usage(directory).free < RESERVE or log.stat().st_size>2*1024**2: raise RuntimeError("Disk/log budget reached")
                usage = subprocess.run(["/bin/ps", "-o", "rss=", "-p", str(process.pid)], capture_output=True, text=True, timeout=2)
                if usage.stdout.strip(): peak=max(peak, int(usage.stdout.strip())*1024)
                if peak>2*1024**3: raise RuntimeError("Native job exceeds 2 GiB sampled RSS")
                time.sleep(.05)
            if process.returncode: raise RuntimeError("Native helper failed; inspect retained log: "+log.name)
        finally:
            if process.poll() is None:
                cancelled=True; process.send_signal(signal.SIGTERM)
                try: process.wait(timeout=3)
                except subprocess.TimeoutExpired: process.kill(); process.wait(timeout=3)
    return dict(seconds=time.monotonic()-started, sampled_peak_rss_bytes=peak, process_reaped=True, forced_cleanup=cancelled)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("score", type=Path); parser.add_argument("new_job", type=Path)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--build", type=Path, default=ROOT/"build")
    parser.add_argument("--weights", type=Path, default=ROOT/".build-downloads/pianist-transformer")
    parser.add_argument("--bank", type=Path, default=ROOT/".build-downloads/salamander-v3")
    args=parser.parse_args()
    if sys.platform!="darwin": raise ValueError("RSS supervision is qualified for macOS development only")
    if not 0<=args.seed<2**32: raise ValueError("Seed must be uint32")
    if args.score.is_symlink() or args.score.stat().st_size>1024**2: raise ValueError("Invalid score file")
    with args.score.open("rb") as stream: source=stream.read(1024**2+1)
    if len(source)>1024**2: raise ValueError("Score exceeds 1 MiB")
    from codec import score
    score(json.loads(source))
    # Prevent competing framework qualification jobs; native helpers separately
    # hold their cross-process lock. A future app supervisor owns the full chain.
    import fcntl
    lock_path=ROOT/".build-downloads/performance.lock"
    if lock_path.is_symlink(): raise ValueError("Invalid evaluation lock")
    with lock_path.open("a") as lock:
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX|fcntl.LOCK_NB)
        job=args.new_job.absolute()
        if shutil.disk_usage(job.parent).free < RESERVE+140*1024**2: raise ValueError("Preserve 3 GiB plus bounded render space")
        job.mkdir(mode=0o700)
        with (job/"score.json").open("xb") as stream: stream.write(source)
        build=args.build.absolute()
        interpretation=run([build/"AetherPianistPerform", args.weights.absolute(), job/"score.json", job/"expression", args.seed, "--quiet"], job/"expression.log", job, 155)
        learned=json.loads((job/"expression/interpretation.json").read_text())
        if learned.get("completed") is not True: raise ValueError("Incomplete native interpretation")
        actual=hashlib.sha256((job/"expression/performance.json").read_bytes()).hexdigest()
        if actual!=learned["performance_sha256"]: raise ValueError("Interpretation digest mismatch")
        rendered=run([build/"AetherPianoRender", args.bank.absolute(), ROOT/"tools/piano/salamander-manifest.json", ROOT/"tools/piano/salamander-layout.json", job/"expression/performance.json", job/"audio", "48000"], job/"render.log", job, 125)
        checked=verify(job/"audio");write(job/"audio/independent-validation.json",checked)
        render=json.loads((job/"audio/render.json").read_text())
        if render["source_performance_sha256"]!=actual: raise ValueError("Renderer used a different performance")
        source_notes=sorted(json.loads(source)["notes"],key=lambda n:(n["tick"], n["pitch"]))
        performance=json.loads((job/"audio/performance.json").read_text())
        if len(source_notes)!=len(performance["notes"]) or any(a["pitch"]!=b["pitch"] or a["tick"]!=b["tick"] for a,b in zip(source_notes,performance["notes"])):
            raise ValueError("Score pitches or onset anchors changed")
        for key in ("bpm", "bars", "ticks_per_beat", "sharps", "minor"):
            if performance[key]!=json.loads(source)[key]: raise ValueError("Requested musical metadata changed")
        evidence=dict(schema=1, completed=True, source_sha256=hashlib.sha256(source).hexdigest(), seed=args.seed,
                      interpretation=interpretation, rendering=rendered, native_model=learned, validation=checked,
                      no_python_ml_runtime=True, source_composition_reused=True, plugin_integration=False,
                      musical_quality_approved=False, commercial_clearance="not_established")
        write(job/"qualification.json", evidence)
        print(json.dumps(evidence,indent=2))


if __name__=="__main__": main()

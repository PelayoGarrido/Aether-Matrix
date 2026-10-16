#!/usr/bin/env python3
"""Supervised offline learned piano expression, separate from the audio thread."""
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
from codec import score

REVISION = "8f1568201d1ec035c73939d5a67c5168c0b80221"
SIZE = 271506656
SHA = "8fb9111efc147adcc61a4885bd42a6e6b625626d9fb8576811916927aa8281d0"
RESERVE = 3 * 1024**3


def atomic_json(path, value):
    temporary = path.with_name(path.name + ".part")
    with temporary.open("x") as stream:
        json.dump(value, stream, indent=2, allow_nan=False); stream.write("\n")
        stream.flush(); os.fsync(stream.fileno())
    os.link(temporary, path); temporary.unlink()


def worker(weights, job, seed, timing):
    started = time.monotonic()
    if hasattr(os, "nice"):
        os.nice(10)
    # Imported only inside the supervised process; the parent stays lightweight.
    import torch
    from model import Pianist
    from codec import perform, timing_range
    from reference import compare
    torch.set_num_threads(2); torch.set_num_interop_threads(1)
    source = json.loads((job / "score.json").read_text())
    notes, tokens = score(source)
    model = Pianist(str(weights))
    loaded = time.monotonic()
    with torch.inference_mode():
        # Cached decoding must match a full-prefix pass before generating music.
        prefix = [2] + tokens[:15]
        reference = compare(model, tokens, prefix)
        model.encode(tokens)
        full = model.decode(prefix)
        model.encode(tokens)
        for token in prefix:
            cached = model.decode([token])
        cache_error = float((full - cached).abs().max())
        if not torch.isfinite(full).all() or cache_error > 0.005:
            raise ValueError("Cached/full-prefix numerical parity failed")
        model.check_used_weights()
        model.encode(tokens)
        output, previous, performance_ms = [], 2, 0
        timing_mass = []
        rng = torch.Generator(device="cpu").manual_seed(seed)
        ranges = ((5, 133), (261, 5252), (134, 261), (262, 5261)) + ((5261, 5389),) * 4
        generation_start = time.monotonic()
        for i in range(len(tokens)):
            logits = model.decode([previous])
            if not torch.isfinite(logits).all():
                raise ValueError("Non-finite model logits")
            if i % 8 == 0:
                previous = tokens[i]  # Pitch is hard-conditioned, as upstream.
            else:
                low, high = ranges[i % 8]
                if timing == "tempo" and i % 8 == 1:
                    constrained_low, constrained_high = timing_range(notes, i // 8, source, performance_ms)
                    timing_mass.append(float(logits[low:high].softmax(-1)[constrained_low-low:constrained_high-low].sum()))
                    low, high = constrained_low, constrained_high
                probabilities = logits[low:high].softmax(dim=-1)
                ordered, indices = probabilities.sort(descending=True)
                remove = ordered.cumsum(0) - ordered > 0.95
                ordered[remove] = 0
                previous = low + int(indices[torch.multinomial(ordered, 1, generator=rng)])
            output.append(previous)
            if i % 8 == 1:
                performance_ms += previous - 261
            if i % 128 == 0:
                print(f"Learned expression {i}/{len(tokens)} tokens", flush=True)
        atomic_json(job / "tokens.json", dict(input=tokens, output=output))
        try:
            result, diagnostics = perform(source, notes, output, "tempo" if timing == "tempo" else "global")
        except ValueError as error:
            atomic_json(job / "rejection.json", dict(reason=str(error), load_seconds=loaded-started,
                        generation_seconds=time.monotonic()-generation_start, seed=seed, timing=timing,
                        cache_full_prefix_max_logit_error=cache_error, quality_approved=False))
            raise
    result.update(candidate="yhj137/pianist-transformer-rendering", revision=REVISION)
    atomic_json(job / "performance.json", result)
    report = dict(schema=1, completed=True, model=result["candidate"], revision=REVISION, weights_sha256=SHA,
                  composition_candidate=source.get("candidate", "external_score"), composition_revision=source.get("revision", ""),
                  score_sha256=hashlib.sha256((job / "score.json").read_bytes()).hexdigest(),
                  performance_sha256=hashlib.sha256((job / "performance.json").read_bytes()).hexdigest(),
                  torch_version=torch.__version__, device="cpu", threads=2, seed=seed, timing=timing, top_p=0.95, temperature=1.0,
                  timing_allowed_probability_mass_min=min(timing_mass) if timing_mass else None,
                  timing_allowed_probability_mass_mean=sum(timing_mass)/len(timing_mass) if timing_mass else None,
                  load_seconds=loaded-started, generation_seconds=time.monotonic()-generation_start,
                  cache_full_prefix_max_logit_error=cache_error, reference_layer_parity=reference,
                  commercial_clearance="not_established", neural_waveform_generation=False, quality_approved=False,
                  diagnostics=diagnostics)
    atomic_json(job / "model-result.json", report)
    print(json.dumps(report, indent=2), flush=True)


def main():
    if len(sys.argv) == 6 and sys.argv[1] == "--worker":
        worker(Path(sys.argv[2]), Path(sys.argv[3]), int(sys.argv[4]), sys.argv[5]); return
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("score", type=Path); parser.add_argument("new_job", type=Path)
    parser.add_argument("--weights", type=Path, default=Path(__file__).resolve().parents[2] / ".build-downloads/pianist-transformer/model.safetensors")
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--timing", choices=("tempo", "free"), default="tempo")
    args = parser.parse_args()
    if sys.platform != "darwin":
        raise ValueError("This Python evaluator is qualified for macOS development only")
    # OS-owned advisory lock is released on exit/crash; no stale PID file and
    # no competing model jobs consuming the resource budget concurrently.
    import fcntl
    lock_path = Path(__file__).resolve().parents[2] / ".build-downloads/performance.lock"
    if lock_path.is_symlink():
        raise ValueError("Invalid evaluator lock path")
    lock = lock_path.open("a")
    try: fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError: raise RuntimeError("Another learned performance is already running")
    if not 0 <= args.seed < 2**32:
        raise ValueError("Seed outside uint32 range")
    with args.score.open("rb") as stream:
        data = stream.read(1024**2 + 1)
    if len(data) > 1024**2:
        raise ValueError("Score exceeds 1 MiB")
    score(json.loads(data))
    weights = args.weights.absolute()
    if weights.is_symlink() or not weights.is_file() or weights.stat().st_size != SIZE:
        raise ValueError("Pinned model missing or changed")
    with weights.open("rb") as stream:
        if hashlib.file_digest(stream, "sha256").hexdigest() != SHA:
            raise ValueError("Model SHA256 verification failed")
    job = args.new_job.absolute()
    if shutil.disk_usage(job.parent).free < RESERVE + 8 * 1024**2:
        raise ValueError("Preserve 3 GiB free plus job budget")
    job.mkdir(mode=0o700)  # Exclusive; never replace an existing job.
    with (job / "score.json").open("xb") as stream:
        stream.write(data)
    started, peak_rss = time.monotonic(), 0
    env = dict(os.environ, HF_HUB_OFFLINE="1", HF_HUB_DISABLE_TELEMETRY="1", OMP_NUM_THREADS="2", VECLIB_MAXIMUM_THREADS="2")
    with (job / "worker.log").open("x") as log:
        process = subprocess.Popen([sys.executable, str(Path(__file__).resolve()), "--worker", str(weights), str(job), str(args.seed), args.timing], stdout=log, stderr=subprocess.STDOUT, env=env)
        try:
            while process.poll() is None:
                if time.monotonic() - started > 150:
                    raise RuntimeError("Learned performance exceeded 150 seconds")
                if shutil.disk_usage(job).free < RESERVE or (job / "worker.log").stat().st_size > 2 * 1024**2:
                    raise RuntimeError("Learned performance disk/output budget reached")
                if sys.platform == "darwin":
                    usage = subprocess.run(["/bin/ps", "-o", "rss=", "-p", str(process.pid)], capture_output=True, text=True, timeout=2)
                    if usage.stdout.strip():
                        peak_rss = max(peak_rss, int(usage.stdout.strip()) * 1024)
                    if peak_rss > 2 * 1024**3:
                        raise RuntimeError("Learned performance exceeded 2 GiB sampled RSS")
                time.sleep(0.1)
            if process.returncode:
                raise RuntimeError("Learned performance failed; inspect the retained worker.log")
            report = json.loads((job / "model-result.json").read_text())
            report.update(supervised_seconds=time.monotonic()-started, sampled_peak_rss_bytes=peak_rss)
            atomic_json(job / "evaluation.json", report)
            print(json.dumps(report, indent=2))
        finally:
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
                try: process.wait(timeout=3)
                except subprocess.TimeoutExpired: process.kill(); process.wait(timeout=3)


if __name__ == "__main__":
    main()

# Learned piano expression — development evaluator

This evaluates **real pretrained Pianist Transformer weights** on the CPU, through a native helper packaged with Piano Studio, or the independent Python reference evaluator. It takes a score and predicts note intervals, velocities, durations and sustain-pedal values. It does not compose the score or generate a neural waveform. The recorded-piano helper renders the resulting shared performance to audio and matching editable MIDI.

## Provenance and suitability

- Code: [yhj137/PianistTransformer](https://github.com/yhj137/PianistTransformer/tree/747df2d12291e37f6638b39f1b71517e579ad48c), source revision `747df2d12291e37f6638b39f1b71517e579ad48c`, Apache-2.0.
- Weights: [pianist-transformer-rendering](https://huggingface.co/yhj137/pianist-transformer-rendering/tree/8f1568201d1ec035c73939d5a67c5168c0b80221), revision `8f1568201d1ec035c73939d5a67c5168c0b80221`, 271,506,656 bytes, SHA256 `8fb9111efc147adcc61a4885bd42a6e6b625626d9fb8576811916927aa8281d0`. Model card labels them Apache-2.0.
- Architecture: compressed eight-token note embeddings, 10 encoder layers and two cached decoder layers; 135M checkpoint parameters. The fixed inference runtime adapts the original model and [Transformers v4.54.0 T5Gemma](https://github.com/huggingface/transformers/blob/v4.54.0/src/transformers/models/t5gemma/modeling_t5gemma.py), Apache-2.0. Licence retained in `docs/licenses/PIANIST-APACHE-2.0.txt`.
- The [paper](https://arxiv.org/html/2512.02652v1) lists Aria-MIDI, GiantMIDI-Piano, PDMX, POP909 and Pianist8 pretraining, followed by ASAP fine-tuning. Some corpus terms are non-commercial or require further review. **Training, redistribution and output commercial clearance are not established.** A permissive model label does not resolve every underlying right. This is a documented research candidate, not a commercial-use promise.

No training data is concealed, relabelled as our own or downloaded in bulk. Adding more data does not remove existing conditions. The repository code, optional weights, instruments and generated outputs have distinct provenance. Free/open-source distribution does not itself waive third-party conditions.

## Native app path

Piano Studio uses `PianistEngine` with embedded fixed ONNX encoder/decoder graphs and verified original BF16 safetensors. Encoder and decoder sessions run sequentially with two CPU threads; the runtime needs no Python or Torch. `export_native.py` reproduces the tiny architecture graphs using the standard library; original weights remain external and unbundled. `checkpoint-tensors.json` and `native-manifest.json` pin the contract.

```sh
build/AetherPianistPerform .build-downloads/pianist-transformer INPUT_SCORE.json NEW_JOB 42 --quiet
```

Native sampling is deterministic on the tested native backend but intentionally differs from Torch's random stream. Teacher-forced numerical parity is checked separately using `compare_native.py`; it is not a claim of identical generated performances or musical quality. Backward feasible timing windows preserve upcoming chord/interval constraints; retained probability mass and boundary corrections remain visible in each report.

## Isolated reference environment and disk budget

Python is an explicitly authorised exception for local AI evaluation/export; the web continues using pnpm. This setup targets **macOS Apple Silicon / Python 3.12**, with two CPU threads. Other hardware/framework paths are unqualified.

The measured installed environment occupies about 747 MB; weights add 272 MB. The optional recorded instrument adds 748 MB. Allow installation/download working space and outputs **above a 3 GiB free-space reserve**. Measure physical available space first; do not add packages or checkpoints when that reserve would be crossed. No system Python packages, CUDA, GUI dependencies, Transformers installation or hub cache are needed.

```sh
df -k /System/Volumes/Data
python3.12 -m venv .build-downloads/performance-env
SSL_CERT_FILE=/etc/ssl/cert.pem PIP_CERT=/etc/ssl/cert.pem .build-downloads/performance-env/bin/python -m pip install --no-cache-dir --only-binary=:all: --require-hashes -r tools/performance/requirements-macos-arm64-py312.txt
.build-downloads/performance-env/bin/python -m pip check
python3 tools/performance/download.py
mkdir -p build/piano-evidence
.build-downloads/performance-env/bin/python tools/performance/evaluate.py INPUT_SCORE.json build/piano-evidence/NEW_JOB --seed 42
```

Do not install this platform-specific lock on another platform. `dependency-review.json` records PyPI/OSV checks for all 11 selected versions on 2026-10-17. No applicable known advisories were returned at that time; this is not a guarantee against future/unpublished vulnerabilities. PyTorch 2.14.1 is beyond the affected ranges of the reviewed checkpoint-loading advisories, including [CVE-2026-24747](https://github.com/pytorch/pytorch/security/advisories/GHSA-63cw-57p8-fm3p). The evaluator never calls `torch.load`, executes downloaded Python, uses `trust_remote_code`, or connects to a model hub during inference. The unused optional NumPy bridge may emit a warning in Torch's developer log; generation does not use NumPy.

The native piano consumes the result:

```sh
build/AetherPianoRender .build-downloads/salamander-v3 tools/piano/salamander-manifest.json tools/piano/salamander-layout.json build/piano-evidence/NEW_JOB/performance.json build/piano-evidence/NEW_AUDIO_JOB 48000
python3 tools/piano/verify.py build/piano-evidence/NEW_AUDIO_JOB
```

## Tempo and inference contract

- Score input: 2–512 notes, 1–32 bars, 40–300 BPM, piano range 21–108, one fixed tempo/key, no pre-existing expressive offsets or ambiguous same-pitch overlaps. Long gaps/durations outside the model's millisecond vocabulary fail explicitly.
- Pitch tokens are forced to the supplied score, following upstream's approach. Velocity, duration, interval and four pedal points per note are sampled from learned logits with seeded top-p 0.95 sampling.
- Default `--timing tempo` constrains **cumulative** learned onset time to ±90 ms around the score, or ±45 ms for chord groups, plus at most 0.5 ms from vocabulary rounding; the first note retains its score onset at millisecond precision. This prevents cumulative tempo drift while preserving learned timing within that envelope. No post hoc time stretching is applied. The report records probability mass retained by the timing constraint: a small value signals model/score disagreement and needs musical review.
- Optional `--timing free` evaluates unconstrained intervals, then aligns first/last score onsets globally. Excessive stretch or >500 ms drift fails. Earlier F-major, D-minor and C-major free-expression candidates were rejected. Those failures remain in the evidence; constrained success does not erase them or prove unrestricted expressive quality.
- Velocity zero and zero duration are excluded. Same-pitch releases are resolved once, and note tails at the requested boundary are clipped once with counts recorded. The resulting schema 2 performance drives **both** MIDI and WAV.
- Before each inference, compact encoder/logits are compared against selected original eager-layer definitions in float32, and cached decoding against a full-prefix pass. This checks the fixed no-padding context, not every upstream framework/dtype/model configuration. `reference_layers.py` retains original selected definitions with limited imports/aliases for numerical validation.

Each job is exclusive, with an OS advisory lock allowing only one model job at a time, two CPU threads, reduced priority, 150-second supervision, a sampled 2 GiB RSS cap on macOS, bounded logs and a 3 GiB disk reserve. Cancellation terminates/kills the child. Failed jobs retain diagnostics; success requires `evaluation.json` and matching performance hash. This is process isolation **without an OS sandbox**. Piano Studio supervises the packaged native helper independently of this Python reference evaluator.

`regression-score.json` is an original, hand-authored four-bar test score, **not an AI composition or training corpus**. It tests expression separately. Primary end-to-end evidence reuses the previously generated SkyTNT scores with their original provenance.

## Still required for stable v1

Broad musical evaluation, appropriate composition controls, host transport integration, project restoration, native first-run asset setup, repeated/long-job stress tests, verified support limits, sample realism/true-peak/loudness review, external musician acceptance and a documented rights decision. Passing event/format/numerical tests is not production-quality musical approval.

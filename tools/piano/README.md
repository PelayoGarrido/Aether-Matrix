# Recorded piano development renderer

This is an **offline development component**, not a feature in the released plugin. It renders a shared, validated performance to stereo 24-bit WAV (44.1, 48 or 96 kHz) and matching editable MIDI at 9600 PPQ. Composition/performance may be learned; the waveform comes from recorded piano samples, not neural audio synthesis.

## Instrument

[Salamander Grand Piano v3](https://github.com/sfzinstruments/SalamanderGrandPiano/tree/3382bf9496bba2486f5ab0de55a264d1dfc38404): Alexander Holm's Yamaha C5 recordings, 48 kHz/24-bit stereo, 16 velocity layers. Reconstruction by kinwie, retuning by Markus Fiedler, sample-start offsets by Jeff Learman. Samples/mapping are CC-BY-3.0, which permits commercial use subject to its conditions. Keep attribution, the licence, source link and modification notices; no endorsement is implied. See `docs/licenses/SALAMANDER-CC-BY-3.0.txt`.

The 643 assets occupy **748,421,477 bytes**. They are optional, revision/size/SHA256 pinned, excluded from Git and existing installers. Downloads are explicit and never run on plugin startup. Acquire only when the complete remaining download fits above a 3 GiB free-space reserve:

```sh
python3 tools/piano/download.py .build-downloads/salamander-v3
python3 tools/piano/download.py .build-downloads/salamander-v3 --verify-only
```

Completed assets are reusable after interruption. Existing changed files/partials are preserved and require explicit inspection before retry. No archive extraction, package installation or remote script execution is involved.

## Build and render

Use the project's existing JUCE/ONNX build configuration, with `AETHER_BUILD_GENERATION_TOOLS=ON` and `AETHER_BUILD_TESTS=ON`. Reuse caches under storage pressure.

```sh
cmake --build build --config Release --target AetherPianoRender AetherPianoTests --parallel 2
ctest --test-dir build --output-on-failure
mkdir -p build/piano-evidence
build/AetherPianoRender .build-downloads/salamander-v3 tools/piano/salamander-manifest.json tools/piano/salamander-layout.json INPUT_PERFORMANCE.json build/piano-evidence/NEW_JOB 48000
```

On Windows, the executable normally resides under `build/Release/`. Generation tools require the verified ONNX SDK for the existing MIDI evaluator; the recorded piano itself does not perform ONNX inference.

The input is schema 1/2 performance JSON, bounded to 512 notes, 32 bars, 40–300 BPM, piano keys 21–108 and 2048 pedal events. Schema 2 adds signed microsecond onset/release offsets and continuous sustain values. A single event timeline drives both audio and MIDI. Notes shorter than one millisecond and overlapping same-pitch key-down voices are rejected; any model adapter must resolve them once before both exports. Sustain may continue past a key release.

Outputs: `instrument.wav`, `performance.mid`, `performance.json`, `CREDITS.txt`, `render.json`. Consumers must require `render.json` with `completed=true`, then check file hashes. It is written last. Use a new job directory; existing jobs are never overwritten.

## Limits and fidelity

- Offline, separate process; no disk access/decoding in the audio callback. 768 MiB decoded-sample LRU cache and 256 active voices; excessive jobs fail explicitly rather than stealing voices silently.
- 32-tap windowed-sinc resampling, source-layer offsets and retuning; distinct hammer/string releases and deterministic pedal-noise alternation. A repeated attack fades the prior same-pitch main voice over 0.5 seconds, following the source instrument's note-polyphony/off-time intent. Half-pedal damping, pedal recapture and the fade shape are approximations. This is a restricted native mapping, **not full SFZ/ARIA emulation**. Sympathetic resonance and broad repeated-note/pedal qualification remain open.
- Maximum 120 seconds per render, sampled 2 GiB physical-footprint limit on macOS, cancellation and a 3 GiB disk reserve. Other platforms lack the macOS footprint measurement and are not memory-qualified. One offline spool is removed after conversion; preflight budgets both copies.
- Only attenuation when necessary to leave -1 dBFS sample-peak headroom; no loudness boost, limiter or mastering. Seeded TPDF dither for PCM24. True-peak/loudness and musical acceptance are separate release gates.
- Eight-second bounded tail; an audible residual at the limit fails instead of being silently truncated. Failures clean only this job's known output files.

Tests use small synthetic FLAC fixtures, so CI does not download the sample bank. They verify MIDI/audio physical-time agreement, event ordering, malformed contracts, pedal response, dynamics, chunk invariance, the voice cap and tampered samples. Passing these tests does not establish concert-piano realism or DAW readiness.

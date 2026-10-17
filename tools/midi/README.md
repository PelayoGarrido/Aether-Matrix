# Offline trained MIDI candidate evaluation

This tool runs a genuine pretrained MIDI transformer on CPU. The same native generator now ships as a supervised Piano Studio helper and is **not approved for commercial production**. Read [v1 readiness](../../docs/v1/READINESS.md) before using its generated output. No weights, generated music, Python ML environment or instrument samples are included in the repository or installers.

## Build with an existing SDK

Requirements: CMake 3.24+, C++20, Python 3.11+ and a verified ONNX Runtime 1.31.0 SDK. macOS inference currently targets arm64; Windows x64 compilation is included in CI, but inference/hardware qualification is pending.

From the repository root, using the SDK already fetched by the main build:

```sh
cmake -S tools/midi -B build-midi -DCMAKE_BUILD_TYPE=Release -DAETHER_ORT_ROOT="$PWD/.build-downloads/onnxruntime-osx-arm64-1.31.0"
cmake --build build-midi --config Release --parallel 2
```

For a fresh SDK in another location, supply its verified extracted directory. Windows uses the matching x64 SDK and `-A x64`. Alternatively the main CMake project can compile the tools with `-DAETHER_BUILD_GENERATION_TOOLS=ON`; the app then bundles the three Studio helpers. The Python evaluation wrapper expects the standalone `build-midi` layout above.

## Optional research-only weights

`download_candidate.py` retrieves only the two official self-contained ONNX files for `skytnt/midi-model-tv2o-medium`, revision `0f8f265d4330f4e46527ac2313200254c5757f5f`.

| File | Bytes | SHA256 |
| --- | ---: | --- |
| model_base.onnx | 821713887 | `1af68e16a0509936caeb70d322ab30619d784c7af72df019f3a59276c67c1bc4` |
| model_token.onnx | 116661381 | `cce9376abe8f5c8d51d1b110580cc8501d38679af55ebd5a332e757e62ab9911` |

The two files total 938,375,268 bytes. The downloader requires that space **in addition to** a 3 GiB free-space reserve, rechecks storage while writing, resumes partial downloads only when the server honours the byte range, and verifies the whole SHA256 before promotion. Interrupted or failed-integrity partials remain local for inspection; no unrelated files are deleted. No Hugging Face cache or duplicate weight copy is created.

```sh
python3 tools/midi/download_candidate.py
python3 tools/midi/evaluate.py build-midi/evidence/piano-f-major-91 --events 256 --seed 91 --bpm 90 --bars 8 --program 0 --sharps -1
```

Use a new output prefix without an extension. `--sharps` is the MIDI key signature (-7 to 7); `--minor 1` selects its relative minor. Examples: C major = 0/0, D minor = -1/1, F major = -1/0. These condition the model; they do not hard-filter chromatic pitches. Events are an inference budget, not a guarantee that an incomplete phrase fills every bar. Generation stops on EOS, timeline or event limit.

Both the wrapper and the native helper verify the pinned hashes **before model loading**. It supervises a separate process, preserves a 3 GiB storage reserve and bounds process duration to 150 seconds. The native generation watchdog limits the job to 120 seconds including load time and 4 GiB of sampled process footprint after sessions are constructed; the macOS wrapper additionally monitors resident memory during loading. Do not invoke the native binary directly on arbitrary/unverified ONNX files. This is not an OS security sandbox.

Outputs:

- `.json`: model/revision, seed/controls, sampled timing/memory, raw generated tokens and the canonical note performance.
- `.mid`: tempo, key signature, program, notes and note-offs from that performance. Same-pitch intersections are trimmed in the performance first because MIDI 1.0 cannot distinguish voices on the same channel/pitch.
- `.validation.json`: an independent SMF parser checks event lengths, pairing, bounds, channel, metadata and agreement with JSON; it reports velocity variation, scale diagnostics and polyphony without claiming musical quality.
- `.log`: local diagnostics. Output creation refuses to overwrite existing files. A failed multi-file write is reported; the app controller additionally commits a verified complete take atomically.
- `.process.json`: supervisor timing and sampled resident-memory peak on macOS (separate from the native physical-footprint counter).

To hear the performance on macOS without downloading a sample library:

```sh
python3 tools/midi/audition_macos.py build-midi/evidence/piano-f-major-91.json
```

This invokes the system DLS synthesizer and creates a stereo 44.1 kHz / 16-bit WAV with a two-second release tail, plus an audio validation report. It is a listening reference, not Aether's final instrument or a commercial-use clearance. Only matching MIDI/JSON performances are accepted.

Network-free integrity/format tests:

```sh
python3 -m unittest discover -s tests -p test_midi_tools.py -v
```

The inference protocol was adapted from [SkyTNT/midi-model](https://github.com/SkyTNT/midi-model/tree/f504d5cb58f769ab0f2909c679238f6621034573), Apache-2.0, with the full licence retained. No upstream demo server or package installation is needed. The V2 token timing grid is one sixteenth of a beat, not millisecond expressive timing.

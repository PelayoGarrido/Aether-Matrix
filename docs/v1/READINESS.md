# v1 readiness — 2026-10-16

**Decision: do not release v1.** The public plugin remains a preview synthesizer. The new trained-model experiment is a separate developer tool, not a feature shipped in the plugin. Successful local inference does not establish production-quality music, a commercially cleared model, or DAW reliability.

## Product requirement

Generate a usable single-instrument performance locally, with a requested tempo and key, editable MIDI, and convincing production audio derived from that **same performance**. Evaluate piano first; additional instruments must earn their own quality and hardware qualification. Random chord selection or a reference ONNX decoder is not the generative model required here.

## Implemented and measured

- PolyBLEP saw/pulse synthesis, suppression of upper partials near Nyquist, an independent 5.2 Hz vibrato oscillator and sample-rate-corrected envelope decay.
- Live note-off handling preserves a newer pitch and ignores another channel's note-off. Ordinary note-offs have a short release; CC120 remains an emergency mute.
- Native C++ ONNX inference of an actual pretrained symbolic-music transformer, with cached attention, fixed single-channel/instrument conditioning, bounded events and timeline, seeded sampling, and independent MIDI/performance agreement checks.
- Separate evaluation process with two inference threads, reduced scheduling priority, cancellation, time/memory supervision, disabled runtime telemetry and a 3 GiB disk reserve. No Python ML framework or cloud inference was installed. Candidate downloads are revision-, size- and SHA256-pinned; no remote Python code is executed.
- Tests cover oscillator alias energy, physical-time envelope decay, MIDI note release/channel behaviour, corrupt downloads, storage exhaustion, ignored HTTP ranges, and malformed MIDI. Native plugin tests also pass under Rosetta.

The spectral result is approximately 24.59 dB less alias energy for saw and 22.52 dB for pulse **in one coherent high-frequency oscillator test**. It is not a claim that the complete signal chain is alias-free; nonlinear stages still require an oversampling review.

See [measurement evidence](evidence.json) and [reproduction instructions](../../tools/midi/README.md). Measurements apply to one Apple M1 / 16 GB Mac. Memory values are sampled physical footprints, not guaranteed maxima or system-wide memory consumption. Energy, thermal behaviour, 8 GB machines and Windows inference have not been qualified.

## Candidate assessment

[SkyTNT tv2o-medium](https://huggingface.co/skytnt/midi-model-tv2o-medium) is useful for testing local trained inference: its official model card identifies a transformer, Apache-2.0 weights and ONNX exports. It is **not approved as the v1 model**.

The model card lists Los Angeles MIDI, Monster MIDI and SymphonyNet as training sources. Both [Los Angeles MIDI](https://huggingface.co/datasets/projectlosangeles/Los-Angeles-MIDI-Dataset) and [Monster MIDI](https://huggingface.co/datasets/projectlosangeles/Monster-MIDI-Dataset) are labelled CC-BY-NC-SA-4.0. The difference between those dataset terms and the Apache model label is unresolved for this product. Do not interpret the model label as clearance of all training, redistribution, output or commercial-use rights. This is a release blocker pending documented clarification or replacement with a model whose provenance fits the commercial requirement.

Quality also fails qualification. One tested seed produced uniform velocity 127; another varied velocity only from 52–59. A C-major run initially stayed on one triad. These are limited observations, not a universal verdict on the model. Its tokenizer uses a sixteenth-note time grid; this experiment does not generate expressive microtiming or sustain-pedal control. Key is conditioning, not guaranteed enforcement; the F-major example has a duration-weighted diatonic fraction of about 98.8%. Chromatic notes can be musically valid, so that fraction is a diagnostic rather than an automatic quality score.

The first evaluator implementation incorrectly restricted time sampling to the remaining timeline, trapping notes at the final onset. That defect was fixed: time is now sampled naturally, generation stops at the requested boundary, and note tails are clipped to it. Results from that discarded constraint are excluded from the current candidate comparison.

Listening references use the already-installed Apple DLS instrument. They help assess composition, and are **not** the proposed production piano, a neural audio renderer, or a sample library for redistribution. No listening panel or subjective quality approval has occurred.

## Release gates

| Gate | Required evidence | Current status |
| --- | --- | --- |
| Commercial suitability | Reviewed model, code, corpus and instrument rights, including redistribution conditions | Blocked: candidate corpus terms unresolved |
| Trained musical generation | Local inference and controllable complete performances, useful across representative prompts/seeds | Candidate inference works; consistency and expression unqualified |
| Performance expression | Musical phrasing, velocity, articulation, sustain and timing without random cosmetic humanisation | Missing trained expressive stage |
| Audio production quality | A qualified instrument/renderer, convincing dynamics and releases, no unexplained clipping, discontinuities or rendering defects | Current oscillator synth / system DLS are insufficient |
| MIDI/audio agreement | One versioned performance drives playback, MIDI and audio; edits regenerate the same events; tempo and tails preserved | Tool MIDI/JSON agree; plugin integration and final renderer missing |
| Real-time reliability | AU/VST3 host tests at 44.1/48 kHz and 64/128/256 samples, cancellation, repeated jobs and 30-minute load sessions | Synthetic tests only; one deadline exceeded under concurrent work |
| Hardware limits | Measured peak memory, latency, disk consumption, thermal/energy behaviour and recovery on every supported baseline | One M1/16 GB evaluated; no validated support matrix |
| Musical acceptance | Blinded, loudness-matched review by producers of representative complete phrases against a suitable reference instrument | Not performed |
| Distribution | Native CI, host validation, clean-machine install/uninstall, signing/notarisation and publication/privacy review | Development checks only; no new stable installer |

A zero-miss synthetic timing sample does not cancel a failure in another sample. The callback benchmark runs unpaced on a normal thread and does not reproduce an audio-device deadline, real-time thread priority or a DAW project. It cannot certify absence of dropouts.

## Next engineering decisions

1. Resolve the commercially usable composition and performance models before integrating or bundling weights. Do not spend storage on more checkpoints merely because their code is permissively licensed.
2. Use a shared performance representation with high-resolution musical time, explicit note durations/velocity, pedal and articulation events. Keep score time and expressive timing together; a global tempo map must not unintentionally change other tracks in an existing DAW session.
3. Qualify a production instrument and its disk-streaming/render path. A conventional licensed sampler is compatible with genuinely AI-generated composition/performance, but must be labelled accurately if the waveform itself is not neurally generated.
4. Put inference in a crash-isolated helper with one bounded job, cancellation, resource checks and an atomic result handoff. The current CLI is an evaluation component; it is not yet a packaged, app-supervised service.
5. Run musical and DAW acceptance tests before deciding on v1. Existing previews and the finished website must not imply that those gates have passed.

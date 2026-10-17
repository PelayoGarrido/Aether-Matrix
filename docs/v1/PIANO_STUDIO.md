# Piano Studio — v1.0.0-rc.1

## Run

Install the macOS package (AU, VST3, standalone) or Windows VST3 installer. The application starts in Matrix Synth; select Piano Studio. Obtain optional assets using the root README and choose their common parent folder. Generate a take, audition it, then drag the completed WAV or MIDI into a DAW. Use a variation number to reproduce sampling on the tested native backend; identical bytes across hardware/runtime versions are not guaranteed.

The resources folder contains:

```text
midi-tv2o-medium/model_base.onnx
midi-tv2o-medium/model_token.onnx
pianist-transformer/model.safetensors
salamander-v3/LICENSE
salamander-v3/README.md
salamander-v3/Samples/...
```

App generation uses fixed tempo 40–300 BPM, key signature, 4/8/16 bars and a bounded note budget. Models have their own vocabulary limits: successful composition does not guarantee compatible expression. Explicit rejection leaves the previous take available. Key is conditioning rather than a hard diatonic filter. Rendering is 48 kHz PCM24 stereo, with release tail and attenuation-only sample-peak protection; no loudness/true-peak mastering certification is implied.

## MIDI edit and render

Keep the take's MIDI with your DAW project, edit notes/velocity/pedal, export a single-piano MIDI file and select **Render edited MIDI** in Studio. The imported performance directly drives recorded audio and the new MIDI, without another AI expression pass. Controls update from its metadata. The app provides a piano-roll view, not an in-plugin note editor.

Accepted SMF types 0/1: at most 1 MiB, 64 tracks, 8192 events, 512 notes, 2048 pedal events, PPQ 96–15360; one piano channel, pitches 21–108, fixed integer 40–300 BPM, fixed key, 4/4, at most 32 bars. Drum channel, tempo/key changes, unsupported controllers, pitch bend, SysEx, ambiguous overlaps and malformed/truncated metadata are rejected. Keep tempo changes out of the exported take. Missing final sustain reset is canonicalised once.

## Files and recovery

Each UUID take directory contains canonical performance, MIDI, WAV, credits and verification/provenance records. `take.json` is the completion marker; incomplete jobs are not draggable. Diagnostics can contain paths from your own machine and are local: review them before posting a bug report. No private output files are uploaded automatically.

Cancel cooperatively terminates helpers, then kills/reaps unresponsive children. macOS helpers also exit after parent reparenting (100 ms polling); Windows uses kill-on-close Jobs. A machine power loss can leave a diagnostic-only job. The app never deletes unrelated output directories or attempts an automatic retry that conceals failed evidence.

One job across instances is admitted; each stage has wall-time, memory and disk supervision, with a 3 GiB free-space reserve. macOS memory caps are sampled, not OS-enforced ceilings. Windows uses a 2 GiB Job limit. Process isolation does not provide an operating-system sandbox. Trusted pinned models are required.

## Support limits

Measured generation baseline: Apple M1 / 16 GB, macOS 15.7.3. macOS 14+ universal binaries include Intel Matrix Synth; the bundled ONNX SDK and generative helpers execute natively on Apple Silicon, including when a translated plugin calls them. Windows x64 is a build target; model performance and real host acceptance need separate qualification. No 8 GB or Intel generation promise is made.

Studio audition uses its own clock. Incoming live MIDI, DAW transport sync, automatic project restoration and native automatic asset setup remain unimplemented. Save exported takes with the session. Commercial model/output suitability and musical production quality remain unapproved: see READINESS.md.

# v0.1 release review

This is an engineering preview. Record exact builds, host versions and results before promoting it to a stable release.

## Required evidence

- [ ] Universal macOS Release build and x64 Windows Release build.
- [ ] Processor/export/model tests pass on both platforms.
- [ ] `codesign --verify --deep --strict` passes; `lipo` confirms both plugin slices.
- [ ] Bundle inspection finds no developer paths; runtime/model/licence files are present.
- [ ] Native macOS package expands correctly. Install on a clean Mac with quarantine present.
- [ ] Windows installer builds, installs and uninstalls on a clean Windows 10/11 machine.
- [ ] Logic Pro AU validation, playback, MIDI, automation, reopen and WAV/MIDI drag.
- [ ] FL Studio macOS/Windows VST3 scan, playback, automation, reopen and file drag.
- [ ] Ableton Live and REAPER VST3 smoke checks.
- [ ] Intel macOS hybrid fallback and non-48-kHz fallback are audible and stable.
- [ ] Repeated model replacement, invalid models, failed export path and low disk space.
- [ ] Long playback, rapid automation, oversized blocks, multiple instances and unload during render.
- [ ] Instruments / ThreadSanitizer checks for races, locks, malloc and CPU/energy behavior.
- [ ] Release source matches binary build commit; installer checksums published.

The Actions release workflow produces draft prereleases only after both native builds pass. A manual run without a tag produces artifacts without inventing a release tag. The Pages workflow publishes only after the same commit's native and source jobs pass, and includes both installers from that build. Enable GitHub Pages with **GitHub Actions** as its source.

Unsigned package approval, ad-hoc integrity signatures and a clean build are separate from Developer ID signing, notarization, Windows Authenticode reputation and real host acceptance. None of those external checks is implied by the code or CI files.

## Local evidence — 2026-10-16

The universal macOS Release build completed with Xcode 26.3 / AppleClang 17. Processor tests passed natively on arm64 and under Rosetta on x86_64. The native tests also executed the bundled reference ONNX model; Rosetta exercised the hybrid fallback. No C++ compiler warnings remained. GNU Make emitted an upstream JUCE VST3 manifest-helper jobserver warning; it did not prevent manifest generation.

AU, VST3 and Standalone signatures, universal slices, model/runtime resources and development-path checks passed. The unsigned macOS installer was generated and expanded without installing: the two plugin signatures, three model copies and exact postinstall script were verified. The Standalone UI opened and generated both draggable file pills. Installation into system plugin folders and real DAW validation remain pending.

The website was checked at 375, 768 and 1280 pixels without horizontal overflow. Matrix buttons, keyboard sliders, audio start/stop and installation details were exercised; no browser console warnings/errors were observed. Reduced-motion and visibility handling are implemented; OS-level reduced-motion emulation was not exercised in this browser session.

## Interface update — 2026-10-16

The editor now uses a wide metallic enclosure, a recessed style-weight display, two large modules, two compact modules, illuminated percentage readouts and accessible X/Y sliders. AU, VST3 and Standalone share this editor. The plotted lobes represent synthesis style weights, not a frequency equalizer. The overlaid waveform comes from the actual audio scope.

The website uses an actual native capture, licensed photo/video, and a recorded native four-bar hybrid export. Its former Web Audio approximation and generated SVG decorations have been removed. Native browser animations require no added JavaScript dependencies.

Keep this build a **pre-release**. Automated builds and unit checks establish engineering evidence, but clean-machine installation, real AU/VST3 host validation, stress testing and a complete Windows user-path check remain required before stable promotion. Signing/notarization improve distribution and must be reported separately; open-source status does not require a stable release label or a trained RAVE model.

## v1.0.0-rc.1 — 2026-10-17

Piano Studio now ships with native composition, expression and recorded-render helpers. Its optional weights and sample bank remain outside the installers. This release is deliberately a **candidate**; the stable-release requirements in docs/v1/READINESS.md remain open. Stage 3 evidence records final local verification separately from earlier stages. Publication does not certify clean-machine installation, commercial output rights, musical acceptance or 30-minute DAW reliability.

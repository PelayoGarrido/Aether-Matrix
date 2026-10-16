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

The Actions release workflow produces draft prereleases only after both native builds pass. A manual run without a tag produces artifacts without inventing a release tag. The Pages workflow publishes only after the same commit's native and source jobs pass, and includes that build's macOS installer. Enable GitHub Pages with **GitHub Actions** as its source.

Unsigned package approval, ad-hoc integrity signatures and a clean build are separate from Developer ID signing, notarization, Windows Authenticode reputation and real host acceptance. None of those external checks is implied by the code or CI files.

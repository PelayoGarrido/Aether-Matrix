# Aether Matrix

An open-source AU / VST3 instrument with a 2D style matrix, local synthesis and four-bar WAV + MIDI export. Move between hip-hop, classical, pop, electronic and cinematic synthesis voices, then edit the phrase in your DAW.

[Website](https://PelayoGarrido.github.io/Aether-Matrix/) · [Builds](https://github.com/PelayoGarrido/Aether-Matrix/actions) · [Releases](https://github.com/PelayoGarrido/Aether-Matrix/releases) · [Contribute](CONTRIBUTING.md)

**v0.1 is a preview.** AU targets Logic Pro; VST3 targets FL Studio, Ableton Live and REAPER. Real host validation is tracked in [release review](docs/RELEASE_REVIEW.md). Do not treat successful compilation as DAW compatibility certification.

## What it does

- XY interpolation between five synthesis styles; MIDI input and host-tempo sequencing.
- Root key, scale, density, tension, articulation and reverb controls.
- Background export of a four-bar, 24-bit stereo WAV master and a Type-1 MIDI file with tempo, chords, bass and lead tracks.
- Drag completed WAV and MIDI files into a host. Exports remain in `~/Music/AetherMatrix_Exports/` (the user's Music folder on Windows).
- Optional, local ONNX decoding on Apple Silicon and Windows x64 at 48 kHz. Other sample rates and Intel use hybrid synthesis.

The included `rave_decoder.onnx` is a deterministic sine-basis **reference decoder, not a trained RAVE model**. No checkpoint or training corpus is included. Offline exports use the hybrid synth and will differ from live ONNX playback. Exports contain a stereo audio master and MIDI parts, not separate audio stems. Live MIDI output is disabled to avoid growing a host-owned MIDI buffer in the audio callback.

No account, telemetry, network inference or runtime download is required.

## Install

Download an installer from a published release or the artifacts of a successful CI run. Never install artifacts from an untrusted fork. Close the DAW before installation, then reopen it and rescan plugins. Administrator permission is required.

### macOS 14 or later

1. Open `Aether_Matrix_macOS.pkg` and follow the installer.
2. If blocked, open **System Settings → Privacy & Security → Open Anyway**, then reopen the installer. [Apple's instructions](https://support.apple.com/102445).

Installs a universal AU and VST3 into `/Library/Audio/Plug-Ins/Components/` and `/Library/Audio/Plug-Ins/VST3/`. The shared reference model goes in `/Library/Application Support/AetherAudioLabs/AetherMatrix/`. The current ONNX runtime has an arm64 slice; Intel runs the universal hybrid synth.

The package is unsigned and the plugins are ad-hoc signed, **not notarized**. The installer removes quarantine only from the two installed bundles, verifies their ad-hoc signatures and requests an AudioComponentRegistrar restart. This does not guarantee Gatekeeper or host acceptance on every Mac. Do not disable Gatekeeper globally. If Logic rejects a component, check it in Plug-in Manager and report the host/macOS versions.

### Windows 10/11 x64

1. Open `Aether_Matrix_Windows_Setup.exe` and follow the installer.
2. If SmartScreen offers an override for this trusted build, use **More info → Run anyway**. Managed devices may prohibit overrides; contact the administrator.

Installs VST3 into `C:\Program Files\Common Files\VST3\Aether Matrix.vst3`, the reference model into `C:\ProgramData\AetherAudioLabs\AetherMatrix`, and the required redistributable runtime files beside the plugin binary. The installer is not Authenticode signed. Use Windows Settings to uninstall.

## Build from source

Requires CMake 3.24+, Python 3 and a C++20 compiler. On macOS use full Xcode; on Windows use Visual Studio 2022 or newer with Desktop development with C++, plus Inno Setup 6 for packaging. JUCE and ONNX SDKs are fetched at configure time with pinned revisions/checksums; no package-manager install is needed. Allow at least 3 GiB of free space before starting.

```sh
git clone https://github.com/PelayoGarrido/Aether-Matrix.git
cd Aether-Matrix
./scripts/build.sh '-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64'
./installers/macos/build_pkg.sh build 0.1.0
```

Windows, in a developer PowerShell:

```powershell
python scripts/check_storage.py --reserve-gib 3
cmake -S . -B build -A x64
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure
& "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe" "/DBuildDir=$pwd\build" installers/windows/installer.iss
```

Before packaging Windows locally, copy the Microsoft x64 CRT redistributable DLLs from Visual Studio's `VC/Redist/MSVC/<version>/x64/Microsoft.VC*.CRT/` into the VST3 bundle's `Contents/x86_64-win/` directory, as CI does. Do not copy DLLs from arbitrary system folders or download sites.

Outputs are in `build/Aether_Matrix_artefacts/Release/`; installers are in `build/installers/`. Builds never install directly into your DAW plugin folders. Use `-DAETHER_ENABLE_ONNX=OFF` for synthesis-only development, or `-DAETHER_ORT_ROOT=/path/to/extracted/sdk` to reuse an existing 1.31.0 SDK. See [models](docs/MODELS.md), [architecture](docs/ARCHITECTURE.md) and [dependency review](docs/DEPENDENCIES.md).

The website has no frontend dependencies:

```sh
cd web
pnpm dev
pnpm check
```

## License

[AGPL-3.0-or-later](LICENSE), matching JUCE 8's open-source licensing route. See [third-party notices](THIRD_PARTY_NOTICES.md). A commercial, closed-source derivative requires a separate review of JUCE and other licensing terms; this repository does not grant a commercial JUCE licence.

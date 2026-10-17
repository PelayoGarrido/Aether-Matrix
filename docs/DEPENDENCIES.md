# Dependency review — 2026-10-16

| Dependency | Original | Selected | Evidence / reason |
| --- | --- | --- | --- |
| JUCE | 8.0.4 | 8.0.16-security | Security release published 2026-10-13, protecting against malformed String input. Remain on maintained JUCE 8 rather than introducing a major migration. Shallow fetch is checked against immutable commit `ea77444750de45e036e0279bb0291144bf8e3688` before CMake integration. |
| ONNX Runtime | 1.19.2 | 1.31.0 | Official stable release published 2026-10-09. SDK archives have SHA256 pins from official release metadata. Old Intel binaries are not retained merely to satisfy a universal-runtime claim. |
| Python exporter | PyTorch, implicit/unpinned | Standard library | No large framework install, checkpoint execution or Python package supply chain is required for the reference model. |
| Website | None | None | Static HTML/CSS/JS and browser-native audio; no CDN scripts, fonts, analytics or backend costs. |
| GitHub Actions | None | Official actions, full SHA pins | Read-only defaults; artifact and Pages/release writes are restricted to dedicated jobs. Bounded build concurrency, timeout and artifact retention. |

Sources: [JUCE security release](https://github.com/juce-framework/JUCE/releases/tag/8.0.16-security), [JUCE security policy](https://github.com/juce-framework/JUCE/security), [ONNX Runtime release](https://github.com/microsoft/onnxruntime/releases/tag/v1.31.0), [Microsoft security guidance](https://github.com/microsoft/onnxruntime/blob/v1.31.0/SECURITY.md), [ONNX advisories](https://github.com/onnx/onnx/security/advisories).

The public repository advisory endpoints for JUCE and ONNX Runtime returned no published entries during this review. That is not evidence that either dependency is vulnerability-free. ONNX (the separate model-format package) has its own advisories and must not be confused with the C++ runtime bundled here. Current native release notes include protobuf/ONNX security fixes; do not fall back to old SDKs without reviewing their embedded dependencies. Unknown zero-days cannot be ruled out by an advisory search.

Keep model loading local and restricted to trusted files. Review release notes and advisories again before publishing a new installer. A version bump must update both SDK hashes, the runtime filename in the bundling script, minimum OS assumptions, notices and model tests.

## Native Studio review — 2026-10-17

Stage 3 adds no runtime package: it reuses pinned JUCE/ORT, platform process APIs and original verified external safetensors. The graph exporter and asset acquisition use Python's standard library. The separately authorised Torch 2.14.1 / safetensors 0.8.0 reference environment is not shipped or required by the app; its hash-locked review is in tools/performance/dependency-review.json. Public JUCE/ORT advisory APIs were checked again and returned no entries; this cannot rule out unpublished issues. The reference loader is now byte-restricted, and Studio parses models only in bounded helpers. Unknown zero-days are not detectable by this review.

Existing system FFmpeg was not used for release acceptance and is not a product dependency. True-peak/loudness qualification remains pending a reviewed meter rather than relying on that unrelated installation.

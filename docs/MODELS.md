# Model contract

The included model is an original, deterministic sine-basis reference decoder. It demonstrates local ONNX inference; it is not a trained RAVE model and is not marketed as one. The previous exporter created an untrained convolutional network with random weights. That would not establish musical or RAVE performance.

Rebuild the included model with `python3 scripts/export_rave_onnx.py`. The legacy filename is retained for compatibility. The exporter uses only Python's standard library, writes inline weights, uses ONNX IR 8 / opset 17 and has no network or checkpoint dependencies. The file is embedded in the plugin as a fallback as well as installed in the shared model directory.

Custom models must have exactly one float32 input `[1,16,1]`, one float32 output `[1,1,2048]`, inline tensor weights, and a 48 kHz audio interpretation. Dynamic shapes, external tensor files, multiple inputs/outputs and non-float tensors are rejected. Files are capped at 16 MiB and sessions are created from memory so external weights have no model directory to resolve against. Output samples are checked for finiteness and clipped to [-1,1]. These checks do not sandbox a graph or cap all intermediate allocations. Only load models you trust.

The loader runs on the worker thread. A load error enables hybrid fallback and is shown in the UI. Changing models does not reset the live FIFO; the audio consumer discards stale samples after a model-epoch change. Inference errors disable the layer. FIFO underruns decay toward silence rather than holding a DC sample.

The decoder produces 2048-sample windows with a periodic Hann window and 1024-sample overlap/add. The latent controls are illustrative, not a learned mapping between genres. This layer is enabled only at 48 kHz; it does not resample a model trained for another sample rate. The worker and queued audio introduce control latency, and offline exports use the hybrid engine.

The bundled ONNX Runtime 1.31.0 SDKs target macOS arm64 and Windows x64. The plugin binary itself is universal on macOS; failed runtime loading on Intel leaves hybrid synthesis available. A recent universal ONNX SDK can be supplied via `AETHER_ORT_ROOT` only after independently verifying its version, architectures, minimum OS, licences and security status. Older Intel runtimes are not silently bundled.

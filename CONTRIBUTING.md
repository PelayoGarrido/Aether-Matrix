# Contributing

Open an issue with the OS, CPU architecture, DAW version, plugin format, sample rate, buffer size, and steps to reproduce. Attach the smallest useful project or test case. Remove personal file paths, audio, credentials and private session data from logs before posting.

For code changes, fork the repository and open a pull request. Keep unrelated refactors separate. By contributing, you agree that your contribution is licensed under AGPL-3.0-or-later and that you have permission to submit it.

## Checks

```sh
python3 scripts/privacy_check.py
python3 scripts/verify_model.py
./scripts/build.sh
```

For website changes run `pnpm --dir web check`, then check a narrow mobile viewport, keyboard controls, audio start/stop, tab visibility and reduced motion. No dependency installation is necessary.

Audio callbacks must not allocate, acquire locks, access files, instantiate ONNX sessions or dispatch UI work. Use bounded preallocated storage and preserve the FIFO's single-producer/single-consumer ownership. Include meaningful behavioral tests for DSP, model, export and threading changes. Keep code comments technical and limited to non-obvious invariants or host workarounds.

New runtime dependencies need a pinned version, integrity verification, licence notice and current advisory review. Do not submit trained models without source/provenance, redistribution terms and the documented input/output contract. Host compatibility claims need recorded tests in `docs/RELEASE_REVIEW.md`.

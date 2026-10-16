#!/usr/bin/env python3
"""Fetch the pinned evaluation candidate without a package manager or ML cache.

Weights are not part of Aether's installers. No remote Python is executed.
"""
import hashlib
import os
from pathlib import Path
import shutil
import ssl
import sys
import urllib.request

REVISION = "0f8f265d4330f4e46527ac2313200254c5757f5f"
FILES = (
    ("model_base.onnx", 821713887, "1af68e16a0509936caeb70d322ab30619d784c7af72df019f3a59276c67c1bc4"),
    ("model_token.onnx", 116661381, "cce9376abe8f5c8d51d1b110580cc8501d38679af55ebd5a332e757e62ab9911"),
)
RESERVE = 3 * 1024**3
ROOT = Path(__file__).resolve().parents[2] / ".build-downloads" / "midi-tv2o-medium"


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def ensure_space(path, required=0):
    if shutil.disk_usage(path).free < RESERVE + required:
        raise RuntimeError("Insufficient storage: preserve at least 3 GiB free")


def main():
    ROOT.mkdir(parents=True, exist_ok=True)
    if ROOT.is_symlink():
        raise RuntimeError("Model directory must not be a symlink")
    remaining = 0
    for name, size, sha in FILES:
        target, partial = ROOT / name, ROOT / (name + ".part")
        if target.is_symlink() or partial.is_symlink():
            raise RuntimeError("Model files must not be symlinks")
        if target.exists():
            if target.stat().st_size != size or digest(target) != sha:
                raise RuntimeError(f"Existing {name} failed integrity verification")
        else:
            offset = partial.stat().st_size if partial.exists() else 0
            if offset > size:
                raise RuntimeError(f"Invalid partial size for {name}")
            remaining += size - offset
    ensure_space(ROOT, remaining)
    # Use the system CA store on macOS; never disable TLS verification.
    context = ssl.create_default_context(cafile="/etc/ssl/cert.pem" if sys.platform == "darwin" else None)
    for name, size, sha in FILES:
        target, partial = ROOT / name, ROOT / (name + ".part")
        if target.exists():
            print(f"Verified {name}", flush=True)
            continue
        offset = partial.stat().st_size if partial.exists() else 0
        if offset < size:
            url = f"https://huggingface.co/skytnt/midi-model-tv2o-medium/resolve/{REVISION}/onnx/{name}"
            headers = {"User-Agent": "AetherMatrix-candidate-evaluation/1"}
            if offset:
                headers["Range"] = f"bytes={offset}-"
            request = urllib.request.Request(url, headers=headers)
            with urllib.request.urlopen(request, context=context, timeout=30) as response:
                if not response.url.startswith("https://"):
                    raise RuntimeError("Unexpected insecure redirect")
                if offset and (response.status != 206 or not response.headers.get("Content-Range", "").startswith(f"bytes {offset}-")):
                    raise RuntimeError("Server did not honour the partial download; retained existing bytes")
                expected = size - offset
                if int(response.headers.get("Content-Length", expected)) != expected:
                    raise RuntimeError("Unexpected model length")
                last_report = offset // (64 * 1024**2)
                with partial.open("ab") as stream:
                    while offset < size:
                        chunk = response.read(min(1024**2, size - offset))
                        if not chunk:
                            raise RuntimeError("Incomplete download; run again to resume")
                        ensure_space(ROOT, len(chunk))
                        stream.write(chunk)
                        offset += len(chunk)
                        report = offset // (64 * 1024**2)
                        if report != last_report:
                            print(f"{name}: {offset * 100 // size}%", flush=True)
                            last_report = report
                    if response.read(1):
                        raise RuntimeError("Model response exceeds pinned size")
                    stream.flush()
                    os.fsync(stream.fileno())
        if partial.stat().st_size != size or digest(partial) != sha:
            raise RuntimeError(f"SHA256 mismatch for {name}; partial retained for inspection")
        os.replace(partial, target)
        print(f"Verified {name}", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError) as error:
        sys.exit(str(error))

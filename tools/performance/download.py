#!/usr/bin/env python3
"""Explicitly fetch pinned expressive-model weights; never execute remote code."""
import hashlib
import os
from pathlib import Path
import shutil
import ssl
import urllib.request
from evaluate import REVISION, SIZE, SHA, RESERVE


def main():
    root = Path(__file__).resolve().parents[2] / ".build-downloads/pianist-transformer"
    root.mkdir(parents=True, exist_ok=True)
    target = root / "model.safetensors"
    if root.is_symlink() or target.is_symlink():
        raise ValueError("Choose ordinary model paths")
    if target.exists():
        with target.open("rb") as stream:
            if target.stat().st_size != SIZE or hashlib.file_digest(stream, "sha256").hexdigest() != SHA:
                raise ValueError("Existing model changed; preserved for inspection")
        print("Pinned model verified"); return
    if shutil.disk_usage(root).free < RESERVE + SIZE + 8 * 1024**2:
        raise ValueError("Preserve 3 GiB free plus model download")
    partial = root / "model.safetensors.part"
    if partial.exists() or partial.is_symlink():
        raise ValueError("Existing partial preserved; inspect/remove it explicitly before retry")
    context = ssl.create_default_context(cafile="/etc/ssl/cert.pem" if Path("/etc/ssl/cert.pem").exists() else None)
    url = f"https://huggingface.co/yhj137/pianist-transformer-rendering/resolve/{REVISION}/model.safetensors"
    try:
        with partial.open("xb") as output, urllib.request.urlopen(url, context=context, timeout=45) as response:
            count, sha = 0, hashlib.sha256()
            while chunk := response.read(1024**2):
                if count + len(chunk) > SIZE or shutil.disk_usage(root).free < RESERVE + 2 * 1024**2:
                    raise ValueError("Model download budget reached")
                output.write(chunk); sha.update(chunk); count += len(chunk)
            if count != SIZE or sha.hexdigest() != SHA:
                raise ValueError("Model integrity failed")
        os.link(partial, target); partial.unlink()
    except BaseException:
        if partial.is_file() and not partial.is_symlink(): partial.unlink()
        raise
    print("Verified 271506656 bytes. Apache-labelled weights; training/output commercial clearance is not established. See docs/v1/MODEL_REVIEW.md.")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Explicit, revision/SHA256-pinned recorded piano acquisition. No ML cache."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import ssl
import urllib.parse
import urllib.request

REVISION = "3382bf9496bba2486f5ab0de55a264d1dfc38404"
RESERVE = 3 * 1024**3
HERE = Path(__file__).resolve().parent


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def entries():
    manifest = json.loads((HERE / "salamander-manifest.json").read_text())
    if manifest["revision"] != REVISION or len(manifest["files"]) != 643:
        raise ValueError("Unexpected instrument manifest")
    seen = set()
    for item in manifest["files"]:
        name = item["path"]
        parts = name.split("/")
        valid = name in ("LICENSE", "README.md") or (
            len(parts) == 2 and parts[0] == "Samples" and parts[1].endswith(".flac")
            and not parts[1].startswith(".")
            and all(c.isascii() and (c.isalnum() or c in "#._-") for c in parts[1]))
        if not valid or name in seen or not 0 < item["bytes"] <= 32 * 1024**2:
            raise ValueError("Unsafe instrument entry")
        sha = item["sha256"]
        if len(sha) != 64 or any(c not in "0123456789abcdef" for c in sha):
            raise ValueError("Invalid instrument digest")
        seen.add(name)
    return manifest["files"]


def verify(root, item):
    path = root / item["path"]
    if root.is_symlink() or (root / "Samples").is_symlink() or path.is_symlink():
        raise ValueError("Instrument paths must not be symlinks")
    return path.is_file() and path.stat().st_size == item["bytes"] and digest(path) == item["sha256"]


def fetch(root, item, context):
    target = root / item["path"]
    if target.exists():
        raise ValueError("Existing unverified asset preserved; move it aside explicitly")
    target.parent.mkdir(parents=True, exist_ok=True)
    partial = target.with_name(target.name + ".part")
    # Never truncate an existing or linked file, even after an interrupted job.
    if partial.exists() or partial.is_symlink():
        raise ValueError("Existing partial preserved; inspect/remove it explicitly before retry")
    url = "https://raw.githubusercontent.com/sfzinstruments/SalamanderGrandPiano/" + REVISION + "/" + urllib.parse.quote(item["path"], safe="/")
    try:
        with partial.open("xb") as output, urllib.request.urlopen(url, context=context, timeout=45) as response:
            if response.status != 200:
                raise ValueError("Unexpected instrument HTTP response")
            count, sha = 0, hashlib.sha256()
            while chunk := response.read(1024**2):
                if count + len(chunk) > item["bytes"] or shutil.disk_usage(root).free < RESERVE + 2 * 1024**2:
                    raise ValueError("Instrument download budget reached")
                output.write(chunk); sha.update(chunk); count += len(chunk)
            if count != item["bytes"] or sha.hexdigest() != item["sha256"]:
                raise ValueError("Instrument integrity check failed")
        # Atomic publish without replacing another writer's file.
        import os
        os.link(partial, target)
        partial.unlink()
    except BaseException:
        if partial.is_file() and not partial.is_symlink():
            partial.unlink()  # This function exclusively created this partial.
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("--verify-only", action="store_true")
    args = parser.parse_args()
    root = args.root.absolute()
    if root.is_symlink():
        raise ValueError("Choose an ordinary instrument directory")
    root.mkdir(parents=True, exist_ok=True)
    files = entries()
    missing = [item for item in files if not verify(root, item)]
    if args.verify_only:
        if missing:
            raise ValueError(f"{len(missing)} missing or changed instrument assets")
        print(f"Verified {len(files)} assets; {sum(i['bytes'] for i in files)} bytes")
        return
    remaining = sum(item["bytes"] for item in missing)
    if shutil.disk_usage(root).free < RESERVE + remaining + 16 * 1024**2:
        raise ValueError("Preserve 3 GiB free plus the complete remaining instrument download")
    context = ssl.create_default_context(cafile="/etc/ssl/cert.pem" if Path("/etc/ssl/cert.pem").exists() else None)
    for index, item in enumerate(missing):
        fetch(root, item, context)
        print(f"Verified {index + 1}/{len(missing)} new assets", flush=True)
    print("Instrument ready. Retain LICENSE, README.md and exported CREDITS.txt.")


if __name__ == "__main__":
    main()

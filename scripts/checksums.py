#!/usr/bin/env python3
import hashlib
from pathlib import Path
import sys
root = Path(sys.argv[1])
files = sorted(p for p in root.iterdir() if p.suffix in {'.pkg', '.exe'})
if not files:
    sys.exit('No installer to hash')
# Separate names avoid collisions when both platform artifacts are attached to a release.
for path in files:
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    path.with_suffix(path.suffix + '.sha256').write_text(f'{digest}  {path.name}\n')
    print(path.name, digest)

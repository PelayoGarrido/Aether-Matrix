#!/usr/bin/env python3
"""Screen the Git index (or source tree before Git initialization) before publication."""
import re
from pathlib import Path
import subprocess
root = Path(__file__).resolve().parents[1]
tracked = subprocess.run(['git', '-C', str(root), 'ls-files', '-z'], capture_output=True)
files = [root / p.decode() for p in tracked.stdout.split(b'\0') if p] if tracked.returncode == 0 else [
    p for p in root.rglob('*') if p.is_file() and not any(
        part in {'.git', '.build-downloads', 'node_modules'} or part.startswith('build') for part in p.relative_to(root).parts)]
patterns = [rb'/Users/[A-Za-z0-9_.-]+/', rb'-----BEGIN [A-Z ]*PRIVATE KEY-----',
            rb'gh[pousr]_[A-Za-z0-9]{20,}', rb'github_pat_[A-Za-z0-9_]{30,}',
            rb'AKIA[0-9A-Z]{16}']
issues = []
for file in files:
    if any(p.startswith('.env') or p in {'.DS_Store', '.git-credentials'} for p in file.parts):
        issues.append(str(file.relative_to(root)))
    if file.suffix in {'.onnx', '.png', '.jpg', '.woff2'}:
        continue
    data = file.read_bytes()
    if any(re.search(pattern, data) for pattern in patterns):
        issues.append(str(file.relative_to(root)))
if issues:
    raise SystemExit('Publication screen failed: ' + ', '.join(issues))
print(f'Publication screen passed for {len(files)} source files. Review does not replace secret scanning.')

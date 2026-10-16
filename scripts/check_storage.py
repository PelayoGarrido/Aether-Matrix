#!/usr/bin/env python3
import argparse
import shutil
import sys
from pathlib import Path
p = argparse.ArgumentParser()
p.add_argument('--reserve-gib', type=float, default=2.5)
a = p.parse_args()
free = shutil.disk_usage(Path(__file__).resolve().parent).free / 1024**3
print(f'Available storage: {free:.2f} GiB; minimum reserve: {a.reserve_gib:.2f} GiB')
if free < a.reserve_gib:
    sys.exit('Build stopped. Free space manually or use another volume; nothing was deleted.')

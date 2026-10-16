#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
python3 scripts/check_storage.py --reserve-gib 3
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release "$@"
python3 scripts/check_storage.py --reserve-gib 2.5
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure

#!/usr/bin/env bash
set -euo pipefail
native_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cmake -S "$native_root" -B "$native_root/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$native_root/build" --parallel 2
ctest --test-dir "$native_root/build" --output-on-failure

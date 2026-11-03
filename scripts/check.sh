#!/usr/bin/env bash
# DOCTRINE pre-commit gate: format check -> build (-Werror, ASan/UBSan) -> full parallel tests.
# Usage: scripts/check.sh [--gpu]   (--gpu also runs the Vulkan/window tests; needs a display)
set -euo pipefail
cd "$(dirname "$0")/.."

echo "== lint: clang-format =="
mapfile -t files < <(git ls-files --cached --others --exclude-standard '*.cpp' '*.h')
if ((${#files[@]})); then
  clang-format --dry-run --Werror "${files[@]}"
fi

echo "== build (debug, -Werror, sanitizers) =="
cmake --preset debug >/dev/null
cmake --build --preset debug

echo "== tests =="
ctest --preset debug

if [[ "${1:-}" == "--gpu" ]]; then
  echo "== gpu tests =="
  ctest --preset debug-gpu
fi
echo "== all gates passed =="

#!/usr/bin/env bash
# Pre-commit / CI entry point (AGENTS.md section 4): format check,
# layering check, host build and the full ctest suite (including the
# simulator smoke tests). Safe to run from any directory; it cd's to
# the repository root itself.
#
# Note: this only checks formatting (clang-format --dry-run --Werror);
# it does not reformat anything. Run `clang-format -i` yourself first
# if this fails because of formatting drift.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# Every C/C++ source file in the repo, except build output and
# components/wisp_ui/fonts/ (a ported third-party font -- see
# AGENTS.md section 6 / docs/references.md).
mapfile -t SOURCE_FILES < <(find . \
    \( -path './build' -o -path './.git' -o -path './components/wisp_ui/fonts' \) -prune -o \
    -type f \( -name '*.hpp' -o -name '*.cpp' -o -name '*.h' -o -name '*.c' \) -print)

echo "==> clang-format --dry-run --Werror (${#SOURCE_FILES[@]} files)"
clang-format --dry-run --Werror "${SOURCE_FILES[@]}"

echo "==> tools/check_layering.py"
python3 tools/check_layering.py

echo "==> cmake -S simulator -B build/sim -G Ninja"
cmake -S simulator -B build/sim -G Ninja

echo "==> cmake --build build/sim"
cmake --build build/sim

echo "==> ctest --test-dir build/sim"
ctest --test-dir build/sim --output-on-failure

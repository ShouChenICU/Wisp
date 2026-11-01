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

# The clang-format version this repo is formatted against -- this is
# the single place that version is recorded (see Task 14 fix round 1):
# .github/workflows/ci.yml's "Install pinned clang-format" step reads
# this exact line (by grep) to install the matching PyPI package,
# rather than duplicating the literal. Different clang-format major
# versions format the same source differently, so a mismatch here (a
# locally installed clang-format from apt/brew/etc. that isn't this
# one) can make --dry-run --Werror below disagree with CI -- warn,
# don't fail, since the actual authority is whatever CI has pinned.
CLANG_FORMAT_VERSION="23.1.3"

CLANG_FORMAT_ACTUAL="$(clang-format --version | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"
echo "==> clang-format --version: ${CLANG_FORMAT_ACTUAL} (pinned: ${CLANG_FORMAT_VERSION})"
if [[ "${CLANG_FORMAT_ACTUAL%%.*}" != "${CLANG_FORMAT_VERSION%%.*}" ]]; then
    echo "warning: clang-format major version differs from the pin (${CLANG_FORMAT_ACTUAL} vs ${CLANG_FORMAT_VERSION});" \
         "formatting here may disagree with CI" >&2
fi

# Every C/C++ source file in the repo, except build output,
# components/wisp_ui/fonts/ (a ported third-party font -- see
# AGENTS.md section 6 / docs/references.md), and (Task 15)
# firmware/build/ and firmware/managed_components/ (ESP-IDF build
# output and third-party sources fetched by the component manager --
# both gitignored, neither ours to format).
mapfile -t SOURCE_FILES < <(find . \
    \( -path './build' -o -path './.git' -o -path './components/wisp_ui/fonts' \
       -o -path './firmware/build' -o -path './firmware/managed_components' \) -prune -o \
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

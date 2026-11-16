#!/usr/bin/env bash
# Vendors stb_image_write.h and the repository's LICENSE from nothings/stb
# into third_party/stb_image_write/stb-<commit7>/, verbatim (AGENTS.md
# section 6; spec 3.6). Downloads both files at the pinned commit, refuses
# them unless their SHA-256 values match the pinned ones, and replaces only
# that directory. Upgrading means changing the three constants below,
# rerunning this script and updating the directory name in
# third_party/stb_image_write/CMakeLists.txt, docs/references.md and
# THIRD_PARTY_NOTICES.md.
set -euo pipefail

# stb_image_write.h v1.16.
STB_COMMIT="aeb69073d7570a481245a665dc17726d68827062"
STBIW_SHA256="03f49804a8cf8a28c7ce4ed870e0eb642737ef97249aec30fdd3a8dc7886ae40"
LICENSE_SHA256="bebfe904b14301657e4e5d655c811d51fd31b97c455b9cc2d8600d6bac6cff63"
STB_URL="https://raw.githubusercontent.com/nothings/stb/${STB_COMMIT}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${ROOT}/third_party/stb_image_write/stb-${STB_COMMIT:0:7}"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

for f in stb_image_write.h LICENSE; do
    echo "==> downloading ${STB_URL}/${f}"
    curl -fsSL -o "${WORK}/${f}" "${STB_URL}/${f}"
done

echo "==> verifying SHA-256"
(
    cd "${WORK}"
    printf '%s  %s\n%s  %s\n' \
        "${STBIW_SHA256}" stb_image_write.h \
        "${LICENSE_SHA256}" LICENSE | sha256sum -c -
)

rm -rf "${DEST}"
mkdir -p "${DEST}"
cp -p "${WORK}/stb_image_write.h" "${DEST}/stb_image_write.h"
cp -p "${WORK}/LICENSE" "${DEST}/LICENSE"
echo "==> vendored stb_image_write at ${STB_COMMIT:0:7} into ${DEST#"${ROOT}"/}"

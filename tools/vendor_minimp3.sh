#!/usr/bin/env bash
# Vendors minimp3.h, minimp3_ex.h and the repository's LICENSE from
# lieff/minimp3 into third_party/minimp3/minimp3-<commit7>/, verbatim
# (AGENTS.md section 6; media-library spec). Downloads the files at the
# pinned commit, refuses them unless their SHA-256 values match the pinned
# ones, and replaces only that directory. Upgrading means changing the
# constants below, rerunning this script and updating the directory name in
# third_party/minimp3/CMakeLists.txt, docs/references.md and
# THIRD_PARTY_NOTICES.md.
set -euo pipefail

MP3_COMMIT="142823796ab8efa55e1bf11406028f3be77ecceb"
MINIMP3_SHA256="f9592be69e8b94044e9da6f45f993c9708000abae38cf6f44b70c699de55b509"
MINIMP3_EX_SHA256="bdda836f5ea2de782bce7cf122a41a3d76fad57d960dfb47b6765dc631779de0"
LICENSE_SHA256="6a1ee543e5282cd9061881edf462e6fdab181f328da71fc2c9a6950a80e94d01"
MP3_URL="https://raw.githubusercontent.com/lieff/minimp3/${MP3_COMMIT}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${ROOT}/third_party/minimp3/minimp3-${MP3_COMMIT:0:7}"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

for f in minimp3.h minimp3_ex.h LICENSE; do
    echo "==> downloading ${MP3_URL}/${f}"
    curl -fsSL -o "${WORK}/${f}" "${MP3_URL}/${f}"
done

echo "==> verifying SHA-256"
(
    cd "${WORK}"
    printf '%s  %s\n%s  %s\n%s  %s\n' \
        "${MINIMP3_SHA256}" minimp3.h \
        "${MINIMP3_EX_SHA256}" minimp3_ex.h \
        "${LICENSE_SHA256}" LICENSE | sha256sum -c -
)

rm -rf "${DEST}"
mkdir -p "${DEST}"
for f in minimp3.h minimp3_ex.h LICENSE; do
    cp -p "${WORK}/${f}" "${DEST}/${f}"
done
echo "==> vendored minimp3 at ${MP3_COMMIT:0:7} into ${DEST#"${ROOT}"/}"

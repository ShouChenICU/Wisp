#!/usr/bin/env bash
# Vendors the official Lua release into third_party/lua/lua-<version>/
# (scene-runtime-core spec 2.1: the upstream sources are taken verbatim,
# never patched). Downloads the tarball, refuses it unless its SHA-256
# matches the pinned value (the one lua.org publishes), and replaces only
# third_party/lua/lua-<version>/ with the tarball's README and src/.
# Upgrading means changing the two constants below, rerunning this
# script and updating third_party/lua/CMakeLists.txt's directory name.
set -euo pipefail

LUA_VERSION="5.5.1"
LUA_SHA256="1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce"
LUA_URL="https://www.lua.org/ftp/lua-${LUA_VERSION}.tar.gz"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${ROOT}/third_party/lua/lua-${LUA_VERSION}"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "==> downloading ${LUA_URL}"
curl -fsSL -o "${WORK}/lua.tar.gz" "${LUA_URL}"

echo "==> verifying SHA-256"
echo "${LUA_SHA256}  ${WORK}/lua.tar.gz" | sha256sum -c -

mkdir "${WORK}/x"
tar -xzf "${WORK}/lua.tar.gz" -C "${WORK}/x"
SRC="${WORK}/x/lua-${LUA_VERSION}"
if [[ ! -f "${SRC}/README" || ! -d "${SRC}/src" ]]; then
    echo "error: unexpected tarball layout (no lua-${LUA_VERSION}/README or src/)" >&2
    exit 1
fi

rm -rf "${DEST}"
mkdir -p "${DEST}"
cp -p "${SRC}/README" "${DEST}/README"
cp -pR "${SRC}/src" "${DEST}/src"
echo "==> vendored Lua ${LUA_VERSION} into ${DEST#"${ROOT}"/}"

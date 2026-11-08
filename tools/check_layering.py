#!/usr/bin/env python3
"""Enforces Wisp's portable-layer layering rule (AGENTS.md section 3,
docs/architecture.md):

  - The portable layers (wisp_base, wisp_os, wisp_hal, wisp_core,
    wisp_net, wisp_voice, wisp_admin, wisp_ui, wisp_scene, wisp_agent,
    wisp_shell, wisp_app) must
    never #include a platform header:
    anything starting with esp_, freertos/, driver/, hal/ (ESP-IDF's, as
    opposed to wisp_hal's own wisp/hal/ headers), soc/, rom/, lwip/,
    mbedtls/, SDL or curl/, or exactly sdkconfig.h, nvs.h, nvs_flash.h or
    sdmmc_cmd.h, or a bmgr-generated header (basename starting with
    dev_ or gen_board_ -- see AGENTS.md section 8). POSIX file APIs
    (<unistd.h>, <sys/stat.h>, <dirent.h>, ...) are *not* forbidden:
    both ESP-IDF (via its VFS) and the host simulator provide them, so
    portable code may use them directly (docs/architecture.md).
  - Each portable layer must only #include headers from its own layer
    or a strictly lower one in AGENTS.md's order (wisp_base < wisp_os
    < wisp_hal < wisp_core < wisp_net < wisp_voice < wisp_admin < wisp_ui
    < wisp_scene < wisp_agent < wisp_shell < wisp_app; wisp_net's and
    wisp_admin's places are fixed by spec 2.2 of the connectivity-admin
    design, wisp_scene's by spec 2.2 of the scene-runtime-core design,
    wisp_agent's by spec 2 of the agent-core design, wisp_voice's and
    wisp_shell's by spec 1.4 of the MVP design) -- no reverse
    dependencies "upward", checked for every layer against every other.
  - Each portable component's CMakeLists.txt's wisp_component()
    call may only declare the two external dependencies the portable
    stack actually has (ArduinoJson for wisp_base, LVGL for wisp_ui) in
    its IDF_REQUIRES/HOST_LIBS lists -- catches a new platform or
    host-only dependency slipped in that way instead of via #include,
    which the checks above can't see at all. Its REQUIRES list is
    checked the same way as a reverse #include dependency above: on the
    ESP build, wisp_component() forwards REQUIRES straight into
    idf_component_register()'s own REQUIRES, so a platform or
    higher-layer component could otherwise be pulled in there without
    ever being #include'd.
  - Lua's headers (lua.h, lauxlib.h, lualib.h, lua.hpp, luaconf.h) may
    only be #included by wisp_scene, and only wisp_scene may REQUIRES
    the vendored `lua` component (third_party/lua): every other layer
    reaches scripts through wisp_scene's services, never the VM itself.
  - Flash access (spec 3.3 of the flash-thread design): calls to nvs_*,
    esp_partition_*, esp_ota_*, esp_flash_, spi_flash_ and esp_littlefs_,
    and #include of nvs.h, esp_ota_ops.h, esp_partition.h, esp_flash.h,
    spi_flash_mmap.h and esp_littlefs.h, may only appear in the files
    listed in FLASH_API_WHITELIST -- anywhere under components/,
    boards/, drivers/ and firmware/, the platform layer included. Every
    other path to flash goes through StorageManager or Settings on the
    flash thread.
  - Flash-access declarations (spec 3.9 of the flash-thread design):
    `flash_access = true` and brace-init forms like `flash_access{true}`
    or `.flash_access = true` may only appear in
    FLASH_ACCESS_TRUE_WHITELIST's one file (components/wisp_core/src/
    storage_worker.cpp) -- wisp.fs is the only task permitted flash
    access; every other task relies on os::TaskConfig::flash_access's
    (and EspHttpServerConfig::flash_access's) false default. Scanned over
    the same roots as the flash-API rule above.

Prints one "path:line: <message>" line per violation and exits 1 if
any were found, else exits 0 with no output.

Usage: python3 tools/check_layering.py [--root <repo-root>]
"""

import argparse
import pathlib
import re
import sys

# The portable components to scan, in AGENTS.md's layering order.
COMPONENTS = (
    "wisp_base",
    "wisp_os",
    "wisp_hal",
    "wisp_core",
    "wisp_net",
    "wisp_voice",
    "wisp_admin",
    "wisp_ui",
    "wisp_scene",
    "wisp_agent",
    "wisp_shell",
    "wisp_app",
)
SOURCE_SUFFIXES = (".hpp", ".cpp", ".h", ".c")

# Each component's header namespace (AGENTS.md section 5: "header path
# wisp/<layer>/<name>.hpp"), in the same order as COMPONENTS -- used to
# generalize the reverse-dependency check below.
LAYER_INCLUDE_PREFIX = {
    "wisp_base": "wisp/base/",
    "wisp_os": "wisp/os/",
    "wisp_hal": "wisp/hal/",
    "wisp_core": "wisp/core/",
    "wisp_net": "wisp/net/",
    "wisp_voice": "wisp/voice/",
    "wisp_admin": "wisp/admin/",
    "wisp_ui": "wisp/ui/",
    "wisp_scene": "wisp/scene/",
    "wisp_agent": "wisp/agent/",
    "wisp_shell": "wisp/shell/",
    "wisp_app": "wisp/app/",
}

# components/wisp_ui/fonts/ holds generated font data (see
# tools/gen_cjk_font.py / docs/references.md), not hand-written code, so
# it's excluded from the scan.
SKIPPED_DIR_NAMES = ("fonts",)

FORBIDDEN_PREFIXES = (
    "esp_",
    "freertos/",
    "driver/",
    "hal/",
    "soc/",
    "rom/",
    "lwip/",
    "mbedtls/",
    "SDL",
    # libcurl backs only the simulator's net::HttpClient (wisp_port_sim).
    "curl/",
    # wisp_port_sim/wisp_port_esp headers: a portable layer including one
    # of these compiles fine on the host (wisp/port/sim/* has no
    # platform headers of its own), so only a firmware build -- which
    # this checker never runs -- would actually fail; forbidding the
    # #include itself here catches it on the host instead.
    "wisp/port/",
)
FORBIDDEN_EXACT = ("sdkconfig.h", "nvs.h", "nvs_flash.h", "sdmmc_cmd.h")
# esp_board_manager's own per-device headers (dev_*.h) and the board-
# specific code `idf.py bmgr` generates (gen_board_*.*) are matched by
# basename, not full path, since boards include them as bare quoted
# filenames (see e.g. boards/waveshare_esp32_s3_touch_amoled_2_16/board.cpp's
# #include "dev_display_lcd.h") with no fixed directory prefix to
# anchor on -- both are gitignored (AGENTS.md section 8) so they can't
# be listed by exact name either.
FORBIDDEN_BASENAME_PREFIXES = ("dev_", "gen_board_")

# Lua's public headers (third_party/lua/lua-5.5.1/src): only the layer
# that owns the VM may see them.
LUA_HEADERS = ("lua.h", "lauxlib.h", "lualib.h", "lua.hpp", "luaconf.h")
LUA_HEADER_OWNERS = ("wisp_scene",)

# Non-wisp_* components a portable layer may list in REQUIRES (as
# opposed to IDF_REQUIRES): the vendored third_party/ components, which
# build the same way under idf.py (EXTRA_COMPONENT_DIRS) and on the host
# (add_subdirectory), keyed by the one layer allowed to require each.
THIRD_PARTY_REQUIRES = {"wisp_scene": {"lua"}}

# The only files that may call nvs_*/esp_partition_*/esp_ota_* or include
# their headers, each with the reason it needs raw flash access.
FLASH_API_WHITELIST = {
    "components/wisp_port_esp/settings_nvs.cpp":
        "the NVS settings backend, NVS init at boot and the factory-reset erase",
    "components/wisp_port_esp/esp_firmware.cpp":
        "the firmware (OTA) service: writes, verifies and activates the OTA slot",
    "components/wisp_port_esp/ui/esp_font_partition.cpp":
        "maps the font partition at boot, before the flash thread starts",
    "components/wisp_port_esp/dev/esp_storage.cpp":
        "mounts internal LittleFS at boot and queries its capacity",
}
FLASH_API_ROOTS = ("components", "boards", "drivers", "firmware")
# Fetched or generated code, not Wisp's own.
FLASH_API_SKIPPED_DIRS = ("managed_components", "build")
FLASH_API_HEADERS = (
    "nvs.h",
    "esp_ota_ops.h",
    "esp_partition.h",
    "esp_flash.h",
    "spi_flash_mmap.h",
    "esp_littlefs.h",
)
FLASH_API_CALL_RE = re.compile(
    r"\b((?:nvs|esp_partition|esp_ota|esp_flash|spi_flash|esp_littlefs)_[A-Za-z0-9_]*)\s*\("
)

# The only file allowed to declare flash_access = true (spec 3.9): wisp.fs
# (core::StorageWorker::task_config()) is the one task on which flash
# access is permitted.
FLASH_ACCESS_TRUE_WHITELIST = "components/wisp_core/src/storage_worker.cpp"
FLASH_ACCESS_TRUE_RE = re.compile(r"\bflash_access\s*=\s*true\b|\bflash_access\s*\{\s*true\s*\}")

INCLUDE_RE = re.compile(r'#\s*include\s*[<"]([^">]+)[">]')

# The only external (non-wisp_*) dependencies the portable stack
# actually has: ArduinoJson (wisp_base) and LVGL (wisp_ui) -- see each
# component's wisp_component() call. Anything else added to a
# components/wisp_{base..app}/CMakeLists.txt's IDF_REQUIRES/HOST_LIBS
# is either a platform dependency (which belongs in wisp_port_esp, not
# a portable layer) or a host-only one (which belongs in
# wisp_port_sim) smuggled in through CMake instead of #include, so the
# #include-based checks above can't catch it.
ALLOWED_IDF_REQUIRES = {"bblanchon__arduinojson", "lvgl__lvgl"}
ALLOWED_HOST_LIBS = {"ArduinoJson", "lvgl"}
CMAKE_LIST_KEYWORDS = ("NAME", "SRCS", "INCLUDE_DIRS", "REQUIRES", "IDF_REQUIRES", "HOST_LIBS")


def iter_source_files(root, component):
    """Yields every source file under components/<component>/, skipping
    SKIPPED_DIR_NAMES subdirectories, in a stable (sorted) order."""
    comp_dir = root / "components" / component
    if not comp_dir.is_dir():
        return
    for path in sorted(comp_dir.rglob("*")):
        if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
            continue
        rel_dirs = path.relative_to(comp_dir).parts[:-1]
        if any(part in SKIPPED_DIR_NAMES for part in rel_dirs):
            continue
        yield path


def reverse_dep_prefixes_for(component):
    """The set of #include prefixes `component` must never use: every
    layer strictly above it in COMPONENTS' order."""
    idx = COMPONENTS.index(component)
    return tuple(LAYER_INCLUDE_PREFIX[c] for c in COMPONENTS[idx + 1 :])


def is_forbidden_platform_include(include_path):
    if include_path in FORBIDDEN_EXACT:
        return True
    if any(include_path.startswith(prefix) for prefix in FORBIDDEN_PREFIXES):
        return True
    basename = include_path.rsplit("/", 1)[-1]
    return any(basename.startswith(prefix) for prefix in FORBIDDEN_BASENAME_PREFIXES)


def strip_comments(text):
    """Blanks out // line comments and /* ... */ block comments
    (replacing their characters with spaces, newlines kept as-is), so
    a commented-out #include isn't mistaken for a real one. A simple
    character-by-character state machine; doesn't special-case string
    or char literals (fine for this check -- a '//' or '/*' inside a
    string literal on an #include line isn't a pattern Wisp's own
    source uses)."""
    out = []
    in_block_comment = False
    in_line_comment = False
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if in_block_comment:
            if c == "*" and i + 1 < n and text[i + 1] == "/":
                out.append("  ")
                i += 2
                in_block_comment = False
                continue
            out.append(c if c == "\n" else " ")
            i += 1
            continue
        if in_line_comment:
            if c == "\n":
                in_line_comment = False
                out.append(c)
            else:
                out.append(" ")
            i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            in_line_comment = True
            out.append("  ")
            i += 2
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            in_block_comment = True
            out.append("  ")
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def check_file(path, component, errors):
    reverse_prefixes = reverse_dep_prefixes_for(component)
    text = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
    for lineno, line in enumerate(text.splitlines(), start=1):
        match = INCLUDE_RE.search(line)
        if not match:
            continue
        include_path = match.group(1)
        if is_forbidden_platform_include(include_path):
            errors.append(f"{path}:{lineno}: forbidden include <{include_path}>")
        elif include_path in LUA_HEADERS and component not in LUA_HEADER_OWNERS:
            errors.append(
                f"{path}:{lineno}: forbidden include <{include_path}> ({component} must "
                f"not use Lua directly; only {', '.join(LUA_HEADER_OWNERS)} may)"
            )
        elif include_path.startswith(reverse_prefixes):
            errors.append(
                f"{path}:{lineno}: reverse dependency: {component} must not include "
                f"<{include_path}>"
            )


def _extract_wisp_component_call(text):
    """Returns the argument text inside this file's one wisp_component(...)
    call, or None if there isn't one. Finds the matching close paren by
    depth counting rather than a non-greedy regex, since REQUIRES/SRCS
    values are plain component/file names that never themselves contain
    parens -- a flat scan is enough and doesn't need a real CMake
    parser."""
    start = text.find("wisp_component(")
    if start == -1:
        return None
    depth = 0
    i = start + len("wisp_component(")
    depth = 1
    args_start = i
    while i < len(text) and depth > 0:
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
        i += 1
    return text[args_start : i - 1]


def _parse_keyword_lists(args_text, keywords):
    """Splits whitespace-separated `args_text` into {keyword: [values]},
    attributing each token to the keyword most recently seen before it."""
    result = {kw: [] for kw in keywords}
    current = None
    for tok in args_text.split():
        if tok in keywords:
            current = tok
            continue
        if current is not None:
            result[current].append(tok)
    return result


def check_component_cmake(path, component, errors):
    text = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
    args_text = _extract_wisp_component_call(text)
    if args_text is None:
        # Not every portable component necessarily has one yet; the
        # #include-based checks above still apply regardless.
        return
    lists = _parse_keyword_lists(args_text, CMAKE_LIST_KEYWORDS)
    allowed_requires = set(COMPONENTS[: COMPONENTS.index(component)])
    allowed_requires |= THIRD_PARTY_REQUIRES.get(component, set())
    for value in lists["REQUIRES"]:
        if value not in allowed_requires:
            errors.append(
                f"{path}: forbidden REQUIRES <{value}> ({component} may only require "
                f"{sorted(allowed_requires) or '(nothing)'})"
            )
    for value in lists["IDF_REQUIRES"]:
        if value not in ALLOWED_IDF_REQUIRES:
            errors.append(
                f"{path}: forbidden IDF_REQUIRES <{value}> (portable layers may only "
                f"require {sorted(ALLOWED_IDF_REQUIRES)})"
            )
    for value in lists["HOST_LIBS"]:
        if value not in ALLOWED_HOST_LIBS:
            errors.append(
                f"{path}: forbidden HOST_LIBS <{value}> (portable layers may only "
                f"link {sorted(ALLOWED_HOST_LIBS)})"
            )


def iter_flash_api_files(root):
    for top in FLASH_API_ROOTS:
        top_dir = root / top
        if not top_dir.is_dir():
            continue
        for path in sorted(top_dir.rglob("*")):
            if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
                continue
            rel_dirs = path.relative_to(root).parts[:-1]
            if any(part in FLASH_API_SKIPPED_DIRS for part in rel_dirs):
                continue
            yield path


def check_flash_api(root, path, errors):
    rel = path.relative_to(root).as_posix()
    if rel in FLASH_API_WHITELIST:
        return
    parts = path.relative_to(root).parts
    # A portable layer's platform include is already reported by
    # check_file(); one message per line is enough.
    portable = len(parts) > 1 and parts[0] == "components" and parts[1] in COMPONENTS
    text = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
    for lineno, line in enumerate(text.splitlines(), start=1):
        match = INCLUDE_RE.search(line)
        if match and match.group(1) in FLASH_API_HEADERS:
            if portable and is_forbidden_platform_include(match.group(1)):
                continue
            errors.append(
                f"{path}:{lineno}: flash API include <{match.group(1)}> outside "
                f"FLASH_API_WHITELIST (go through StorageManager/Settings on the flash thread)"
            )
            continue
        for call in FLASH_API_CALL_RE.finditer(line):
            errors.append(
                f"{path}:{lineno}: flash API call {call.group(1)}() outside "
                f"FLASH_API_WHITELIST (go through StorageManager/Settings on the flash thread)"
            )


def check_flash_access_true(root, path, errors):
    rel = path.relative_to(root).as_posix()
    if rel == FLASH_ACCESS_TRUE_WHITELIST:
        return
    text = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
    for lineno, line in enumerate(text.splitlines(), start=1):
        if FLASH_ACCESS_TRUE_RE.search(line):
            errors.append(
                f"{path}:{lineno}: flash_access = true outside "
                f"{FLASH_ACCESS_TRUE_WHITELIST} (only wisp.fs may declare flash access; "
                "spec 3.9 of the flash-thread design)"
            )


def run(root):
    """Runs every check against `root` and returns the list of error
    strings found (empty if clean). Importable so tools/check_layering_
    test.py can drive it against a scratch directory tree instead of
    shelling out."""
    errors = []
    for component in COMPONENTS:
        for path in iter_source_files(root, component):
            check_file(path, component, errors)
        cmake_path = root / "components" / component / "CMakeLists.txt"
        if cmake_path.is_file():
            check_component_cmake(cmake_path, component, errors)
    for path in iter_flash_api_files(root):
        check_flash_api(root, path, errors)
        check_flash_access_true(root, path, errors)
    return errors


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    default_root = pathlib.Path(__file__).resolve().parent.parent
    parser.add_argument(
        "--root",
        default=str(default_root),
        help="repository root to scan (default: %(default)s)",
    )
    args = parser.parse_args(argv)
    root = pathlib.Path(args.root).resolve()

    errors = run(root)
    for error in errors:
        print(error)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())

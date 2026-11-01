#!/usr/bin/env python3
"""Simulator smoke tests (task-14-brief.md).

Runs the wisp_sim executable (simulator/CMakeLists.txt, built by the
host CMake project) through a handful of --headless scenarios and
checks its machine-readable stdout (the single-line-JSON
"WISP_BOOT_REPORT " and "WISP_MANIFEST " lines -- see AGENTS.md
section 4) and, for the default case, the PNG screenshot it writes.
Each case runs wisp_sim in a fresh temporary data directory.

Registered with ctest as the "sim_smoke" test (tests/CMakeLists.txt).

Usage: python3 tests/smoke/sim_smoke.py <path-to-wisp_sim>
"""

import json
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import traceback

TIMEOUT_S = 20
EXIT_AFTER_MS = 3000

# The 12 devices SimBoard::init() registers by default (task 13; see
# components/wisp_port_sim/sim_board.cpp).
EXPECTED_DEVICE_IDS = {
    "audio_in.main",
    "audio_out.main",
    "button.power",
    "button.vol_down",
    "button.vol_up",
    "clock.main",
    "display.main",
    "imu.main",
    "power.main",
    "storage.int",
    "storage.sd",
    "touch.main",
}

# display.main's safe_area for the default shape (rounded_rect, corner
# radius 40) and for --shape circle (spec 5.6's safe-area model).
DEFAULT_SAFE_AREA = {"x1": 12, "y1": 12, "x2": 467, "y2": 467}
CIRCLE_SAFE_AREA = {"x1": 71, "y1": 71, "x2": 408, "y2": 408}

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


class SmokeError(AssertionError):
    """Raised by a case function to fail that case with a message."""


def expect(condition, message):
    if not condition:
        raise SmokeError(message)


def run_sim(sim_path, data_dir, extra_args):
    args = [
        str(sim_path),
        "--headless",
        "--exit-after-ms",
        str(EXIT_AFTER_MS),
        "--data-dir",
        str(data_dir),
        *extra_args,
    ]
    try:
        return subprocess.run(args, capture_output=True, text=True, timeout=TIMEOUT_S)
    except subprocess.TimeoutExpired as exc:
        raise SmokeError(f"{' '.join(args)} timed out after {TIMEOUT_S}s") from exc


def parse_output(stdout):
    """Pulls the WISP_BOOT_REPORT/WISP_MANIFEST JSON payloads out of
    wisp_sim's stdout (one line each, prefix + single-line JSON)."""
    boot_report = None
    manifest = None
    for line in stdout.splitlines():
        if line.startswith("WISP_BOOT_REPORT "):
            boot_report = json.loads(line[len("WISP_BOOT_REPORT "):])
        elif line.startswith("WISP_MANIFEST "):
            manifest = json.loads(line[len("WISP_MANIFEST "):])
    if boot_report is None:
        raise SmokeError(f"no WISP_BOOT_REPORT line in stdout:\n{stdout}")
    if manifest is None:
        raise SmokeError(f"no WISP_MANIFEST line in stdout:\n{stdout}")
    return boot_report, manifest


def devices_by_id(value):
    """value is a boot report or manifest dict; both carry a "devices"
    list of per-device dicts keyed by "id"."""
    return {entry["id"]: entry for entry in value["devices"]}


def expected_manifest_ids(boot_report):
    """Returns the device ids WISP_MANIFEST should carry for this run,
    given its boot report. Normally that's exactly EXPECTED_DEVICE_IDS,
    but ruling 1's audio_in.main leniency (ok-or-failed) has a
    manifest-side consequence: when SimBoard::init() can't find an SDL
    capture device it never calls registry.add() for audio_in.main at
    all (see sim_board.cpp), so a "failed" audio_in.main in the boot
    report means the manifest must NOT contain it either -- a strict
    12-id equality would wrongly fail on hosts without a capture
    device. A missing/ok audio_in.main entry (or no entry at all)
    leaves the full set untouched."""
    ids = set(EXPECTED_DEVICE_IDS)
    audio_in = devices_by_id(boot_report).get("audio_in.main")
    if audio_in is not None and audio_in["state"] == "failed":
        ids.discard("audio_in.main")
    return ids


def case_expected_manifest_ids_unit(_sim_path, _tmp_root):
    """Not a wisp_sim case: a synthetic-data unit check for
    expected_manifest_ids() itself, since case_default alone only ever
    exercises whichever one of its two branches this particular host
    happens to hit for audio_in.main."""
    ok_report = {"devices": [{"id": "audio_in.main", "state": "ok", "reason": ""}]}
    expect(
        expected_manifest_ids(ok_report) == EXPECTED_DEVICE_IDS,
        "expected_manifest_ids(): an ok audio_in.main must not change EXPECTED_DEVICE_IDS",
    )

    failed_report = {
        "devices": [{"id": "audio_in.main", "state": "failed", "reason": "no capture device"}]
    }
    expect(
        expected_manifest_ids(failed_report) == EXPECTED_DEVICE_IDS - {"audio_in.main"},
        "expected_manifest_ids(): a failed audio_in.main must drop it from the expected set",
    )

    no_entry_report = {"devices": []}
    expect(
        expected_manifest_ids(no_entry_report) == EXPECTED_DEVICE_IDS,
        "expected_manifest_ids(): no audio_in.main entry at all must not change EXPECTED_DEVICE_IDS",
    )


def read_png_chunks(path):
    """Returns (width, height, idat) for the PNG at `path`: idat is the
    concatenation of every IDAT chunk's data. Only walks the chunk
    structure (length + type + data + CRC) -- doesn't verify CRCs."""
    data = path.read_bytes()
    expect(data[:8] == PNG_SIGNATURE, f"{path}: missing PNG signature")

    pos = 8
    width = height = None
    idat = bytearray()
    while pos + 8 <= len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        chunk_type = data[pos + 4:pos + 8].decode("ascii", errors="replace")
        chunk_data = data[pos + 8:pos + 8 + length]
        if chunk_type == "IHDR":
            width, height = struct.unpack(">II", chunk_data[0:8])
        elif chunk_type == "IDAT":
            idat += chunk_data
        elif chunk_type == "IEND":
            break
        pos += 8 + length + 4  # length + type + data + CRC-32
    expect(width is not None, f"{path}: no IHDR chunk found")
    return width, height, bytes(idat)


def inflate_stored_blocks(zlib_data):
    """Decodes a zlib stream made only of uncompressed ("stored")
    deflate blocks (RFC 1950/1951) -- exactly what
    components/wisp_port_sim/png_writer.cpp emits (task-14-brief.md:
    "只需跳过 zlib 头、逐块读取，不需要真正的 inflate"). Skips the
    2-byte zlib header, then walks each stored block's 1-byte header
    (final bit + BTYPE) and 4-byte length/~length pair."""
    pos = 2  # CMF, FLG
    raw = bytearray()
    while pos < len(zlib_data):
        block_header = zlib_data[pos]
        is_final = block_header & 0x1
        pos += 1
        block_len = struct.unpack("<H", zlib_data[pos:pos + 2])[0]
        pos += 4  # LEN (2 bytes) + NLEN (2 bytes, ~LEN)
        raw += zlib_data[pos:pos + block_len]
        pos += block_len
        if is_final:
            break
    return bytes(raw)


def check_screenshot(path):
    expect(path.exists(), f"{path}: screenshot was not written")
    size = path.stat().st_size
    expect(size > 1024, f"{path}: screenshot is only {size} bytes, expected > 1 KiB")

    width, height, idat = read_png_chunks(path)
    expect((width, height) == (480, 480), f"{path}: expected 480x480, got {width}x{height}")

    pixels = inflate_stored_blocks(idat)
    expect(len(set(pixels)) > 1, f"{path}: decoded pixel bytes are all identical")


def case_default(sim_path, tmp_root):
    data_dir = tmp_root / "default"
    data_dir.mkdir()
    screenshot = data_dir / "s.png"

    result = run_sim(sim_path, data_dir, ["--screenshot", str(screenshot)])
    expect(
        result.returncode == 0,
        f"exit code {result.returncode}, stderr:\n{result.stderr}",
    )

    boot_report, manifest = parse_output(result.stdout)
    expect(manifest["board"] == "simulator", f"manifest board == {manifest['board']!r}")

    manifest_devices = devices_by_id(manifest)
    manifest_ids = set(manifest_devices.keys())
    # audio_in.main is only in the manifest if the boot report didn't
    # just say it's "failed" -- see expected_manifest_ids()'s comment.
    expected_ids = expected_manifest_ids(boot_report)
    expect(
        manifest_ids == expected_ids,
        f"manifest device ids {sorted(manifest_ids)} != expected {sorted(expected_ids)}",
    )

    imu_caps = manifest_devices["imu.main"]["caps"]
    expect(
        imu_caps.get("magnetometer") is False,
        f"imu.main.caps.magnetometer == {imu_caps.get('magnetometer')!r}, expected false",
    )

    display_safe_area = manifest_devices["display.main"]["safe_area"]
    expect(
        display_safe_area == DEFAULT_SAFE_AREA,
        f"display.main safe_area == {display_safe_area}, expected {DEFAULT_SAFE_AREA}",
    )

    # All devices report "ok", with two controller-approved exceptions:
    # storage.sd (not asserted here; --no-sd covers its "missing"
    # state below) and audio_in.main, whose SDL dummy-driver capture
    # availability differs between hosts and so may legitimately come
    # back "failed" (with a reason) instead of "ok".
    report_devices = devices_by_id(boot_report)
    for device_id, entry in report_devices.items():
        if device_id == "storage.sd":
            continue
        if device_id == "audio_in.main":
            expect(
                entry["state"] in ("ok", "failed"),
                f"audio_in.main state == {entry['state']!r}, expected ok or failed",
            )
            if entry["state"] == "failed":
                expect(entry["reason"], "audio_in.main is failed with an empty reason")
            continue
        expect(
            entry["state"] == "ok",
            f"{device_id} state == {entry['state']!r}, expected ok",
        )

    check_screenshot(screenshot)


def case_disable_imu(sim_path, tmp_root):
    data_dir = tmp_root / "disable-imu"
    data_dir.mkdir()

    result = run_sim(sim_path, data_dir, ["--disable", "imu"])
    expect(
        result.returncode == 0,
        f"exit code {result.returncode}, stderr:\n{result.stderr}",
    )

    boot_report, manifest = parse_output(result.stdout)
    report_devices = devices_by_id(boot_report)
    expect("imu.main" in report_devices, "imu.main missing from boot report")
    expect(
        report_devices["imu.main"]["state"] == "failed",
        f"imu.main state == {report_devices['imu.main']['state']!r}, expected failed",
    )
    expect(
        "--disable" in report_devices["imu.main"]["reason"],
        f"imu.main reason {report_devices['imu.main']['reason']!r} doesn't mention --disable",
    )

    expect(
        "imu.main" not in devices_by_id(manifest),
        "imu.main is present in the manifest despite --disable imu",
    )


def case_no_sd(sim_path, tmp_root):
    data_dir = tmp_root / "no-sd"
    data_dir.mkdir()

    result = run_sim(sim_path, data_dir, ["--no-sd"])
    expect(
        result.returncode == 0,
        f"exit code {result.returncode}, stderr:\n{result.stderr}",
    )

    boot_report, _manifest = parse_output(result.stdout)
    report_devices = devices_by_id(boot_report)
    expect("storage.sd" in report_devices, "storage.sd missing from boot report")
    expect(
        report_devices["storage.sd"]["state"] == "missing",
        f"storage.sd state == {report_devices['storage.sd']['state']!r}, expected missing",
    )


def case_shape_circle(sim_path, tmp_root):
    data_dir = tmp_root / "shape-circle"
    data_dir.mkdir()

    result = run_sim(sim_path, data_dir, ["--shape", "circle"])
    expect(
        result.returncode == 0,
        f"exit code {result.returncode}, stderr:\n{result.stderr}",
    )

    _boot_report, manifest = parse_output(result.stdout)
    display_safe_area = devices_by_id(manifest)["display.main"]["safe_area"]
    expect(
        display_safe_area == CIRCLE_SAFE_AREA,
        f"display.main safe_area == {display_safe_area}, expected {CIRCLE_SAFE_AREA}",
    )


CASES = (
    ("expected_manifest_ids_unit", case_expected_manifest_ids_unit),
    ("default", case_default),
    ("disable_imu", case_disable_imu),
    ("no_sd", case_no_sd),
    ("shape_circle", case_shape_circle),
)


def main(argv):
    if len(argv) != 2:
        print(f"usage: {argv[0]} <path-to-wisp_sim>", file=sys.stderr)
        return 2
    sim_path = pathlib.Path(argv[1]).resolve()
    if not sim_path.is_file():
        print(f"{sim_path}: not found", file=sys.stderr)
        return 2

    tmp_root = pathlib.Path(tempfile.mkdtemp(prefix="wisp_sim_smoke_"))
    try:
        for name, case in CASES:
            try:
                case(sim_path, tmp_root)
            except Exception as exc:  # noqa: BLE001 -- any case failure (an
                # assertion from expect()/SmokeError, or a plain bug like a
                # KeyError/json decode error) should produce a readable FAIL
                # line plus the traceback, not an unhandled crash.
                print(f"FAIL {name}: {exc}", file=sys.stderr)
                print(traceback.format_exc(), file=sys.stderr)
                return 1
            print(f"PASS {name}")
    finally:
        shutil.rmtree(tmp_root, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

# Testing

Run `bash tools/ci.sh` before committing. This always checks formatting,
layering, the event topic registry, the settings schema (`tools/check_settings.py`:
no code restates a setting's default or range outside `wisp/core/settings_schema.hpp`;
it checks `components/`, `simulator/`, `boards/`, `drivers/`, `firmware/main/` and
`web/app/` line by line, tests exempt, so a value restated a few lines away from the
setting's name, or in a document, is not caught),
comment rules, translations, asset
budgets and generated files, builds the simulator and runs the C++ regression suite and admin API smoke test.
Simulator smoke covers boot, missing peripherals, network access, scene execution,
scene errors and an Agent conversation. Cheap unit tests remain enabled; test
count alone is not a useful measure of CI cost.

Run `bash tools/ci.sh --full` before release. It also runs every tool self-test
and all simulator smoke scenarios, including navigation on each screen shape,
scheduling, firmware upload and scene lifecycle variations. CI uses this mode on
pushes to main and manual workflow runs. PRs and other branches use daily mode.
Both firmware board builds remain required.

Firmware CI also requires the final `firmware/dependencies.lock` to remain
unchanged after initial configuration and both board builds. The first
configuration runs before bmgr generates its dependency manifest and therefore
resolves a smaller graph, and a clean checkout resolves every version range
afresh, taking the newest matching release. So every registry component in the
lock, including those that only arrive through another component's range, is
pinned to its locked version in `firmware/main/idf_component.yml`.
`tools/check_deps.py` (offline, part of `tools/ci.sh`) fails when one is not, so
a new dependency's unpinned transitive components show up locally instead of as
a changed lock in firmware CI. A local firmware build that changes the lock is
the same failure: pin the component rather than restoring the file.

Media tests (`tests/media/`, plus the media cases in `tests/core`, `tests/admin`,
`tests/agent`, `tests/scene`, `tests/app` and `tests/ui`) run on the host. The player
tests decode real files through minimp3 and write to a simulated audio output on a
fake clock. The media files are committed in `tests/data/media/` (a few seconds of
sine wave as MP3 and WAV, a float WAV and garbage); CI and the tests need neither
`ffmpeg` nor `lame`. To change the set, edit `tools/gen_media_test_data.sh` (it needs
`ffmpeg` and `lame`), run it and commit the outputs; keep the files under 200 KiB in
total. `tools/ci.sh` does not build the firmware, so the device decoder
(`esp_audio_codec`) is covered only by the firmware CI builds and by the device
checks in `docs/superpowers/plans/2026-11-19-media-library-device-checks.md`.

Tool self-tests can be selected with `WISP_CI_BASE=<revision> bash tools/ci.sh`.
The selector compares the merge base with HEAD and local tracked edits, and
includes new files under tools. Tool changes and known input changes select their
consumers. Build configuration, dependencies, unknown tools or changes to the
selection rules run all tool self-tests. Missing or invalid baselines also run
all self-tests. Actual repository validators always run regardless of selection.
CI supplies the PR base or previous push revision; local runs without an explicit
baseline run all tool self-tests.

Layout checks cover shapes and languages when text affects geometry. Gesture
checks cover the three shapes using one language. Keep regressions for common
invalid input, empty states, cancellation, network failures, persistence recovery,
secret handling and thread restrictions. Do not multiply combinations that
exercise the same behaviour. Real-time timeout and recovery tests remain enabled;
shorter CI must not weaken their guarantees or change product timing.

UI tree folding and output limits are checked with pure unit tests; CI does not
render a 2,000-widget scene as a wall-clock performance benchmark. Scene-list
deletion tests reuse the long-press action directly for business checks, while
the initial long press still exercises the touch path. Repeated timed gestures
are unnecessary for checking data deletion and asynchronous data discovery.

Connection screenshot previews are manual because functional tests already cover
their interactions. After building, run:

```bash
mkdir -p /tmp/wisp-connections
WISP_CONNECTIONS_SCREENSHOTS=/tmp/wisp-connections \
  build/sim/tests/shell_settings_page_test \
  --gtest_also_run_disabled_tests --gtest_filter='*ConnectionsPreview*'
```

The settings page preview (the page from its top to its last row, the
screen-off choice page at both ends, and the key focus on a value row, the
last row and, after the wrap, the first, on rect 480, circle 466 and
rounded_rect 480; PNG files) runs the same way:

```bash
mkdir -p /tmp/wisp-settings
WISP_SETTINGS_SCREENSHOTS=/tmp/wisp-settings \
  build/sim/tests/shell_settings_page_test \
  --gtest_also_run_disabled_tests --gtest_filter='*SettingsPreview*'
```

The key navigation preview (the confirm dialog with each button focused by a
key, on rect 480, circle 466 and rounded_rect 480; PNG files) runs the same way:

```bash
mkdir -p /tmp/wisp-keynav
WISP_KEYNAV_SCREENSHOTS=/tmp/wisp-keynav \
  build/sim/tests/system_layer_test \
  --gtest_also_run_disabled_tests --gtest_filter='*SystemLayerPreview*'
```

The pull-down panel preview (the now playing row playing, paused on a long
title, and with its play button focused by a key, on rect 480, circle 466 and
rounded_rect 480; PNG files) runs the same way:

```bash
mkdir -p /tmp/wisp-panel
WISP_PANEL_SCREENSHOTS=/tmp/wisp-panel \
  build/sim/tests/system_layer_test \
  --gtest_also_run_disabled_tests --gtest_filter='*NowPlayingPanel*'
```

For direct CTest use, configure with `-DWISP_FULL_SMOKE=ON` for every simulator
scenario or `-DWISP_FULL_SMOKE=OFF` for core scenarios. `tools/ci.sh` sets this
explicitly on each run so a previous full run cannot change the next daily run.

# Testing

Run `bash tools/ci.sh` before committing. This always checks formatting,
layering, comment rules, translations, asset budgets and generated files, builds
the simulator and runs the C++ regression suite and admin API smoke test.
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
resolves a smaller graph. Pin USB in `firmware/main/idf_component.yml` as well as
the lock file so this bootstrap step cannot upgrade usb_host_uac's dependency.

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

For direct CTest use, configure with `-DWISP_FULL_SMOKE=ON` for every simulator
scenario or `-DWISP_FULL_SMOKE=OFF` for core scenarios. `tools/ci.sh` sets this
explicitly on each run so a previous full run cannot change the next daily run.

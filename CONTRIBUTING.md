# 🤝 Contributing

Issues and PRs are welcome. Human developers and AI coding assistants follow the same process and rules. **Read [`AGENTS.md`](AGENTS.md) before you start**: the project rules, architecture constraints, coding conventions and workflow are all written there, and it is the authority.

## Process

| Size of change | What to do |
| --- | --- |
| Bug fix, small improvement | Open an issue or send a PR directly, and describe the problem and the fix clearly |
| New feature, interface change, architecture change | First write a design spec and get it agreed ([`docs/superpowers/specs/`](docs/superpowers/specs/)), then write an implementation plan ([`docs/superpowers/plans/`](docs/superpowers/plans/)), and only then start writing code |
| New development board | Follow the [porting guide](docs/porting-guide.md), and include the board's hardware document and its on-device acceptance results |

The spec takes priority over the plan and the code. If you find a problem in the spec itself, change the spec first or explain it in the PR. Do not quietly work around it.

## Code requirements

- **Layering**: the portable layers do not include platform headers and do not depend on higher layers. Board-specific constants may appear only in `boards/` and `drivers/`.
- **Language**: use C++20, compiled with `-fno-exceptions -fno-rtti`. All errors are returned with `wisp::Result<T>`.
- **Testing**: write a failing GoogleTest test first, then the implementation (TDD). Test output must be clean, with no warnings.
- **UI**: call LVGL only on the UI thread. Lay out by the safe area of the screen shape; do not hard-code margins.
- **Memory**: put large blocks in PSRAM (`os::alloc(Mem::Large)`). A new task must declare whether it accesses flash. Any change that uses internal RAM must be measured on the device with `mem`.
- **Comments**: explain only why the code is the way it is. Do not write change history, and do not cite review ids.
- **Privacy**: no secrets or personal information may appear in code, logs or test data.

## Commits and PRs

1. **Branch**: create a feature branch from `main`, for example `feat/xxx` or `fix/xxx`. Do not develop directly on `main`.
2. **Full check**: before you commit, run this locally. Everything must pass:

   ```bash
   pip install clang-format==23.1.3
   bash tools/ci.sh    # formatting, layering, comment rules, build, all unit tests and smoke tests
   ```

3. **Commit messages**: use the [Conventional Commits](https://www.conventionalcommits.org/) style, written in English, for example `feat(shell): ...` or `fix(voice): ...`. Commits created by an AI coding assistant carry a `Co-Authored-By` attribution.
4. **PR description**: cover the following:
   - what changed and why;
   - how it was tested;
   - what was verified in the simulator and what was verified on the device; say so when something has not been tried on the device yet, and do not claim it has been verified;
   - for a change to the rendering path, the `WISP_PERF` frame rate before and after;
   - for a change to internal RAM use, the measured `mem` data.
5. **Documentation**: specs, `docs/` and `AGENTS.md` that change along with the behaviour go in the same PR.

> 🤖 **For AI coding assistants**: read `AGENTS.md` first, then the specs, plans and hardware documents that this change touches. When unsure about a large question, stop and ask; do not guess. Do not push or merge to `main` unless the maintainer explicitly agrees.

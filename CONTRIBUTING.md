# 🤝 贡献指南

欢迎提交 issue 和 PR。人类开发者和 AI 编程助手都按同一套流程和规范来，**动手前请先读 [`AGENTS.md`](AGENTS.md)**：项目规则、架构约束、编码约定和工作流程都写在里面，以它为准。

## 流程

| 改动规模 | 做法 |
| --- | --- |
| 修 bug、小改进 | 开 issue 或直接提 PR，在描述里写清问题和改法 |
| 新功能、改接口、动架构 | 先写设计 spec 并讨论通过（[`docs/superpowers/specs/`](docs/superpowers/specs/)），再写实施计划（[`docs/superpowers/plans/`](docs/superpowers/plans/)），然后才开始写代码 |
| 新开发板 | 按[移植指南](docs/porting-guide.md)来，并附上该板子的硬件文档和真机验收结果 |

spec 的优先级高于计划和代码。发现 spec 本身有问题时，先改 spec 或在 PR 里说明，不要悄悄绕过去。

## 代码要求

- **分层**：可移植层不引用平台头文件，也不反向依赖上层；板子相关的常量只能写在 `boards/` 和 `drivers/` 里。
- **语言**：使用 C++20，编译选项 `-fno-exceptions -fno-rtti`，错误一律用 `wisp::Result<T>` 返回。
- **测试**：先写失败的 GoogleTest 测试，再写实现（TDD）；测试输出要干净，不能有告警。
- **界面**：只在 UI 线程调用 LVGL；布局要按屏幕形状的安全区来做，不能写死边距。
- **内存**：大块内存放进 PSRAM（`os::alloc(Mem::Large)`）；新建任务要声明是否访问 flash；占用内部 RAM 的改动要在真机上用 `mem` 实测。
- **注释**：只解释代码为什么这样写，不写修改历史，也不引用评审编号。
- **隐私**：代码、日志、测试数据里都不能出现密钥或个人信息。

## 提交与 PR

1. **分支**：从 `main` 拉一个功能分支，比如 `feat/xxx` 或 `fix/xxx`，不要直接在 `main` 上开发。
2. **完整检查**：提交前在本地跑一遍，必须全部通过：

   ```bash
   pip install clang-format==23.1.3
   bash tools/ci.sh    # 格式、分层、注释规范、构建、全部单元测试和冒烟测试
   ```

3. **提交信息**：采用 [Conventional Commits](https://www.conventionalcommits.org/) 风格，用英文写，例如 `feat(shell): ...`、`fix(voice): ...`。AI 编程助手创建的提交要附上 `Co-Authored-By` 署名。
4. **PR 描述**：写清以下几点：
   - 改了什么、为什么改；
   - 怎么测的；
   - 哪些是在模拟器上验证的，哪些是在真机上验证的；还没上真机的要写明，不要声称已经验证过；
   - 改了渲染路径的，附上改动前后的 `WISP_PERF` 帧率；
   - 改了内部 RAM 用量的，附上 `mem` 的实测数据。
5. **文档**：跟着行为一起改的 spec、`docs/` 和 `AGENTS.md`，放进同一个 PR。

> 🤖 **给 AI 编程助手**：先读 `AGENTS.md`，再读这次改动涉及的 spec、计划和硬件文档。拿不准的大问题要停下来问，不要猜。不要 push 或合并到 `main`，除非维护者明确同意。

# Wisp 子项目 3a：场景运行时核心 设计文档

- 日期：2026-11-04
- 状态：待审阅
- 范围：Wisp 整体路线中第 3 个子项目（场景运行时）的第一期
- 前置：子项目 1 基础骨架（`2026-11-01-foundation-design.md`，下文简称"基础 spec"）、子项目 2 连接与管理后台（`2026-11-03-connectivity-admin-design.md`，下文简称"连接 spec"）

---

## 1. 背景与目标

### 1.1 拆分

基础 spec 1.2 给子项目 3 列了九块内容，合在一起超过一份 spec 能讲清的范围，拆成两期，各自走"spec → 计划 → 实施"：

- **3a 场景核心（本文档）**：让一个 Lua 场景能写、能跑、能存数据。
- **3b 场景与设备、Agent 的接口**：外设占用登记与传感器引用计数、电源状态分级与唤醒锁、后台场景、场景导出工具与 headless 执行、界面树读取与模拟输入、`wisp.files`、数据迁移钩子。3b 与子项目 4 的设计衔接进行。

### 1.2 目标

3a 完成后，开发者可以手写 Lua 场景，在模拟器和真机上运行、调试、安装和导出：

1. 场景包格式与 `SceneRepository`（安装、列出、导出、删除）；
2. 每个场景一个 Lua 5.5 VM，跑在专门的场景任务上，有沙箱、执行超时和内存上限；
3. 用 Lua 表声明界面、细粒度响应式状态驱动更新，映射到 LVGL，根容器强制遵守安全区；
4. `wisp.*` API：界面、状态、定时器、数据、HTTP、设备信息、日志、场景控制；
5. `SceneStore` 场景数据（键值与集合）；
6. 前台场景模型与系统层（返回手势、错误面板）；
7. 开发闭环：模拟器目录热重载、管理后台"场景"页、命令行推送工具；
8. 一组示例场景作为验收标准，也作为将来 Agent 提示词里的范例。

### 1.3 已定决策

| 议题 | 决策 | 理由 |
|---|---|---|
| 界面声明方式 | 单个 `main.lua`，用 Lua 表声明控件树，属性绑定到状态后自动更新；不做整树 diff | 对 LLM 最友好：只有一个文件、写法接近 React/SwiftUI、"改了数据忘了刷新界面"从结构上消除；token 最少；性能与另两种写法同一量级。取代基础 spec 1.7 暂定的 `ui.json` |
| VM 模型 | 每个场景一个 `lua_State`，在场景任务上执行，与 UI 线程分开执行、优先级更低，由双核调度器分配 | 脚本再慢也不拖渲染；隔离彻底、内存上限易做准；3b 的后台场景与 headless 执行直接沿用消息模型 |
| Lua 版本 | Lua 5.5，锁定最新修复版本 | 大数组省约 60% 内存；major GC 也增量执行，停顿更短；可声明全局变量，LLM 拼错变量名在编译期报错；`luaL_openselectedlibs` 便于沙箱 |
| 全局变量 | 场景代码必须显式声明全局变量，未声明即使用在编译期报错 | 把 LLM 最常见的拼写错误提前到安装时 |
| 联网 | 允许；只能访问 `scene.json` 声明的域名，不弹窗确认 | 减少使用负担；声明即放行，运行时仍强制精确匹配、HTTPS 与限额 |
| 场景放到设备上 | 模拟器目录热重载、后台网页上传/导出、命令行推送；不做网页内代码编辑器 | 开发闭环最快；编辑器库会占掉网页包预算 |
| 设备上的入口 | 自检界面加临时"场景"列表；系统层提供左边缘右滑返回 | 不提前做系统外壳；返回手势在子项目 8 保留 |
| 示例场景 | 工具类、列表与数据类、信息展示类 | 用户选定；传感器类在 3b |

### 1.4 不在本期范围内

后台场景与唤醒锁、外设占用登记与传感器 Lua API、场景导出工具与 headless 执行、界面树读取与模拟输入、`wisp.files`、`migrate()` 数据迁移钩子、按介质存放与迁移到 SD 卡、场景数据备份、场景密钥、`.history/` 版本与回滚、网页代码编辑器、多个前台场景、场景间共享数据。

### 1.5 对基础 spec 的修订

- 基础 spec 1.7 的 `ui.json` 取消：界面与逻辑都在 `main.lua`（第 4 节）。场景包其余布局保持不变。
- 基础 spec 1.7 的 `wisp.store` 形态按本文 5.4 定稿。

---

## 2. 组件与分层

### 2.1 新增与改动的组件

| 层 | 组件 | 职责 |
|---|---|---|
| `wisp_core` | `SceneRepository` | 场景包：列出、安装（原子替换代码包）、导出、删除；解析与校验 `scene.json`。只管文件，不碰 Lua 和 LVGL |
| `wisp_core` | `SceneStore` | 场景数据：键值与集合，追加日志 + 压缩，写入合并，配额 |
| `wisp_core` | `SceneHost`（接口） | 打开、关闭、重载场景，查询运行状态。由 `wisp_scene` 实现，在 `wisp_app` 注入，`wisp_admin` 只依赖接口 |
| `wisp_net` | `SceneHttp` | 场景 HTTP 代理：按场景声明的域名放行，强制 HTTPS，大小与频率限额，过滤请求头 |
| `wisp_admin` | `/scenes` 路由 + 网页"场景"页 | 列表、上传、导出、打开/关闭/重载、删除、场景日志 |
| **新** `wisp_scene` | Lua 运行时 | 场景任务、每场景 VM、沙箱、`wisp.*` 绑定、响应式状态、控件树到 LVGL 的映射、界面改动批量提交 |
| `wisp_ui` | 系统层 | 高于场景的 LVGL 层：返回手势区、错误面板；已有的恢复出厂与固件确认框迁入该层 |
| `wisp_app` | 接线 | 自检界面"场景"区块、`SceneHost` 注入、熄屏/亮屏通知 |
| 模拟器 | `--scenes-dir` | 从目录直接加载场景并监视变化 |
| `tools/` | `wisp_scene.py` | 推送、监视推送、列出、查看日志 |
| 第三方 | `third_party/lua`（Lua 5.5.x） | 原样引入官方源码，登记许可（MIT） |

### 2.2 依赖方向

分层顺序变为 `wisp_base` → `wisp_os` → `wisp_hal` → `wisp_core` → `wisp_net` → `wisp_admin` → `wisp_ui` → `wisp_scene` → `wisp_app`。`tools/check_layering.py` 与 AGENTS.md 第 3 节同步更新。`wisp_scene` 不得 include 平台头文件；Lua 源码作为独立组件，只被 `wisp_scene` 依赖。

### 2.3 内存放置（AGENTS.md 内存规则）

- **PSRAM**：Lua VM 的堆（每个场景一个分配器，计数并限额）、场景任务的栈、`SceneStore` 的内存索引与写缓冲、HTTP 响应体、控件树描述。
- **内部 RAM**：只增加场景消息队列与少量同步对象，预计 ≤ 2 KB，真机用 `mem` 实测后写入硬件文档。
- 场景任务不碰 Flash：所有文件读写交给 `StorageManager` 的存储工作线程，场景任务只投递请求、在消息里收回调。

---

## 3. 运行时

### 3.1 线程与消息

- **UI 线程**（现有）：唯一调用 LVGL 的线程。把控件事件转成场景消息投递给场景任务；每帧开始前应用场景提交的界面改动。
- **场景任务**（新，优先级低于 UI 任务，栈在 PSRAM；与 UI 任务都不绑核，由 SMP 调度器分配到两个核上）：从一个消息队列取消息，在对应场景的 VM 里执行。消息种类：控件事件、定时器到期、HTTP 完成、存储回调、设备事件、生命周期（打开、关闭、重载、显示、隐藏）。
- 3a 只运行一个场景（前台场景），但 VM 与消息都带场景 id，按多场景设计。

### 3.2 一次交互的流程

1. 用户点击按钮，UI 线程投递 `{scene, node_id, "click"}`。
2. 场景任务执行 `on_click`；其中状态写入通知相关绑定，产生界面改动。
3. 本条消息执行结束后，改动合并成一批（同一控件同一属性只保留最后一个值），交给 UI 线程。
4. UI 线程在下一帧前应用；每帧最多应用 64 条改动，其余顺延到下一帧。

### 3.3 VM 与沙箱

- 每个场景一个 `lua_State`，用自定义分配器从 PSRAM 分配并计数，上限默认 2 MB；超出时分配失败，Lua 抛出内存错误。
- 只打开 `base`（去掉 `dofile`、`loadfile`、`load` 的二进制模式、`collectgarbage` 的 `stop`/`step` 以外选项）、`string`、`table`、`math`、`utf8`、`coroutine`。不提供 `io`、`os`、`debug`、`package`/`require`。`string.dump` 移除。
- 加载场景源码只接受文本，拒绝预编译字节码。
- 场景代码用 Lua 5.5 的全局声明机制编译：运行时在每个源文件前注入对 `ui`、`state`、`computed`、`bind`、`wisp`、`scene` 及允许的标准库名的 `global` 声明，于是文件内使用任何未声明的全局名称都是编译错误；场景自己要用全局变量时必须写 `global` 声明。注入不改变报错行号。
- `wisp.import("lib/x")` 只能加载本场景包内 `lib/` 下的 `.lua` 文件，结果按路径缓存。
- **执行超时**：每条消息的执行时间上限 200 ms，用指令计数钩子检查，超时抛出不可捕获的错误（`pcall` 拦不住）。
- **出错处理**：任何未捕获的错误都让场景进入"出错"状态：VM 销毁、定时器与未完成的 HTTP 取消、界面替换为系统错误面板（3.6）。

### 3.4 生命周期

| 时机 | 行为 |
|---|---|
| 打开 | 创建 VM → 执行 `main.lua` → 它返回的树建成控件 → `scene.on_show()` |
| 熄屏 | `scene.on_hide()`；暂停定时器与界面更新 |
| 亮屏 | 定时器恢复（错过的 `every` 只补触发一次）、`minute` 事件立即补发一次 → `scene.on_show()` |
| 关闭 / 返回 | `scene.on_hide()` → `scene.on_close()` → `SceneStore` 刷盘 → 销毁 VM 与控件 |
| 重载 | 关闭后重新打开；数据保留 |

生命周期钩子各自受 200 ms 上限约束；`on_close` 出错或超时也照常完成关闭。

### 3.5 前台与系统层

- 只有前台场景渲染并接收触摸。系统层位于场景之上，承载返回手势区、错误面板，以及已有的恢复出厂与固件确认框。
- **返回手势**：从屏幕左边缘约 16 px 的判定区向右滑动超过 1/4 屏宽，关闭当前场景回到自检界面。手势由系统层先截获，场景收不到。判定区按 `hal::safe_rect()`/`row_span()` 计算，圆角屏上手指可达。
- 场景打开期间，自检界面不销毁，只隐藏，返回即显示，不重建。

### 3.6 错误面板

系统层面板显示场景名、文件、行号与错误信息，按钮"重载"和"返回"；同时写一条场景日志。出错的场景不会被自动停用：系统不会自行重试，是否重载由用户决定；列表中显示其最近一次错误。

---

## 4. 界面声明与响应式状态

### 4.1 入口

`main.lua` 的返回值是根节点，必须是 `ui.screen`。示例：

```lua
local count = state(0)

return ui.screen {
  ui.column { pad = "safe", gap = 12, align = "center",
    ui.label { text = bind(count, function(v) return "已点击 " .. v .. " 次" end), font = "xl" },
    ui.button { text = "+1", on_click = function() count:set(count:get() + 1) end },
  },
}
```

### 4.2 响应式状态

| 写法 | 作用 |
|---|---|
| `state(v)` | 创建状态；`:get()` 读、`:set(v)` 写；写入与原值相等（基本类型按值，表按引用）时不触发更新 |
| `computed(fn)` | 派生状态；执行 `fn` 时读到的状态自动成为依赖 |
| 属性值为状态 | 如 `text = count`，状态变化时属性随之更新 |
| `bind(s, fn)` | 属性值为 `fn(s:get())` 的结果，随 `s` 更新 |
| `ui.show_if(s, node)` | `s` 为真时显示 `node`，否则隐藏（不销毁） |

- 只有绑定的属性会更新，不做整树比对。
- 同一条消息里多次 `set` 只在消息结束时合并成一次界面改动。
- 状态之间的循环依赖在 `computed` 首次求值时检测并报错。

### 4.3 控件

| 类别 | 控件 |
|---|---|
| 布局 | `screen`、`column`、`row`、`stack`（叠放）、`grid`、`spacer`、`scroll` |
| 文本与图像 | `label`、`icon`（内置图标集）、`image`（只能引用本包 `assets/`） |
| 输入 | `button`、`checkbox`、`switch`、`slider`、`input`（聚焦时弹出系统屏幕键盘，支持中英文） |
| 进度与数据 | `bar`、`arc`（进度环）、`chart`（折线、柱状） |
| 列表 | `list`：`items` 为状态或数组，`row = function(item, index)` 只为可见行调用；`items` 变化时只重建可见范围 |
| 绘制 | `canvas`：`draw = function(g) ... end`，`g` 提供 `line`、`circle`、`arc`、`rect`、`fill`、`text`；依赖的状态变化时重绘 |

### 4.4 通用属性

- 尺寸与布局：`w`/`h`（像素、`"fill"`、`"content"`）、`grow`、`pad`、`gap`、`align`、`justify`。
- 外观：`bg`、`fg`、`radius`、`border`、`font`（`xs`/`sm`/`md`/`lg`/`xl`/`xxl`）、`hidden`、`opa`。
- 事件：`on_click`、`on_long_press`、`on_change`（输入类）。
- 颜色写 `#RRGGBB` 或主题令牌：`accent`、`text`、`muted`、`surface`、`bg`、`danger`、`ok`、`warn`。子项目 7 的主题系统在这组令牌上扩展。
- `pad = "safe"`：按安全区留边距。根节点 `ui.screen` 总是铺满屏幕并强制套上安全区内边距（AGENTS.md 第 3 节"屏幕形状"），场景无法关闭。
- `ref = "名字"` 给节点命名，`ui.ref("名字")` 取得句柄，用于少量命令式操作（如读取输入框的值、滚动到某项）。

### 4.5 校验

控件名与属性按编译进固件的白名单（schema）校验，在建树时检查。未知控件、未知属性、类型不符都报错，报错包含 `main.lua` 行号、控件名、属性名，并给出编辑距离最近的合法名称。白名单同时导出为文档，供示例与子项目 4 的提示词使用。单个场景的控件数上限 2000。

---

## 5. `wisp.*` API

### 5.1 定时器

```lua
local h = wisp.timer.after(ms, fn)
local h = wisp.timer.every(ms, fn)
h:cancel()
```

最小间隔 50 ms，每个场景最多 32 个；超出返回错误。

### 5.2 设备

```lua
wisp.device.info()  -- {width, height, shape, safe_area, lang, time, battery, charging, wifi}
wisp.device.on("battery" | "minute" | "lang", fn)
```

只读。`minute` 在整分钟触发，供时钟使用。

### 5.3 HTTP

```lua
wisp.http.get(url, opts, cb)
wisp.http.post(url, body, opts, cb)  -- cb(err, {status, headers, body, json})
```

- 只能访问 `scene.json` 中 `permissions.http` 声明的主机（精确匹配，不支持通配符），只允许 `https`。
- 声明即放行，不弹窗确认。声明的域名在网页场景详情、安装与推送的输出中显示；更新场景时域名列表变化会写一条日志。
- 请求头只允许普通头与 `Authorization`；`Cookie` 等其他认证类头一律拒绝。
- 响应体上限 256 KB，每个场景每分钟最多 30 次请求，超时 15 秒。
- `json` 字段在响应为 JSON 时提供解析结果。
- 拒绝时 `err` 为 `permission_denied`、`not_https`、`too_large`、`rate_limited` 之一。
- **留给后续子项目**：Agent 修改场景时新增域名，要在对话中明确告知用户（子项目 4、7）。场景密钥（只写不读）在 3b 或子项目 7 设计。

### 5.4 数据（`SceneStore` 的 Lua 绑定）

```lua
wisp.store.get(k) / wisp.store.set(k, v)            -- 键值；v 为可转 JSON 的值，nil 表示删除
wisp.store.batch(function() ... end)                -- 其中的写入原子提交
local c = wisp.store.collection("todos")
c:insert(t) -> id ; c:update(id, t) ; c:delete(id)
c:find(filter?, opts?)                              -- filter 按字段相等；opts = {sort, desc, limit, offset}
```

读写在内存索引上同步完成，持久化异步进行（6.1）。

### 5.5 日志与场景

```lua
wisp.log.info(...) / warn(...) / error(...)   -- 进入系统日志，标签 scene.<id>
wisp.scene.close()
wisp.scene.args()                              -- 打开时传入的参数表
```

### 5.6 限额汇总

| 项目 | 默认上限 |
|---|---|
| VM 内存 | 2 MB |
| 每条消息执行时间 | 200 ms |
| 控件数 | 2000 |
| 场景数据 | 1 MB |
| 单条记录 | 16 KB |
| 集合数 | 16 |
| HTTP 响应 | 256 KB |
| HTTP 频率 | 30 次/分钟 |
| 定时器 | 最小间隔 50 ms，最多 32 个 |
| 单个字符串属性值 | 64 KB（超出报 `value too large`；实施时裁定：每次改动都复制字符串，防止一条大字符串撑爆 PSRAM） |
| 一条消息的界面改动 | 1 MB（字符串字节 + 每个操作/节点/属性按 64 字节估算；超出报 `ui update too large` 并整批丢弃；实施时裁定：同一字符串绑到多个控件会被复制多份，不设上限时可耗尽 PSRAM 并 abort） |

超出上限返回明确错误，执行时间与内存超限按 3.3 进入出错状态。

---

## 6. 存储与场景包

### 6.1 `SceneStore`

- 位置：`<场景目录>/data/`；每个集合一个 `<名>.log`，键值在 `kv.log`。每行一条 JSON：`{"op":"put"|"del","id":…,"v":{…}}`。
- 打开场景时读入内存索引；写操作先改索引再追加日志，300 ms 内的写入合并成一次刷盘；关闭场景与熄屏前强制刷盘。
- 废弃记录超过有效数据一半或日志超过 64 KB 时，用 `StorageManager::write_atomic` 整体重写。
- 读取时丢弃末尾不完整的行并记警告；掉电最多丢失最后 300 ms 的写入。
- 集合 id 为单调递增整数，场景内唯一。
- 配额按场景统计全部日志文件的大小。

### 6.2 场景包

```
scenes/<id>/
  scene.json
  main.lua
  lib/*.lua
  assets/
  data/        （SceneStore 管理，代码包替换时不动）
```

`scene.json`：

```json
{"id": "todo", "name": "待办", "version": "1.0.0", "schema_version": 1,
 "entry": "main.lua", "icon": "assets/icon.png",
 "permissions": {"http": ["api.example.com"]}}
```

- `id`：`[a-z0-9_-]`，1–32 字节；`name` 1–32 字节 UTF-8；`version` 为语义化版本；`entry` 默认 `main.lua`。
- 3a 的场景放在内置存储的 `/int/scenes/`；代码包总大小上限 512 KB。

### 6.3 安装

1. 上传的 zip 解到临时目录，校验 `scene.json`，编译（不执行）全部 `.lua` 文件；任一失败则整体拒绝，返回文件与行号。
2. 同 id 已存在时，只替换代码包（`scene.json`、`.lua`、`lib/`、`assets/`），保留 `data/`；用两阶段改名保证替换原子（AGENTS.md 第 8 节）。
3. 若该场景正在运行，安装完成后自动重载。

### 6.4 模拟器

`--scenes-dir <目录>`：直接从该目录加载场景，不需要安装；监视文件变化，保存后自动重载。数据放在 `<data-dir>/scenes/<id>/data/`，重载保留。报错同时显示在错误面板和控制台。

---

## 7. 管理后台与工具

### 7.1 REST 接口（鉴权与过滤沿用连接 spec 6.3）

| 方法与路径 | 说明 |
|---|---|
| `GET /scenes` | 列表：`id`、`name`、`version`、代码大小、数据占用、`state`（`stopped`/`running`/`error`）、声明的域名 |
| `POST /scenes` | 上传 zip（流式请求体，复用固件上传的流式处理），按 6.3 安装；201 返回场景信息，400 返回校验错误 |
| `GET /scenes/{id}/export?data=0\|1` | 导出 zip；`data=1` 包含 `data/` |
| `POST /scenes/{id}/open` / `close` / `reload` | 经 `SceneHost` 操作 |
| `DELETE /scenes/{id}?data=0\|1` | 202，需在设备上确认；确认后删除代码包，`data=1` 时连数据一起删 |

新增事件 `scene.state {id, state}`，同步写入 `docs/architecture.md`。场景日志沿用现有日志流，按标签 `scene.<id>` 过滤。

### 7.2 网页"场景"页

列表（名称、版本、状态、大小、数据占用）；上传；每个场景的打开、关闭、重载、导出、删除；详情里显示声明的域名与最近的错误；日志按场景过滤。中/日/英文案。

### 7.3 `tools/wisp_scene.py`

- `push <目录> [--open] [--watch]`：打包上传；`--watch` 监视文件，保存后推送并重载。
- `ls`、`logs <id>`。
- 走已配对的管理接口：首次运行时按屏幕配对码配对，令牌存在用户配置目录，文件权限 0600；不在日志或输出中打印令牌。
- 只用 Python 标准库。

---

## 8. 设备端界面

- 自检界面新增"场景"区块：列出已安装场景（名称、状态），点击打开；最近一次运行出错的场景带错误标记。子项目 8 用正式外壳替换这个区块。
- 屏幕键盘为系统层控件，`input` 聚焦时弹出，支持中英文切换；遵守安全区。
- 界面文案中/日/英。

---

## 9. 测试与验收

### 9.1 主机测试（GoogleTest，TDD）

| 对象 | 内容 |
|---|---|
| `SceneRepository` | `scene.json` 校验与 id 规则；原子替换保留 `data/`；语法错误的包被拒绝并报行号；导出、删除；代码包大小上限 |
| `SceneStore` | 增删改查与 `find`；写入合并；压缩；截断行恢复；配额、单条大小、集合数；关闭时刷盘 |
| 沙箱 | `io`/`os`/`debug`/`require`/字节码/`string.dump` 不可用；未声明全局变量编译报错；`wisp.import` 不能越出 `lib/` |
| 执行限制 | 200 ms 超时（含 `pcall` 内的死循环）；2 MB 内存上限；出错后 VM 销毁并可重新打开 |
| 响应式 | `state`/`computed`/`bind` 依赖追踪；相等写入不触发；一条消息内多次 `set` 合并；循环依赖报错 |
| 控件映射 | 白名单报错（行号与建议）；根节点强制安全区；`list` 只为可见行建控件；每帧改动上限 |
| `SceneHttp` | 声明域名精确匹配；非 HTTPS、超大、超频、禁用请求头被拒 |
| 定时器与生命周期 | `after`/`every`/`cancel`；最小间隔与数量上限；熄屏暂停、亮屏补触发一次；`minute` 补发 |
| 管理接口 | `/scenes` 鉴权与 CSRF；流式上传；同 id 覆盖保留数据；删除需设备确认 |

### 9.2 模拟器冒烟测试

扩展 `sim_smoke`：用 `--scenes-dir` 加载计数器场景并模拟点击，检查文字；修改文件触发热重载、数据保留；加载出错场景，检查错误面板与返回手势；用 `wisp_scene.py push` 推送并打开。

### 9.3 示例场景（`scenes/examples/`）

| 场景 | 覆盖 |
|---|---|
| 计数器 | 最小完整示例 |
| 番茄钟 | `arc`、`timer`、熄屏恢复 |
| 待办 | `list`、`input`、屏幕键盘、集合持久化 |
| 记账 | 集合、`find` 排序、`chart` |
| 时钟表盘 | `canvas`、`minute`、圆角屏安全区 |
| 系统状态 | `wisp.device` |
| 天气（可选） | `wisp.http`、域名声明（无需 Key 的公开接口） |

### 9.4 真机验收清单（由用户执行）

1. 网页上传与 `wisp_scene.py push` 两种方式安装示例场景，从自检界面的场景列表打开。
2. 各示例场景功能正常；断电重启后待办与记账数据仍在。
3. 圆角屏上返回手势可用；推送一个出错的场景，错误面板与返回正常，点"重载"可再次打开。
4. 待办放 200 条时滚动，`WISP_PERF` 帧率不低于自检界面基准；场景运行时 `mem` 显示内部 RAM 仍满足 AGENTS.md 预算。
5. 熄屏与亮屏后番茄钟、时钟显示正确；待机耗电与无场景时相当。
6. 天气场景临时改为访问未声明的域名时被拒绝。

### 9.5 实施顺序（供计划参考）

Lua 组件与沙箱 → `SceneStore` → `SceneRepository` → 响应式核心 → 控件映射与批量提交 → 场景任务与生命周期 → 系统层（返回、错误面板、键盘）→ `wisp.*` 其余 API → `SceneHttp` → 管理接口与网页 → 模拟器热重载与命令行工具 → 示例场景 → 文档与真机验收。

---

## 10. 风险与对策

| 风险 | 对策 |
|---|---|
| 场景任务与 UI 线程之间的改动队列堆积，造成延迟或内存增长 | 同一属性合并；每帧应用上限；队列长度上限，超出时合并为"整体刷新绑定" |
| Lua GC 在 PSRAM 上的停顿 | 5.5 增量 major GC；场景任务优先级低于 UI；真机测量 GC 停顿并记录 |
| LLM 写出的场景使用白名单外的写法 | 精确报错 + 最近名称建议；白名单导出为文档进入提示词 |
| 场景经 HTTP 外发数据 | 只放行声明的域名；声明可见；Agent 新增域名时告知用户（子项目 4、7） |
| 中文屏幕键盘占用字体与内存 | 复用现有字体子集；候选与词库放 PSRAM；真机实测内存 |
| 渲染帧率受场景界面复杂度影响 | 控件数上限；`list` 虚拟化；真机验收第 4 条对比基准 |

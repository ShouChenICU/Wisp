# Wisp 子项目 3b-1：场景的设备侧接口 设计文档

- 日期：2026-11-05
- 状态：已批准（2026-11-05），含计划审阅后的修订：错误码统一为 3a 的下划线风格，SD 路径、下滑判定区、`/close` 与 `/exit`、后台场景事件
- 范围：子项目 3（场景运行时）第二期的设备侧部分
- 前置：
  - 基础 spec：`2026-11-01-foundation-design.md`
  - 连接 spec：`2026-11-03-connectivity-admin-design.md`
  - 3a spec：`2026-11-04-scene-runtime-core-design.md`
  - 矢量字体 spec：`2026-11-05-vector-font-design.md`。本期界面上的大字用 `ui::font(px)`。

---

## 1. 背景与目标

### 1.1 拆分

3a spec 1.1 把第二期（3b）定为七块内容。2026-11-05 用户决定按设备侧和 Agent 侧再拆成两份 spec：

- **3b-1 设备侧（本文档）**，包括：
  - 后台场景；
  - 电源分级与唤醒锁；
  - 通知、状态胶囊与下拉面板；
  - 传感器与外设占用登记；
  - `wisp.files`；
  - 数据迁移钩子 `migrate()`。
- **3b-2 Agent 侧**：界面树读取与模拟输入、headless 执行、场景导出工具。3b-2 与子项目 4（Agent 核心）一起设计，接口按 Agent 的实际需要来定。

### 1.2 目标

做完以后，场景可以：

1. 转入后台继续运行，计时、提醒、定时联网都不中断，灭屏待机时也一样；
2. 发通知：横幅、未读列表、提示音，必要时在待机中亮屏；
3. 在前台读取 IMU；
4. 在私有目录里存大文件，可以直接把下载内容写成文件；
5. 在升级时迁移自己的数据。

设备侧同时具备：

- 五级电源管理；
- 唤醒锁登记，可查看是谁在耗电；
- 状态胶囊和下拉面板，用户可以查看、切换、关闭后台场景，处理通知。

### 1.3 已定决策（2026-11-05 与用户确认）

| 议题 | 决策 |
|---|---|
| 后台场景的用途 | 计时与提醒、定时联网拉数据、通知。后台场景不使用传感器 |
| 待机时的后台场景 | 继续运行，按需保持资源。没有需要网络的唤醒锁时进入深度待机（关 Wi-Fi，CPU 自动浅睡），定时器照常触发 |
| 进入后台 | `scene.json` 声明 `"background": true` 的场景，返回时默认转入后台；没有声明的场景照旧关闭（3a spec 1.4） |
| 后台数量与内存 | 最多 3 个后台场景。所有场景的 VM 共享 3 MB 总预算，前台保证能拿到 2 MB。超过数量或预算时关闭最久未使用的后台场景（数据已保存），并发通知告知 |
| 重启 | 关机或重启前在后台运行的场景，开机后自动在后台重新启动 |
| 通知形式 | 横幅加未读列表；可选提示音；待机时可以亮屏 |
| 状态区 | 顶部中央一个小胶囊，有后台场景或未读通知时显示。点胶囊或从顶边下滑（只旁观、不拦截）打开下拉面板 |
| 传感器 | 只给前台用，引用计数共享 IMU。外设占用登记只搭框架，麦克风和扬声器的仲裁留给子项目 5 |
| `wisp.files` | 每个场景一个私有目录。有 SD 卡时放卡上，每场景配额 64 MB；没有卡时放内部存储，每场景配额 512 KB |

### 1.4 不在本期范围内

以下内容不在本期：

- 3b-2 的全部内容；
- 后台场景使用传感器、抬腕亮屏、计步；
- 麦克风和扬声器的仲裁与混音（子项目 5）；
- 场景密钥；
- `.history/` 版本与回滚；
- 场景数据备份；
- 场景文件在 SD 卡与内部存储之间迁移；
- 场景间共享数据；
- 多个前台场景；
- 系统外壳与鬼火角色（子项目 8）。

### 1.5 对已有 spec 的修订

- **3a spec 3.4 生命周期**：增加"转入后台 / 切回前台"两种转换（3.2）。声明了 `background` 的场景，灭屏时定时器不暂停（3.3）。
- **3a spec 3.5**：返回手势对声明了 `background` 的场景改为"转入后台"；系统层新增状态胶囊、下拉面板和横幅（第 5 节）。
- **3a spec 6.2 / `SceneManifest`**：
  - 新增 `background` 字段；
  - 新增 `files` 字段，声明文件存储位置（7.2）；
  - 取消 `schema_version` 必须为 1 的限制（7.3）；
  - 场景目录增加 `files/`（7.2）。
- **基础 spec 1.6**：电源级别和唤醒锁按本文第 4 节定稿。
- **基础 spec 1.5**：传感器引用计数和占用登记按本文第 6 节定稿。

---

## 2. 组件与分层

### 2.1 新增与改动

| 组件 | 内容 |
|---|---|
| `wisp_hal` | `hal::Device` 基类增加 `set_power(PowerLevel)`，默认返回 `Unsupported`；caps 增加 `standby` 策略（`off` / `throttle` / `wake_source`）。Display、Touch、Imu 原有的 `set_power(bool)` 改由基类方法实现 |
| `wisp_core` | `WakeLockRegistry`、`NotificationService`（含持久化）、`SensorService`、`OccupancyRegistry`、`SceneFiles`（配额、路径校验、位置记录） |
| `wisp_app` | `PowerManager` 扩展为五级状态机，遍历设备登记表；开机恢复后台场景 |
| `wisp_ui` | `SystemLayer` 增加状态胶囊、下拉面板、横幅和顶边下滑手势 |
| `wisp_scene` | 多场景调度（一个前台加最多 3 个后台）、VM 总预算与淘汰，以及新的 Lua API：`wisp.notify`、`wisp.sensor`、`wisp.files`、`wisp.power`、`wisp.http.download`、`wisp.scene.state`/`launch`/`exit`，还有 `migrate` |
| `wisp_admin` | REST 接口和网页：唤醒锁、占用登记、后台场景、场景文件 |
| `wisp_port_esp` | 自动浅睡（`esp_pm`）的开关、Wi-Fi 启停的钩子、提示音播放 |
| `wisp_port_sim` | 假 IMU（可以注入动作）；模拟浅睡和 Wi-Fi 启停（只记录状态） |

### 2.2 原则

- Lua 绑定、REST 接口和以后的 Agent 工具都是这些服务的薄适配层（AGENTS.md §3 服务层约定）。
- 可移植层不出现 `esp_pm` 等平台头文件，浅睡和 Wi-Fi 启停都经端口接口。
- 新任务都要如实声明 `flash_access`：
  - IMU 读取任务不访问 flash，栈放 PSRAM；
  - 通知持久化走已有的 `wisp.fs` / `StorageManager`，不新增任务。

---

## 3. 后台场景

### 3.1 调度

- 所有场景继续共用一个场景任务 `wisp.scene`，消息按场景 id 分发。3a 的 VM 和消息已经带场景 id（3a spec 3.1）。
- 前台最多一个，后台最多 3 个。
- 每条消息仍有 200 ms 的执行上限，所以后台场景拖不住前台。

### 3.2 状态与转换

状态有四种：

- `foreground` 前台运行；
- `background` 后台运行；
- `stopped` 已停止；
- `error` 出错。

| 触发 | 声明 `background` 的场景 | 没有声明的场景 |
|---|---|---|
| 返回手势 | 转入后台 | 关闭 |
| 从列表打开另一个场景 | 当前场景转入后台 | 当前场景关闭 |
| 打开一个已经在后台的场景 | 切回前台，不重新加载 | — |
| 面板里的 ×、`wisp.scene.exit()`、管理后台关闭 | 彻底关闭：`on_hide` → `on_close` → 刷盘 → 销毁 VM | 同左 |
| 后台运行时出错 | 进入 `error`，销毁 VM，发系统通知"<名称>出错已停止" | — |

- **转入后台**：`on_hide`，然后销毁控件（LVGL 对象），保留 VM、状态和界面声明树。
  - 后台期间状态照常可以改。界面改动不发给 UI 线程，只更新声明树。
- **切回前台**：从声明树重建控件，然后 `on_show`。重建的耗时要计入 9.3 的真机测量。
- 每次转换都触发场景的 `state` 事件，场景用 `wisp.scene.state()` 查询当前状态。
- `scene.state` 总线事件增加 `background` 取值。
- 首次打开（含开机恢复）时也触发一次 `state` 事件，告知初始状态。
- 后台场景照常收到系统事件：`minute`、电量、语言变化等。
- `wisp.scene.close()`（3a）语义不变，是"返回"；`wisp.scene.exit()` 是新增的，总是彻底关闭。

### 3.3 定时器与灭屏

- **声明了 `background` 的场景**：灭屏、待机、深度待机时，以及在后台时，定时器都照常触发，不暂停也不补发。
  - 亮屏时 `minute` 事件立即补发一次（同 3a）。
  - 场景在前台时，`on_hide` / `on_show` 照常随灭屏和亮屏触发。
- **没有声明的场景**：保持 3a 的行为。

### 3.4 后台能用与不能用的 API

| 能用 | 不能用（调用时返回 `not_in_background`） |
|---|---|
| 定时器、HTTP（待机时按 4.4 排队）、`wisp.store`、`wisp.files`、`wisp.notify`、日志、`wisp.device.info` | `wisp.sensor`、`wisp.power.keep_screen_on`、键盘与确认框 |

界面状态可以改，但不会渲染。

### 3.5 内存预算与淘汰

- 所有场景的 VM 共享 3 MB 总预算，常量写在 `scene::kVmBudgetBytes`。
- 单个 VM 仍然以 2 MB 为上限（3a）。
- 前台场景打开或分配内存时，如果总量会超出预算，按最久未在前台的顺序关闭后台场景，直到前台能拿到 2 MB 为止。被关闭的场景正常执行 `on_close` 并刷盘，然后发一条系统通知："<名称>已因内存不足关闭"。
- 第 4 个场景要转入后台时，同样关闭最久未在前台的那个。
- 淘汰不发生在一次 Lua 分配的中途。分配器发现总量超出预算时，让当前分配失败（同 3a 的内存超限）。淘汰在打开场景、转入后台和每条前台消息之前检查。

### 3.6 开机恢复

- 场景进入或离开后台时，后台场景的 id 列表写入 System 区的 `scenes/background.json`（`write_atomic`）。
- 开机完成后（`system.boot.completed`），按列表逐个在后台启动这些场景。启动流程是：建 VM → 执行 `main.lua` → 建声明树，不建控件，也不调用 `on_show`。
- 启动原因用 `wisp.scene.launch()` 查询：`"user"`、`"boot"` 或 `"notification"`。
- 某个场景启动失败时跳过它，记日志并发系统通知。

### 3.7 管理后台

- 场景页显示每个场景的状态，包括 `background`，后台场景可以关闭。
- REST 保留 3a 的 `POST /scenes/{id}/close`，语义是"返回"：声明了 `background` 的场景转入后台，其余关闭。新增 `POST /scenes/{id}/exit`，总是彻底关闭。鉴权和错误格式沿用连接 spec 6.3。

---

## 4. 电源分级与唤醒锁

### 4.1 级别

| 级别 | 进入条件 | 设备状态 |
|---|---|---|
| `active` | 有触摸或按键 | 正常 |
| `dim` | 距离灭屏还剩 10 秒 | 亮度降到 30%，有触摸就回到 `active` |
| `standby` | 无操作满 `power/screen_off_s`（已有设置，默认 60 秒），或短按电源键 | caps 声明 `off` 的设备关闭（屏幕、触摸、IMU）；CPU 和 Wi-Fi 保持 |
| `deep_standby` | 处于 `standby` 满 `power/deep_after_s`（新增设置，默认 300 秒，0 表示不进入），且没有 `network` 锁 | 关闭 Wi-Fi；在没有 `cpu` 锁时启用自动浅睡 |
| 关机 | 长按电源键并确认（已有） | AXP2101 断电 |

- 从 `standby` 或 `deep_standby` 唤醒的方式：
  - 电源键；
  - 带 `wake = true` 的通知；
  - 场景的定时器（只唤醒 CPU，不亮屏）。
- 从 `deep_standby` 亮屏时，立即重新打开 Wi-Fi。
- 新增设置 `power/light_sleep`（布尔，默认开），用来在调试时关掉浅睡，因为浅睡时 USB 串口可能断开。
- 设置键遵守 15 字符的限制。

### 4.2 设备如何响应级别

- `PowerManager` 遍历 `DeviceRegistry`，对每个设备调用 `set_power(level)`，由设备按自己的 caps `standby` 策略处理：
  - `off`：`standby` 及以下关闭；
  - `throttle`：降频保持；
  - `wake_source`：保持，作为唤醒源（电源键）。
- 返回 `Unsupported` 的设备忽略。
- 代码里不再按名字逐个处理设备。
- `StorageManager`、`PowerMonitor`、`ButtonService` 现有的待机调用保留，它们是服务，不是设备。

### 4.3 唤醒锁

`WakeLockRegistry` 中每把锁记录：

- 持有者：`system.<模块>` 或 `scene.<id>`；
- 原因：文字，界面上显示；
- 类型：`network`、`cpu` 或 `screen`；
- 开始时间。

| 类型 | 作用 | 持有者 |
|---|---|---|
| `network` | 不进入 `deep_standby` 的关 Wi-Fi 步骤 | 系统：上传、OTA、下载、校时进行中 |
| `cpu` | 禁止自动浅睡 | 系统：OTA 写入；以后的音频播放（子项目 5） |
| `screen` | 保持 `active`，不变暗、不灭屏 | 前台场景，通过 `wisp.power.keep_screen_on(true/false)` 申请 |

- 场景的 `screen` 锁在场景转入后台或关闭时自动释放。
- 打开着的管理后台页面不持有任何锁（2026-11-08 修订，用户要求）：WebSocket 会话和普通请求都不阻止休眠。熄屏后照常进入 `deep_standby` 关 Wi-Fi，页面断线，设备唤醒后自动重连。这样忘了关的浏览器标签页不会把电量耗尽。只有实际的传输（上传、OTA、下载）持锁，传完即放。
- 后台场景不能持有任何锁。
- `network`/`cpu` 锁连续持有超过 30 分钟，发一条系统通知"<持有者>已让设备保持唤醒 30 分钟"，附"释放"操作，之后每 30 分钟提醒一次。
  - 对场景锁，"释放"就是释放这把锁；
  - 对系统锁，只提示，不提供"释放"。
  - `screen` 锁不提醒：场景的 `screen` 锁只在前台存在，用户看得见；短按电源键随时可以灭屏，不受锁影响。
- 锁的增减都发 `power.wakelock` 事件：`{"op":"acquire"|"release","holder":..,"kind":..,"reason":..}`。
- `power.state` 的取值扩展为 `active`、`dim`、`standby`、`deep_standby`。

### 4.4 深度待机时的联网

- 在 `deep_standby` 中，场景的 HTTP 请求和下载先进入队列。系统最多每 15 分钟打开一次 Wi-Fi，处理整个队列，顺带一次校时，然后关闭。
- 队列每个场景最多 4 条。超出的请求立即失败，返回 `deferred_queue_full`。
- 一条请求排队超过 1 小时就失败，返回 `deferred_timeout`。
- `standby` 中 Wi-Fi 还开着，请求照常立即发出。
- 场景回调里能看到排队这件事，`res.deferred = true`。

### 4.5 可见性

- REST `GET /system/power` 返回当前级别和锁列表（持有者、类型、原因、持续时长）。
- 管理后台的系统页显示这张表。
- 以后子项目 4 的 Agent 工具直接读这个服务。

---

## 5. 通知、状态胶囊与下拉面板

### 5.1 场景 API

```lua
local id = wisp.notify{ title = "番茄钟", body = "25 分钟到了",
                        sound = "alarm", wake = true, id = "pomo" }
wisp.notify_cancel(id)
```

- 参数：
  - `title`：1–32 字节；
  - `body`：0–200 字节；
  - `sound`：`"none"`（默认）、`"tone"`（一声短音）或 `"alarm"`（反复响铃，直到用户触摸，最长 30 秒）；
  - `wake`：默认 false，待机中是否亮屏；
  - `id`：可选，1–32 字节 `[a-z0-9_-]`。用同一个 id 再发一次会替换旧的那条。
- 返回值是通知 id。
- 用户点击通知时，系统把场景切回前台，场景不在运行时先启动它（`launch()` 为 `"notification"`），然后触发场景的 `notification` 事件，参数为通知 id。
- 限额：
  - 每个场景最多保留 5 条未读，超出时丢掉最旧的；
  - 每分钟最多 10 条，超出返回 `rate_limited`。
- 前台场景自己发的通知只弹横幅，不进入未读列表。

### 5.2 系统通知

系统在以下情况发通知，来源记为 `system`：

- 后台场景因为内存或数量被关闭；
- 后台场景出错停止；
- 开机恢复失败；
- 唤醒锁持有过久；
- 电量低：接 `power.battery.low` 事件。

### 5.3 横幅

- 从顶部滑入，按 `hal::safe_rect()` 和 `row_span()` 布局，宽度跟随横幅所在行的可见宽度。
- 内容：场景图标、场景名、标题、最多两行正文。
- 4 秒后自动收起；点击打开场景；上滑关闭。
- 多条通知排队依次显示。
- 层级在场景之上、确认框之下。
- 横幅在自检界面上也会显示。

### 5.4 提示音

- 端口提供 `play_tone(kind)`。内置两段 PCM，tone 约 150 ms，alarm 约 1 秒一段循环播放，经 `audio_out.main` 播放。
- 音量取系统设置 `audio/notify_vol`（0–100，默认 60）。
- 没有扬声器或初始化失败时静默降级。
- 混音和优先级留给子项目 5。

### 5.5 状态胶囊

- 位于顶部中央、安全区以内，72×24 px（16 px 字体下最宽的文字 `▶9+ 🔔9+` 实测需要 72 px）。
- 大于 9 的数显示为 `9+`。
- 显示后台场景数和未读通知数，两者都为 0 时隐藏。
- `wisp.device.info()` 增加 `status_area = {x1, y1, x2, y2}`，供需要的场景避开它。胶囊隐藏时这个区域照样返回，这样场景的布局不会跳动。

### 5.6 下拉面板

打开方式：

- 点胶囊；
- 从顶边下滑：判定区是按下点所在列的最上面一个可见像素往下屏幕短边的 8% 以内（480 px 屏上 38 px；圆形屏的中间列从 y=0 起，手指可以从物理顶边开始；手指从边框外滑进来时触摸芯片报的是接触面中心，2.16 寸参考板上第一个点实测落在 y=9–26，原来固定的 16 px 有一半的下拉落到判定区外、进了对话记录），向下移动屏幕短边的 5% 触发（480 px 屏上 24 px），不限角度。这些距离都按屏幕短边的百分比算（`hal::share_of_screen()`，四舍五入到像素），不同分辨率的屏手感一致；面板上的上划收起和横幅上划收起用同一个触发距离。实现方式和返回手势相同，只旁观不拦截，接管时下面的控件收到 `PRESS_LOST`。键盘、确认框打开时不触发；面板已经在（打开、跟手或正在滑动）时也不触发。

跟手下拉（仿 Android 状态栏下拉）：

- 面板（全屏底板加安全区内容框，作为一整张"幕布"一起移动）从上往下盖在当前画面上，下面的内容原地不动，不会被推开。
- 幕布底部有一根小横条（40×4 px 胶囊，`kTextFaint` 按 60% 混到 `kBg` 上的不透明色，居中，下沿离幕布底边 12 px），标出幕布在哪里结束，跟手时随幕布一起移动。圆形和圆角屏的中间列都能显示到这个位置。内容框是安全区，但底部如果会碰到横条（横条上方留 8 px），就截短到横条上方。横条不可点击，点它等于点空白处。它也是遮挡标记，与另外两块一起暂停和恢复。
- 顶边下滑一过触发距离就调用 `on_open`、建好面板并开始跟手：幕布底边位于手指处，偏移 = clamp(手指 y − 按下 y, 0, H)，幕布位于 y = 偏移 − H（H 为屏高），之后每次触摸读数都跟着移动，直到松手。
- 松手判定与 shell 的子页面拖动相同（`wisp/ui/drag.hpp`，两者共用常量）：拉过 H 的 1/3，或者向下快速一甩（最近 50 ms 内速度不低于 400 px/s），就以 ease-out 动画滑到完全打开，时长按剩余距离比例算，整屏 220 ms；否则滑回顶部并销毁，视为没有打开过（不会触发任何行的回调，但 `on_close` 照常与 `on_open` 配对调用）。
- `on_open` 目前只取模型，没有副作用，所以在触发时调用；面板滑回去时不需要撤销什么。
- 点胶囊或代码调用 `open_panel()` 直接打开，没有动画。

面板内容，从上到下：

1. 时间、电量、Wi-Fi 状态；
2. 后台场景列表，每行显示图标、名称、进入后台的时长。点击切回前台，× 彻底关闭；
3. 通知列表，最新的在上面。点击打开对应场景，左滑或 × 移除，另有"全部清除"。

- 面板盖住整块屏幕，按 safe_rect 和 row_span 布局，行高至少 48 px，界面文字有中、日、英三种。
- 关闭方式：上滑、返回手势、点空白处。面板打开时，返回手势只关闭面板。
  - 上滑（内容不能滚动或已滚到底时）同样跟手：接管后幕布随手指上移；松手时上移超过 H 的 1/3 或向上快速一甩就滑上去关闭，否则弹回完全打开。
  - 点空白处和返回手势用同样的上滑动画关闭（时长按剩余距离比例）。
  - 点通知行或后台场景行要打开别的东西，面板立即关闭，不做动画。
  - 代码关闭（`close_panel()`：屏幕熄灭、面板处理函数被清空、析构）也立即关闭：熄屏时动画没有意义，处理函数清空后面板不该再存在。
- 面板跟手或滑动期间：新的按下整个被忽略直到抬起（不会点中任何行，也不会开始新的手势），`tap_panel_row()` 返回 "the panel is moving"；销毁 `SystemLayer` 或代码关闭面板时会删掉进行中的动画。
- 遮挡标记（`wisp/ui/overlay.hpp`）：幕布移动期间三块标记都暂停（`pause_opaque_overlay()`），否则移动前失效的区域会按幕布旧位置被裁掉，露出来的地方留下残影；完全打开静止后恢复，下面内容的变化重新被裁掉，不必重画。
- 完全打开后系统层记日志 `ui: panel: shown`，面板销毁时记 `ui: panel: gone`，供只看日志的脚本（模拟器冒烟测试）判断。

### 5.7 持久化与事件

- 未读通知存在 System 区的 `notifications.json`，最多 50 条，最旧的先丢弃，写入走 `write_atomic` 并做 300 ms 合并。
- 总线事件：
  - `notification.posted`：`{"id","source","title"}`，不含正文；
  - `notification.dismissed`：`{"id","source","reason":"tap"|"dismiss"|"cancel"|"evict"}`。

---

## 6. 传感器与外设占用

### 6.1 `SensorService`

- 订阅类型：`accel`、`gyro`。采样率可选 10、25、50、100 Hz。
- IMU 按所有订阅者中最高的采样率工作，再按各订阅者自己的采样率降采样后分发。没有订阅者时调用 `set_power(off)`。
- 读取在一个小任务 `wisp.imu` 中进行：读 FIFO 或轮询。这个任务 `flash_access = false`，栈放 PSRAM，大小按实测确定。
- 每 100 ms 把样本打包投递给订阅者，避免消息过多。
- 手势识别是可移植代码，有主机测试：
  - `shake`：加速度模长超过阈值，在 500 ms 内出现 3 次；
  - `flip`：z 轴翻转后稳定 300 ms。
- 屏幕进入 `standby` 时暂停所有订阅，`active` 时恢复。

### 6.2 Lua API

```lua
local h = wisp.sensor.subscribe("accel", { rate = 25 }, function(samples)
  -- samples = { {x=,y=,z=,t=}, ... }，单位 m/s²（gyro 为 °/s），t 为毫秒
end)
h:stop()
local g = wisp.sensor.on("shake", fn)   -- 也可以是 "flip"；g:stop()
wisp.sensor.available()                 -- { accel = true, gyro = true }
```

- 场景转入后台时自动暂停订阅，回到前台自动恢复，关闭时自动释放。
- 没有 IMU 时，`available()` 返回空表，`subscribe` 返回 `unavailable`。

### 6.3 `OccupancyRegistry`

- 接口：`acquire(device_id, holder, mode)` / `release`，`mode` 为 `shared` 或 `exclusive`；外加 `list()`。
- 本期只有 `SensorService` 以共享方式登记 `imu.main`。
- REST `GET /system/occupancy`；管理后台系统页显示。
- 抢占和用户确认流程留给子项目 4 和 5。

---

## 7. 存储

### 7.1 `wisp.files`

```lua
wisp.files.write(name, data, cb)          -- 覆盖
wisp.files.append(name, data, cb)
wisp.files.read(name, offset, len, cb)    -- cb(err, data)，len ≤ 64 KB
wisp.files.list(cb)                       -- cb(err, { {name=, size=, mtime=}, ... })
wisp.files.remove(name, cb)
wisp.files.info(cb)                       -- cb(err, { location="sd"|"int", want="auto"|"sd"|"int", quota=, used= })
wisp.http.download(url, name, opts, cb)   -- 流式写入文件，受配额和 http_hosts 限制
```

- 文件名：`[A-Za-z0-9._-]`，1–64 字节，不能以 `.` 开头，只有一层平铺目录。不合法的名字返回 `bad_name`。
- 写操作都经 `wisp.fs` 执行；`write` 用 `write_atomic`，`append` 是直接追加。
- 单次 `write` 或 `append` 的数据不超过 256 KB。更大的数据请用 `download` 或多次 `append`。
- `download` 不受 3a 中 256 KB 响应体上限限制，只受配额限制。它和普通 HTTP 共用频率限额，在深度待机中按 4.4 排队。

### 7.2 位置与配额

- 场景在 `scene.json` 里用 `files` 字段声明文件放在哪里，取值为 `"auto"`（默认，不写时即此值）、`"sd"` 或 `"int"`；其他值安装时拒绝，错误信息含字段名。
- 第一次写文件时按声明决定位置：
  - `"auto"`：有可用的 SD 卡就放 `<sd>/wisp/library/scenes/<id>/files`（Library 区在 SD 卡上的根，基础 spec 7.2），配额 64 MB；没有就放 `/int/scenes/<id>/files`，配额 512 KB；
  - `"sd"`：只放 SD 卡。没有可用的卡时返回 `unavailable`，不退回内部存储。需要大存储的场景应声明 `"sd"`；
  - `"int"`：总是放内部存储，配额 512 KB。
- 声明 `"sd"` 的场景在没有卡时照样可以安装和运行，只是文件调用返回 `unavailable`；管理后台场景页标出"需要 SD 卡"。
- 选定的位置记在场景的 data 目录里，此后不再改变。场景更新时改了 `files` 声明，已选定的位置也不变、文件不搬（场景文件在 SD 卡与内部存储之间迁移不在本期，1.4），只记一条日志；新声明只对还没写过文件的场景生效。
- 位置在 SD 卡上而卡被拔掉时，所有调用返回 `unavailable`。卡重新插入后恢复。
- 已用空间开机时统计一次，之后在每次写入时增量更新；写入前检查配额，超出返回 `quota_exceeded`。
- 删除场景时，删除它的 `files/`（两个位置都检查）。
- 导出场景不包含文件。
- 管理后台场景页可以列出、下载、删除文件：
  - REST `GET /scenes/{id}/files`；
  - `GET /scenes/{id}/files/{name}`；
  - `DELETE /scenes/{id}/files/{name}`，需要 `X-Wisp` 头。
  - 附加字段：`GET /scenes` 每项有 `files`（声明的 `"auto"`/`"sd"`/`"int"`），顶层有 `sd_card`（SD 卡是否可用），供页面标出"需要 SD 卡"；`GET /scenes/{id}/files` 返回 `want`（同一声明）和 `truncated`（列表只含前 256 个名字时为 true）。

### 7.3 数据迁移 `migrate`

- `scene.json` 的 `schema_version` 可以是 1–1000 的整数，表示数据结构的版本。
- `SceneStore` 记录数据当前的版本，首次创建时等于 manifest 的值，随第一次写入数据落盘；从不写数据的场景不建 `data/` 目录。
- 打开场景时（包括开机恢复），如果数据版本低于 manifest，先在执行 `main.lua` 之后、建立界面之前调用 `scene.migrate(old_version)`：
  - 时限 2 秒；
  - 从执行 `main.lua` 起到 `migrate` 结束，对 `wisp.store` 的写入先缓存；
  - 成功后一次性提交，并把版本更新为新值；
  - 出错或超时就丢弃全部缓存写入，原数据不变，场景进入 `error`，错误面板显示 `migrate failed: <msg>`。
- 场景没有定义 `migrate`、但版本需要升级时，直接把版本更新为新值，视为结构兼容。
- 数据版本高于 manifest（降级安装）时拒绝打开，返回 `data version <n> newer than scene <m>`。
- 记录版本的文件损坏、读不出版本时同样拒绝打开，返回 `data version unreadable`，数据不动。
- 安装时的检查不变，迁移只在打开场景时执行。

---

## 8. 示例场景

- **pomodoro**：声明 `background`，到点发出 `sound = "alarm"`、`wake = true` 的通知。
- **weather**：声明 `background`，每 30 分钟刷新一次。
- **level**（新增，水平仪）：订阅 `accel`，显示一个气泡和倾角，用矢量大字显示度数。
- 示例场景的 `schema_version` 保持 1。另在测试用的场景里覆盖 `migrate`。

---

## 9. 测试与验收

### 9.1 主机测试（GoogleTest，TDD，全部使用假时钟）

- **`WakeLockRegistry` 与 `PowerManager`**：
  - 各级之间的切换；
  - `dim` 的时机；
  - `deep_after_s` 及其为 0 的情况；
  - `network` 锁阻止关 Wi-Fi，`cpu` 锁阻止浅睡；
  - `screen` 锁随场景转入后台释放；
  - 30 分钟提醒；
  - caps 策略的遍历；
  - `power.state`、`power.wakelock` 事件。
- **深度待机联网队列**：15 分钟窗口、每场景 4 条上限、1 小时超时、`deferred` 标记。
- **后台场景**：
  - 3.2 表中每一种转换；
  - 后台时声明树更新但不渲染；
  - 定时器在灭屏时继续；
  - 内存预算淘汰与数量淘汰；
  - 淘汰通知；
  - 开机恢复以及恢复失败跳过；
  - 后台禁用的 API 返回错误。
- **通知**：替换、限额、未读上限、持久化、点击切回前台或启动场景、前台场景的通知不进列表、事件不含正文。
- **系统层**（三种屏幕形状）：
  - 胶囊的显示与隐藏；
  - 面板布局在安全区内；
  - 下滑手势不拦截点击；
  - 面板打开时返回手势只关闭面板；
  - 横幅排队。
- **传感器**：引用计数、最高采样率、降采样、shake/flip 识别、后台自动暂停、没有 IMU 时的降级。
- **`wisp.files`**：名字校验、配额、位置记录、SD 卡拔出返回 `unavailable`、download 流式写入并受配额限制、删除场景时一并删除。
- **migrate**：成功提交、失败回滚、超时、没有 `migrate` 时直接升版本、降级时拒绝。
- **REST**：新增接口的鉴权和错误格式。

### 9.2 模拟器

- 冒烟测试覆盖：
  - 打开声明了 background 的场景 → 返回 → 胶囊出现 → 打开面板 → 切回前台；
  - 通知横幅。
- 截图：面板和横幅，在 rect、rounded_rect、circle 三种形状上各一张。
- 假 IMU 的命令行参数 `--imu-script` 可以注入摇一摇和倾斜。

### 9.3 真机验收（由用户执行，控制者烧录）

- **浅睡兼容性**：放在计划的第 1 个任务，先做 spike。检查以下各项在浅睡下是否正常：PSRAM、屏幕、触摸唤醒、定时器精度，以及 USB 串口重新连接。不兼容时裁定降级为"深度待机只关 Wi-Fi"，并记录到硬件文档。
- **后台与提醒**：
  - 番茄钟在后台、待机、深度待机下到点亮屏响铃；
  - 天气在深度待机中按 15 分钟窗口刷新。
- **面板与胶囊**：圆角屏上完整可见，下滑手势不影响场景内滚动。
- **耗电**：
  - `standby` 和 `deep_standby` 各放置 2 小时，记录电量百分比的下降；
  - 唤醒锁表与实际状态一致。
- **内存**：
  - 3 个后台场景加 1 个前台场景时的 PSRAM 和内部 RAM 余量；
  - `wisp.imu` 栈的高水位；
  - 切回前台的重建耗时。
  - 结果记入 `docs/hardware/<板名>.md`。

---

## 10. 风险与对策

| 风险 | 对策 |
|---|---|
| 自动浅睡与 PSRAM、屏幕或 USB 串口不兼容 | 计划第一个任务就上真机验证。不行时降级为"深度待机只关 Wi-Fi、不浅睡"，`power/light_sleep` 设置保留 |
| 3 MB 的 VM 总预算加上字形缓存、LVGL、HTTP，超过 PSRAM 可用量（约 4.3 MB） | 真机测最坏情况。预算值是常量，实测后调整；淘汰机制保证前台场景可以打开 |
| 切回前台重建控件太慢 | 测量；必要时对大列表只建可见行（3a 的列表已经是按需建行） |
| 场景频繁发通知打扰用户 | 每分钟 10 条的限额，加每场景 5 条未读上限；以后在子项目 7 加用户屏蔽 |
| 开机恢复的场景反复出错拖慢启动 | 失败的场景从列表中移除，下次开机不再恢复 |
| SD 卡热插拔时 files 处于中间状态 | 写入经 `wisp.fs` 串行执行；SD 卡移除事件让该场景的 files 进入 `unavailable`，进行中的写入返回错误 |

## 2026-11-12 修订：Agent 扩展工具（4b-1）

本节修订优先于本文对应的旧条款；完整规则见 [`2026-11-09-agent-tools-design.md`](2026-11-09-agent-tools-design.md) 第 9 节及其指向章节。

- 场景状态新增 `headless`，scene.json 新增 `tools` 声明。未运行场景可因工具调用而无视图启动，不要求 background，不占后台数量，不持久化后台列表；共用 VM 预算，同时仅一个，最后调用结束后闲置 60 s 关闭并落盘。
- `scene.on_state` 与 `wisp.scene.state()` 支持 headless；打开时沿用同一 VM 转前台。headless 启动及转前台、后台保留 launch reason `user`。总调用超时只取消任务；VM 执行片或内存故障才触发场景故障处理。详见 4b-1 第 5.4 节。

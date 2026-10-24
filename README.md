# NCM_Plugin — 网易云音乐「可加载插件」注入方案研究

参考 [std-microblock/chromatic](https://github.com/std-microblock/chromatic)（BetterNCM 继任者）的注入原理，
针对**当前版本网易云音乐**的插件注入方案研究与可编译脚手架。

> 本仓库只做授权范围内的技术研究与兼容性分析，产物面向「本机自用 / 插件开发者调试」。

> ⚠️ **版本提示**：文档里的环境数据起于 `3.1.40.205461` / `libcef 91.2.2`；
> 实验过程中客户端**自动升级到了 `3.1.42.205594` / `libcef 91.2.3`**。
> 两版的导出名集合一致但 RVA 有漂移，详见 [06 §6.5.3](docs/06-基本注入实测与检测面结论.md)。
> **不要硬编码 RVA**，一律按符号名解析。

---

## 〇、实测结论（真的注入了一次）

已在网易云上完成一轮 **P1 只读探测注入**（`msimg32.dll` 代理劫持），完整报告见
**[06 · 基本注入实测与检测面结论](docs/06-基本注入实测与检测面结论.md)**：

| 层 | 问题 | 实测答案 |
|---|---|---|
| 载入层 | 代理能进进程吗 | ✅ **能，而且全量**：`main` + `renderer` + `gpu-process` 三角色全中 |
| 存活层 | 会被卸载 / 干掉吗 | ✅ **不会**。心跳 120/120 全满，`DLL_PROCESS_DETACH(reserved=NULL)` **零次**；主进程存活 14 小时 |
| 检测层 | 被看见了就反制吗 | ⚠️ **本档未触发任何反制**，但静态痕迹（无签名/路径异常/无版本资源）依然摆着 |

### 注入层与插件宿主（P2 进行中，最新一轮实机结果）

| 层 | 问题 | 实测答案 | 依据 |
|---|---|---|---|
| 钩子 | CEF 入口能拦到吗 | ✅ **能**：`DllMain` 内 IAT 抢装 4 个导出，`补丁=4` 全中，靠导入描述符顺序（`libcef`=#12、`MSIMG32`=#33）保证零竞争 | [07 §7.2](docs/07-注入层实现与实测死路.md) |
| 可见效果 | 注入之后能看见吗 | ✅ **并入标题栏图标行**：私信/设置/皮肤/宠物右侧多一个音符小图标（20×20，与原生同风格，点击开面板）；横幅/浮窗/标题后缀均已移除 | [07 §7.40](docs/07-注入层实现与实测死路.md) |
| 原生桥 | JS 能调到 DLL 吗 | ✅ **能**：扩展上下文里 `native function __ncmHostCall` 实测可调（mode 7/8/11 全存活） | [07 §7.6](docs/07-注入层实现与实测死路.md) |
| 插件扫描 | 插件能被发现吗 | ✅ **能**：`插件 demo.plugin: ok name="Demo" v1.0.0 entry=startup_script.js ncm_req=">=3.0.0 <4" ncm3=1 injects=1 hijacks=1` | [08 §8.4.1](docs/08-与参照实现的逐项对齐.md) |
| 插件校验 | 会拦坏插件吗 | ✅ **会**：`manifest_version=1` 与 `ncm_version_req=">=99.0.0"` 两个样例按原因拒绝 | [08 §8.4.1](docs/08-与参照实现的逐项对齐.md) |
| 宿主 API | 插件能用什么 | ✅ **页面内可用**：`ui/ipc/hook/injects` + `ncm.player`（歌曲/歌词/音频/进度）+ `betterncm` 兼容层（getPlayingSong / getLyrics / play / pause / seek） | [07 §7.27–7.31](docs/07-注入层实现与实测死路.md) |
| 热重载 | 改了插件能重扫并生效吗 | ⚠️ 检测+重扫✅真实；「重新投递」因页面 JS 不执行而无实际效果 | [07 §7.19](docs/07-注入层实现与实测死路.md) |
| 页面 DOM 注入 | JS 能进页面吗 | ✅ **能**：inline hook `ResourceLoader::LoadByUrl` + 按 MSVC 对齐规则扩容容器，把载荷追加进 JS chunk；实测页面顶部出现横幅、插件 `ui-demo` 在左下角创建了 DOM 节点 | [07 §7.25–7.27](docs/07-注入层实现与实测死路.md) |
| 对齐 | 和 BetterNCM 差多少 | 注入层 ✅ 对齐；插件骨架 ✅ 对齐（`injects` 形状待改）；页面注入 ❌ 硬缺口 | [08 §8.6](docs/08-与参照实现的逐项对齐.md) |

**一句话**：当前版本网易云**没有在做主动的模块完整性巡检**，所以 P1→P2 可以推进；
但这是「没被抓住」而不是「抓不住」，P3 长驻宿主必须假设某次更新会加上巡检。

---

## 一、结论速览

| 问题 | 结论 | 依据 |
|---|---|---|
| 网易云 3.x 还是 CEF 吗 | 是。`libcef.dll 91.2.3+g3268653+chromium-91.0.4472.169`（升级前为 91.2.2） | [02 §2.1](docs/02-目标环境勘察.md) |
| 还能 DLL 代理注入吗 | 能。`cloudmusic.dll` **静态导入 `MSIMG32.dll`**，安装目录内**无** `msimg32.dll` | [02 §2.2](docs/02-目标环境勘察.md) |
| 子进程能一起进吗 | 能，**已实测**：`cloudmusic.exe --type=renderer` / `--type=gpu-process`，代理一次落盘全覆盖 | [06 §6.3.2](docs/06-基本注入实测与检测面结论.md) |
| 沙箱拦不拦 | 不拦，**已实测**：renderer 与 gpu 的命令行里各带两个 `--no-sandbox` | [06 §6.3.2](docs/06-基本注入实测与检测面结论.md) |
| CEF Hook 点还在吗 | 在，且**逐个实测到了序号与 RVA**（注意 RVA 随小版本漂移） | [02 §2.3.1](docs/02-目标环境勘察.md) |
| 为什么必须换回调指针而不是 hook 方法 | libcef **只导出 200 个自由函数**，接口方法全在结构体里，无法按名字 hook | [02 §2.3.1](docs/02-目标环境勘察.md)、[03 §3.3.0](docs/03-插件注入方案设计.md) |
| chromatic 直接能用吗 | 引擎层直接可用；`src/injectee` 需替换为 NCM 专用宿主（本仓库 `ncm-loader/`） | [01 §1.4–1.5](docs/01-chromatic注入原理分析.md) |
| 热重载怎么做 | 复用 `runtime::reset()`：JS dispose → 原生全量反注册 → 重建引擎 → 重跑 index.js | [01 §1.2.2](docs/01-chromatic注入原理分析.md) |
| 自动升级会不会冲掉代理 | **不会**，已实测：升级替换了 `libcef.dll`/`cloudmusic.dll`，代理文件幸存且新版本照常加载 | [06 §6.5.3](docs/06-基本注入实测与检测面结论.md) |
| 有没有"服务端下发的清理开关" | **有**。`safemode\config` 里有 `enableL1Clean`/`enableL2Clean`（当前 false，可远端下发） | [06 §6.5.2](docs/06-基本注入实测与检测面结论.md) |

---

## 二、目录

```
NCM_Plugin/
├── README.md                            本文件
├── docs/
│   ├── 01-chromatic注入原理分析.md       引擎层 + v2 入口/宿主层原理拆解
│   ├── 02-目标环境勘察.md                3.1.40 实测：导入表 / 字符串 / libcef 导出表 / 进程模型
│   ├── 03-插件注入方案设计.md            分层架构、进程分发、CEF Hook 上移、热重载、IPC
│   ├── 04-插件规范与宿主API.md           manifest.json / ncm.* API / 生命周期
│   ├── 05-构建部署验证与对抗面.md        构建、部署、验证清单、风险对抗
│   ├── 06-基本注入实测与检测面结论.md    ★ 真跑了一轮注入：结果矩阵 / 反制面 / 结论
│   ├── 07-注入层实现与实测死路.md        ★ IAT 钩子 / 偏移推导 / 四条死路的实测数据
│   └── 08-与参照实现的逐项对齐.md        与 chromatic / BetterNCM 的逐项对照与差距排序
├── chromatic/                           参考项目（已克隆，master + origin/v2）
├── reference/
│   ├── v2-extracted/                    origin/v2 关键源码（GBK→UTF-8，供对照）
│   ├── libcef-91.2.2-exports.txt         实测导出符号（升级前）
│   └── libcef-91.2.3-exports.txt         实测导出符号（升级后，含 4 个 Chromium 内部函数）
├── probe/                                ★ P1 只读探测注入（已编译、已实测、可复现）
│   ├── msimg32_probe.c                  代理 DLL：5 导出转发 + 角色/模块/心跳/卸载自检
│   ├── proxy_msimg32.{asm,def}          MASM64 跳板 + 导出表
│   ├── host.c                          本地冒烟测试宿主（验证 5 个导出转发正确）
│   ├── build.bat                       免 vcvars 的确定性构建（MSVC 14.44 + SDK 10.0.26100）
│   ├── deploy_probe.ps1                装 / 验 / 卸（写 Program Files 需提权）
│   ├── observe.ps1                     只观测：采样进程 + 收日志 + 数据目录快照
│   ├── run_experiment.ps1              ★ 一轮全自动：装→启动→边跑边读→收尾→卸载
│   └── *elevated.cmd                   提权入口（纯 ASCII）
├── evidence/                            实测原始证据（索引见 evidence/README.md）
│   └── inject/{badge,app-probe,browser-route,mode*}  P2 各阶段原始日志
├── ncm-inject/                          ★ 已编译已实机验证的注入层（取代 ncm-loader 的入口层）
│   ├── src/{proxy,util,plugins,host,inject,dllmain}.c  代理 / 基础设施 / 插件扫描 / 原生桥 / CEF 钩子 / 入口
│   ├── src/proxy_msimg32.{asm,def}      MASM64 跳板 + 5 导出转发
│   ├── payload/ncm-page.js              页面侧宿主（当前受阻于 docs/07 §7.4）
│   ├── build.bat / deploy.ps1 / mon_test.ps1   确定性构建 / 部署 / 分模式回归
│   └── build/msimg32.dll                产物（每次部署记录 SHA256）
├── ncm-loader/                          脚手架：chromatic-injectee 的 NCM 特化载体（未编译）
│   ├── xmake.lua
│   ├── src/dllmain.cpp                  入口层：msimg32 代理 + 角色判定（≈v2 hijack.cpp）
│   ├── src/proxy/proxy_msimg32.{def,asm} 5 个导出转发
│   ├── src/loader.cpp / loader.h        宿主层：引擎、插件扫描、热重载、IPC
│   ├── src/ncm_host.h                   原生绑定扩展点（可选升级，bindgen）
│   └── payload/
│       ├── bootstrap.js                 引导 + 极简模块加载器
│       ├── ncm.js                       宿主 API（fs 走 CModule 兜底）
│       ├── plugin_host.js               插件发现/加载/生命周期 + 极简 semver
│       └── cef_bridge.js                CEF 交互桥（v2 EasyCEFHooks 的 JS 版）
├── plugins/demo.plugin/                 示例插件（manifest.json + startup_script.js）
└── tools/
    ├── fripack_lite.py                  fripack 等价：把载荷回填进 g_embedded_config
    ├── gen_def.py                       从 PE 导出表生成转发 .def（libcef 备选路线）
    ├── build.ps1                        构建
    └── deploy.ps1                       部署 / 卸载 / 巡检
```

---

## 三、验证状态（诚实交代）

| 项目 | 状态 | 说明 |
|---|---|---|
| **P1 只读探测注入** | ✅ **已实测通过** | 两轮独立运行 × 三个进程角色；完整报告 [docs/06](docs/06-基本注入实测与检测面结论.md) |
| `probe/` 代理 DLL | ✅ 已编译 + 冒烟测试 + 实机注入 | 190,464 字节；`host.exe` 验 5 个导出全通；实机 `main`/`renderer`/`gpu` 全中 |
| 目标环境事实（导入表、字符串、导出表、版本号） | ✅ **实测** | 全部由本机 PE 解析得出，脚本见 `docs/02 §2.7`、`tools/gen_def.py` |
| libcef 导出表 | ✅ **实测（两版）** | [91.2.2](reference/libcef-91.2.2-exports.txt) / [91.2.3](reference/libcef-91.2.3-exports.txt)（升级后重取） |
| `--no-sandbox` 是否真的开着 | ✅ **已实测（此前只是字符串推断）** | renderer 与 gpu 命令行各带两次 `--no-sandbox`，见 [06 §6.3.2](docs/06-基本注入实测与检测面结论.md) |
| Python 工具（`fripack_lite.py` / `gen_def.py`） | ✅ 语法通过 + `gen_def.py` 已在真 libcef 上跑通 | |
| PowerShell 脚本（`build.ps1` / `deploy.ps1` / `run_experiment.ps1` / `observe.ps1`） | ✅ 语法通过；`run_experiment.ps1` 已实机跑通一轮 | `tools/deploy.ps1` 面向正式宿主，尚未实机跑 |
| payload JS（4 个）+ 示例插件 + manifest | ✅ `node --check` / `JSON.parse` 通过 | |
| C++ 脚手架（`ncm-loader/`） | ⚠️ **未编译验证** | 本机 **未装 xmake**；MSVC 2022 BuildTools 已在位。见下方「跑通步骤」 |
| CEF 结构体成员偏移 | ⚠️ **待核对** | 需对着随包 `include/capi/cef_*_capi.h` 确认（`docs/03 §3.3.0.1` 已列全表） |

---

## 四、跑通步骤

### 4.1 先跑 P1 探测注入（**现在就能跑，已验证**）

```powershell
# 0) 编译探测 DLL（不需要 xmake，只要 MSVC BuildTools）
cd D:\awesomeProject\NCM_Plugin\probe
.\build.bat
cmd /c "build\host.exe build\msimg32.dll"     # 冒烟测试：期望 host exit=0、5 个导出全 OK

# 1) 一轮全自动实验：装代理 → 启动网易云 → 边跑边读日志 → 收尾 → 自动卸载
#    ⚠️ 必须脱离 DSH 沙箱运行，否则网易云写不了自己的数据目录 = 假阳性
cd D:\awesomeProject\NCM_Plugin
pwsh -File probe\run_experiment.ps1 -Seconds 120 -Every 10 -OutDir evidence\run3

# 2) 看结论
Get-Content evidence\run3\experiment.log
Get-Content evidence\run3\timeline.csv
Get-ChildItem evidence\run3\probe-logs | % { "--- $($_.Name)"; Get-Content $_.FullName | Select-String 'role=|心跳 #|DETACH' | Select-Object -First 3 -Last 2 }
```

判定标准见 [06 §6.7.1](docs/06-基本注入实测与检测面结论.md)。

### 4.2 再跑完整宿主（P3，脚手架尚未编译验证）

```powershell
# 0) 装 xmake（本机当前没有）
winget install xmake        # 或 https://xmake.io/#/getting_started

# 1) 构建（先编 chromatic-core，再编 ncm-loader）
pwsh tools/build.ps1 -Mode release -Stage

# 2) 可选：把引导脚本内嵌进 DLL，免去磁盘 payload 目录
python tools/fripack_lite.py ncm-loader/build/windows/x64/release/ncm-loader.dll `
                            ncm-loader/payload/bootstrap.js

# 3) 部署（需要管理员；deploy 会自动备份并记录还原信息）
pwsh tools/deploy.ps1 -Install

# 4) 启动网易云，看日志
Get-Content "$env:LOCALAPPDATA\NCMPlugin\loader.log" -Wait

# 5) 卸载
pwsh tools/deploy.ps1 -Uninstall

# 6) 网易云更新后代理常丢，可挂着巡检
pwsh tools/deploy.ps1 -Watch -IntervalSeconds 300
```

期望日志（分级验证清单见 [05 §5.3.1](docs/05-构建部署验证与对抗面.md)）：

```
[INFO ] =================================================
[INFO ] ncm-loader v0.1.0  role=main pid=1234
[INFO ] [engine] ready (host boot)
[INFO ] plugin_host: 共加载 1 个插件 [demo.plugin]
[INFO ] loaded plugin demo.plugin (v1.0.0)
```

---

## 五、最小可用路径 vs 完整能力

- **P1 只读探测**：只转发、只打日志，验证载入与存活 —— ✅ **已完成，见 [docs/06](docs/06-基本注入实测与检测面结论.md)**
- **P2 页面注入**：`cef_bridge.js` 装到 `renderer`，往 `orpheus://` 页面注入脚本 ← **下一步**
- **P3 完整宿主**：插件目录 + 热重载 + `ncm.fs`（原生绑定或 CModule 兜底）+ IPC
- **P4 高能力**：资源劫持（`hijacks`）、原生插件 DLL、`expose()` 把原生能力暴露给页面

推进顺序建议严格按 P1 → P4，理由见 [05 §5.4](docs/05-构建部署验证与对抗面.md)（易盾 + 完整性校验），
以及实测到的「服务端可下发 L1/L2 清理开关」（[06 §6.5.2](docs/06-基本注入实测与检测面结论.md)）。
## 插件示例：局域网投送（lan-broadcast.plugin）

把当前播放的**封面 / 歌词 / 播放状态**通过 SSE 推到局域网，
协议对齐 go-musicfox（见仓库根目录 `lan_broadcast.md`）。

```bash
# 1) 实际端口（原生会避开被占用的 8765）
curl http://127.0.0.1:38021/bcast-port        # → 8790

# 2) 订阅
curl -N http://127.0.0.1:8790/events

# 3) 取封面
curl -o cover.jpg http://127.0.0.1:8790/cover
```

配置：`%LOCALAPPDATA%\NCMPlugin\broadcast.json`
（`enable` / `port` / `bind`）。详见 [docs/07 §7.41](docs/07-注入层实现与实测死路.md)。
## 插件示例：自定义 CSS（custom-css.plugin）

设置界面 → 「自定义 CSS」：编辑 CSS 立即生效，保存到 `%LOCALAPPDATA%\NCMPlugin\custom.css`。

## 开发：开启 DevTools

用带参数的启动器启动网易云（仓库里已提供 `tools/start-with-devtools.cmd`）：

```bat
cloudmusic.exe --remote-debugging-port=9222 --remote-allow-origins=*
```

然后用 Chrome / Edge 打开 <http://127.0.0.1:9222> 调试页面（含 `ncm.*` API）。
详见 [docs/07 §7.47](docs/07-注入层实现与实测死路.md)。
## 框架能力（第二批补齐）

- **原生端点**：`/host`（插件清单+脚本）、`/fs/*`（沙箱读写）、`/log`（日志落盘）
- **`ncm.fs` 写能力**：按插件 slug 隔离，根目录 `%LOCALAPPDATA%\NCMPlugin\data`
- **`ncm.onDispose`**：插件卸载/重载时的清理钩子
- **插件管理面板**：点右上角角标 → 查看状态、启用/禁用、重载
- **手动热重载**：改插件脚本后点「重载」即可生效，无需重启应用
  （自动热重载因存在自我循环问题，默认关闭）

详见 [docs/07 §7.49](docs/07-注入层实现与实测死路.md)。
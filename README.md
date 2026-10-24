# NCM Plugin Framework

网易云音乐（PC 版 3.x，CEF 内核）插件框架的**私有开发版**：通过 `msimg32.dll` 代理注入，
在渲染进程里装载一个页面插件宿主，插件用 `manifest.json` + `startup_script.js` 的形式编写。

> 仅用于个人学习与技术研究。请勿用于绕过版权保护、商业分发或任何违反服务条款的用途。

---

## 架构

```
cloudmusic.exe
   └── msimg32.dll (代理)              ← AheadLib 式转发 + IAT 补丁
         └── ncm-inject.dll            ← 注入器本体
               ├── IAT 钩子 (libcef)   ← cef_initialize / create_browser / …
               ├── inline hook         ← ResourceLoader::LoadByUrl（把载荷塞进 JS chunk）
               ├── probe_server        ← 127.0.0.1:38021（页面 ↔ 原生通道）
               └── bcast               ← 127.0.0.1:8790（SSE：局域网投送 / 热重载通知）
页面 (orpheus://orpheus/pub/app.html)
   └── ncm-page.js (载荷)              ← 插件宿主：清单解析、生命周期、宿主 API
         └── plugins/*.plugin/         ← 插件（manifest.json + startup_script.js）
```

设计要点：

- **注入时机**：`msimg32.dll` 是宿主静态导入（导入表 #33，`libcef` 是 #12），
  在 `DllMain` 里改 IAT 槽位可在 CEF 初始化之前完成，零竞争。
- **页面投递**：改 `ResourceLoader::LoadByUrl` 的返回值，把载荷追加到 JS chunk 末尾
  （追加时用 MSVC 对齐分配规则，否则宿主 vector 析构会触发 `invalid_parameter`）。
- **页面通道**：页面没有 socket 能力，所以凡是需要监听端口的（SSE、文件、日志）都由原生提供，
  页面通过 `127.0.0.1:38021` 访问。

---

## 能力

### 插件规范（沿用 BetterNCM v2 的字段）

```json
{
  "manifest_version": 2,
  "name": "My Plugin",
  "slug": "my.plugin",
  "version": "1.0.0",
  "author": "you",
  "description": "做什么",
  "ncm_version_req": ">=3.0.0 <4",
  "ncm3Compatible": true,
  "startup_script": "startup_script.js",
  "injects": {},
  "hijacks": {}
}
```

- `startup_script.js` 在页面加载后执行，拿到一个按 slug 隔离的 `ncm` 对象
- `injects`：按 URL 匹配注入脚本 / CSS
- `hijacks`：按版本匹配改写资源

### 宿主 API（插件内可用）

| 分类 | API |
|---|---|
| 事件 | `ncm.on/off/emit`、`ncm.onDispose(fn)` |
| 日志 | `ncm.log(level, msg)` → console + 落盘 |
| 文件 | `ncm.fs.readText/writeText/list/stat/remove`（沙箱，自动按 slug 隔离） |
| 播放 | `ncm.player.current()`、`getProgress()`、`control.{play,pause,toggle,next,prev,isPlaying}` |
| 原生 | `ncm.native.call(name, cb, args)`（透传 CEF native 桥） |

`ncm.player.current()` 返回：

```js
{
  song: { id, name, artist, album, source, durationMs },
  lyric: { lines: [{ t, text, translation }], index, current, source: 'native'|'dom' },
  cover: 'https://…',
  audio: { songId, url, quality, bitrate, durationMs },
  progress: { playedTime, at, playing }
}
```

歌曲/歌词来自原生接口（`playId` → `api/v3/song/detail` → `api/song/lyric`），
不依赖界面是否打开歌词页；播放控制走页面级点击（原生写接口实测无效且易崩）。

### 原生端点（`127.0.0.1:38021`）

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/host` | 宿主信息 + 插件清单（含 `mtime` / `manifest` / `script`） |
| GET/POST | `/fs/read` `/fs/write` | 插件文件读写（沙箱：`%LOCALAPPDATA%\NCMPlugin\data`） |
| GET | `/fs/list` `/fs/stat` | 列目录 / 元信息 |
| POST | `/fs/remove` | 删除 |
| POST | `/log` | 页面日志落盘 |
| GET/POST | `/custom-css` | 自定义 CSS 持久化 |
| GET | `/bcast-port` | 局域网投送服务实际端口 |

### 插件管理面板

点窗口右上角的角标打开：查看插件状态、**启用/禁用**（写入 `data/disabled.json`）、
**重载**、复制数据目录。

> 角标**只在主窗口**（`app.html`）出现。宿主还会打开子应用页面
> （如 `subApp.html?route=pet-desktop` 桌宠小窗），这些窗口不显示角标，但插件宿主仍在其中运行。

---

## 目录结构

```
ncm-inject/            注入器（C，MSVC）
  src/                 源码
  payload/ncm-page.js  页面插件宿主
  build.bat            构建（需要 Visual Studio Build Tools）
  deploy.ps1           部署到 %LOCALAPPDATA%\NCMPlugin
plugins/               插件示例
  custom-css.plugin/   设置界面里的「自定义 CSS」面板
  lan-broadcast.plugin 封面/歌词/播放状态 SSE 投送（对齐 go-musicfox 协议）
  probe.plugin/        开发自检插件（打印宿主能力）
tools/
  start-with-devtools.cmd   带远程调试端口启动网易云
config/
  broadcast.example.json    局域网投送配置样例 → 复制为 %LOCALAPPDATA%\NCMPlugin\broadcast.json
```

---

## 构建与部署

前置：Windows x64、Visual Studio Build Tools（提供 `cl.exe` / `link.exe`）、
网易云音乐 3.1.x（本次验证版本 `3.1.42.205594`，`libcef 91`）。

```bat
cd ncm-inject
build.bat            :: 产物：build\msimg32.dll
```

```powershell
cd ncm-inject
.\deploy.ps1         # 备份原有 msimg32.dll，部署代理 DLL + 载荷 + 插件
```

部署后启动网易云即生效。回滚：`deploy.ps1` 会在同目录留下原 `msimg32.dll` 备份。

---

## 开发：DevTools

用带远程调试端口的启动器启动：

```bat
tools\start-with-devtools.cmd
```

然后用 Chrome / Edge 打开 <http://127.0.0.1:9222>，
即可对 `orpheus://orpheus/pub/app.html` 使用完整的 DevTools（控制台 / 元素 / 网络）。

---

## 已知限制

- 版本敏感：`LoadByUrl` 等使用硬编码 RVA，换网易云版本需要重新定位
- 自动热重载默认关闭（存在自我循环触发问题），改插件后请在面板里点「重载」
- 页面定时器在窗口不可见时会被 Chromium 冻结，因此推送类功能使用 SSE 而非轮询
- 原生写接口（`setVolume` 等）实测不生效且参数错误会导致崩溃，故播放控制只走页面点击
- 数据接口有频控，框架内做了稳定闸 / 节流 / 退避

---

## 参考

- [chromatic](https://github.com/std-microblock/chromatic) — 注入引擎与原生能力面的参考
- BetterNCM v2 — 插件规范（`manifest.json` 字段、`injects`/`hijacks`）与宿主 API 形态的参考

本项目为独立实现，未包含上述项目的代码。

---

## 一键包（Release）

从 [Releases](https://github.com/RemKeeper/ncm-plugin-framework/releases) 下载 `ncm-plugin-framework-<version>.zip`：

1. 解压任意目录
2. 双击 `install.cmd`（UAC 选「是」）
3. 脚本自动定位网易云目录、**备份**原始 `msimg32.dll`，并安装 DLL + 载荷 + 示例插件
4. 结束时可直接启动网易云

包内文件：

```
install.cmd / install.ps1        一键安装（自动找目录、备份、部署）
uninstall.cmd / uninstall.ps1    卸载（恢复原始 DLL）
start-devtools.cmd               带远程调试端口启动
QUICKSTART.txt                   中文使用说明
SHA256.txt                       构建信息与校验和
ncm-inject/build/msimg32.dll     注入器
ncm-inject/payload/ncm-page.js   页面载荷
plugins/                         示例插件
config/broadcast.example.json    局域网投送配置样例
```

## CI / Release

`.github/workflows/build.yml` 在 `windows-latest` 上用 MSVC 自动编译 `msimg32.dll`：

- **打 tag**（`v*`）→ 自动构建并发布到 **Releases**（附带 sha256 / 大小 / 提交号）
- **手动触发**（Actions → build → Run workflow）→ 仅产出 artifact；勾选 `release` 则用短 SHA 作为 tag 发布

本地 CI 等价构建（需要已配置 MSVC 环境）：

```bat
cd ncm-inject
build-ci.bat
```

发布产物是 `msimg32-<tag>.dll`，部署时改名为 `msimg32.dll` 放到 `cloudmusic.exe` 同目录。
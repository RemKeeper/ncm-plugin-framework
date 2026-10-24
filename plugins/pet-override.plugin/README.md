# pet-override.plugin —— DSH Pet 桌宠接管

把网易云自带的桌宠（那只戴耳机的泰迪）换成 [dsh-pet](https://github.com/PC2005-cloud/dsh-pet) 的
webm 动画形象：待机循环、随机小动作、点击回应 + 气泡。

## 原理

桌宠窗口是 `CloudMusicDesktopPetOsrView`（CEF 离屏渲染，260×379 透明窗口），页面为
`subApp.html?route=pet-desktop`。原版用 21 个 `<video>`（640×640，带 alpha 的 MSE 流）播放动画，
同一时刻只 `visibility: visible` 一两个。

本插件在该窗口里：

1. 注入 CSS 把原生 `<video>` 隐藏 —— **必须 `!important`**，App 每帧会重置 inline `visibility`
2. 在 `#pet-desktop-renderer` 里建自己的舞台，双缓冲两个 `<video>` 轮换播放
3. 素材经原生 `/asset` 端点提供（`video/webm` + Range），路径 `data/pet-override/assets/*.webm`

> 注意：隐藏规则要排除插件自己的节点（`video:not(.pet-ov-v)`），否则 `!important` 会把
> 自己的画面也隐藏掉。

## 安装素材

```powershell
pwsh -File tools/fetch-pet-assets.ps1
```

会从 dsh-pet 仓库下载 106 个 webm（约 52 MB）到
`%LOCALAPPDATA%\NCMPlugin\data\pet-override\assets\`。

想换宠物就**直接把别的 webm 丢进这个目录**（按下面命名习惯命名即可被识别），
插件启动时会自动扫描，不用改代码。

## 素材命名约定

| 前缀 / 名称 | 用途 |
|---|---|
| `待机呼吸休闲` | 待机循环 |
| `东张西望` | 转向（预留） |
| `点击回应-*` | 点击时随机播放 |
| 其它 | 随机小动作池（播一次回待机） |

名字不符合约定也能跑：没有待机片段时会把前几个片段当待机。

## 互动

- 左键点击 → 播一个 `点击回应-*` + 随机气泡台词
- 台词在 `startup_script.js` 顶部的 `LINES` 数组里，可随意改
- 想接 AI 台词：在 `onClick` 里换成 `ncm.player`/`fetch` 调用即可

## 卸载

插件面板里禁用/删除即可；`hideStyle` 与舞台会在 `ncm.onDispose` 里清理，宠物恢复正常。

## 许可

动画素材来自 [dsh-pet](https://github.com/PC2005-cloud/dsh-pet)（MIT），版权归原作者。
本插件代码随 NCM Plugin Framework 一同发布。

# deploy.ps1 — 把注入层装到网易云上（需要写 Program Files）
#
# 装了什么（只有这些，卸载就是删一个文件）：
#   1) <NCM 安装目录>\msimg32.dll            ← 代理 DLL（唯一落在 Program Files 的东西）
#   2) %LOCALAPPDATA%\NCMPlugin\payload\      ← 页面侧 JS 载荷
#   3) %LOCALAPPDATA%\NCMPlugin\plugins\      ← 插件目录（从仓库 plugins\ 同步）
#
# 用法： pwsh -File deploy.ps1 [-NcmDir <安装目录>]

param(
  [string]$NcmDir = 'C:\Program Files\NetEase\CloudMusic'
)

$ErrorActionPreference = 'Stop'
$here    = Split-Path -Parent $MyInvocation.MyCommand.Path
$profile = Join-Path $env:LOCALAPPDATA 'NCMPlugin'

Write-Host '=== ncm-inject 部署 ===' -ForegroundColor Cyan

# ── 0. 前置检查 ────────────────────────────────────────────────────────────
$dll = Join-Path $here 'build\msimg32.dll'
if (-not (Test-Path $dll)) { throw "还没编译：$dll 不存在（先跑 build.bat）" }
if (-not (Test-Path (Join-Path $NcmDir 'cloudmusic.exe'))) { throw "网易云目录不对：$NcmDir" }

# 正在跑的实例必须先退干净，否则文件被占用（而且新 DLL 会等下次启动才生效）
for ($i = 0; $i -lt 15; $i++) {
  $running = Get-Process -Name cloudmusic -ErrorAction SilentlyContinue
  if (-not $running) { break }
  if ($i -eq 0) { Write-Host "  [i] 检测到网易云在运行（$($running.Id -join ',')），先结束掉" }
  $running | Stop-Process -Force -ErrorAction SilentlyContinue
  Start-Sleep -Milliseconds 800
}
if (Get-Process -Name cloudmusic -ErrorAction SilentlyContinue) {
  throw '网易云杀不掉，请手动完全退出后再部署'
}

# ── 1. 代理 DLL ────────────────────────────────────────────────────────────
$target = Join-Path $NcmDir 'msimg32.dll'
if (Test-Path $target) {
  $old = Get-FileHash $target -Algorithm SHA256
  Write-Host ("  [覆盖] 已存在 msimg32.dll，原 SHA256 = " + $old.Hash.Substring(0,16) + '…')
}
Copy-Item $dll $target -Force

# ★ 校验：没有管理员权限时 Copy-Item 可能"静默失败"（实测：重启后以普通用户跑，
#   部署了整整一天都没生效，日志里只有一行"[覆盖] 已存在"，非常容易误判）。
$hSrc = (Get-FileHash $dll -Algorithm SHA256).Hash
$hDst = if (Test-Path $target) { (Get-FileHash $target -Algorithm SHA256).Hash } else { "" }
if ($hSrc -ne $hDst) {
  throw "部署失败：$target 与构建产物不一致（通常是没以管理员身份运行，或 DLL 被占用）。`n  源  $hSrc`n  目标 $hDst"
}
$new = Get-FileHash $target -Algorithm SHA256
Write-Host ("  [1/3] " + $target)
Write-Host ("        SHA256 = " + $new.Hash)

# ── 2. payload ────────────────────────────────────────────────────────────
$pdst = Join-Path $profile 'payload'
New-Item -ItemType Directory -Force -Path $pdst | Out-Null
Copy-Item (Join-Path $here 'payload\*.js') $pdst -Force
Write-Host ("  [2/3] " + $pdst + '  ← ' + ((Get-ChildItem $pdst -Filter *.js).Name -join ', '))

# ── 3. plugins ────────────────────────────────────────────────────────────
$repoPlugins = Join-Path (Split-Path -Parent $here) 'plugins'
$pdst2 = Join-Path $profile 'plugins'
New-Item -ItemType Directory -Force -Path $pdst2 | Out-Null
if (Test-Path $repoPlugins) {
  Copy-Item (Join-Path $repoPlugins '*') $pdst2 -Recurse -Force
}
$found = (Get-ChildItem $pdst2 -Directory -ErrorAction SilentlyContinue).Name
Write-Host ("  [3/3] " + $pdst2 + '  ← ' + ($found -join ', '))

Write-Host ''
Write-Host '部署完成。启动网易云后，日志在：' -ForegroundColor Green
Write-Host ("  " + (Join-Path $profile 'inject-*.log'))
Write-Host '页面里应出现右下角徽标 "NCM Plugin Host · n/n"。' -ForegroundColor Green

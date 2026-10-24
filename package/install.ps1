param(
  [switch]$NoLaunch
)

# install.ps1 — 一键安装：代理 DLL + 页面载荷 + 示例插件
# 由 install.cmd 以管理员身份调用（需要写 Program Files）。

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$profileDir = Join-Path $env:LOCALAPPDATA 'NCMPlugin'
$backupDir  = Join-Path $profileDir 'backup'

function Write-Step($n, $msg) { Write-Host ("  [${n}] " + $msg) -ForegroundColor Cyan }
function Write-Ok($msg)      { Write-Host ("      " + $msg) -ForegroundColor Green }
function Write-Warn($msg)    { Write-Host ("      " + $msg) -ForegroundColor Yellow }

Write-Host ''
Write-Host '=== NCM Plugin Framework 一键安装 ===' -ForegroundColor White
Write-Host ''

# ── 1. 定位网易云安装目录 ───────────────────────────────────────────────
$candidates = @(
  'C:\Program Files\NetEase\CloudMusic',
  'C:\Program Files (x86)\NetEase\CloudMusic',
  (Join-Path $env:LOCALAPPDATA 'Programs\NetEase\CloudMusic'),
  (Join-Path ${env:ProgramFiles(x86)} 'NetEase\CloudMusic')
) | Where-Object { $_ }

# 注册表里找（网易云自己写的卸载项）
try {
  $keys = @(
    'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*',
    'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*'
  )
  foreach ($k in $keys) {
    Get-ItemProperty $k -ErrorAction SilentlyContinue |
      Where-Object { $_.DisplayName -like '*网易云*' -or $_.DisplayName -like '*CloudMusic*' } |
      ForEach-Object {
        if ($_.InstallLocation) { $candidates += $_.InstallLocation.TrimEnd('\') }
        if ($_.DisplayIcon) {
          $d = Split-Path -Parent ($_.DisplayIcon -replace ',\d+$','')
          if ($d) { $candidates += $d }
        }
      }
  }
} catch {}

$ncmDir = $null
foreach ($c in ($candidates | Select-Object -Unique)) {
  if (Test-Path (Join-Path $c 'cloudmusic.exe')) { $ncmDir = $c; break }
}

# 兜底：在常见目录里找 cloudmusic.exe
if (-not $ncmDir) {
  Write-Warn '注册表与常见路径都没找到，开始搜索 Program Files…'
  $hit = Get-ChildItem 'C:\Program Files','C:\Program Files (x86)' -Filter 'cloudmusic.exe' -Recurse -Depth 3 -ErrorAction SilentlyContinue |
         Select-Object -First 1
  if ($hit) { $ncmDir = $hit.DirectoryName }
}

if (-not $ncmDir) {
  $ncmDir = Read-Host '没自动找到网易云目录，请手动输入（例如 C:\Program Files\NetEase\CloudMusic）'
  if (-not (Test-Path (Join-Path $ncmDir 'cloudmusic.exe'))) {
    throw "目录里没有 cloudmusic.exe：$ncmDir"
  }
}
Write-Step '1/4' ("网易云目录：" + $ncmDir)

# ── 2. 结束正在运行的网易云（否则 DLL 被占用） ──────────────────────────
$running = Get-Process -Name cloudmusic -ErrorAction SilentlyContinue
if ($running) {
  Write-Step '2/4' ("检测到网易云在运行（" + (($running.Id) -join ',') + '），先退出')
  for ($i = 0; $i -lt 15; $i++) {
    $running | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 700
    if (-not (Get-Process -Name cloudmusic -ErrorAction SilentlyContinue)) { break }
  }
  if (Get-Process -Name cloudmusic -ErrorAction SilentlyContinue) {
    throw '网易云没能退出，请手动完全退出后重试'
  }
} else {
  Write-Step '2/4' '网易云未在运行'
}

# ── 3. 备份原始 msimg32.dll（只在第一次、且它不是我们的 DLL 时备份） ────
New-Item -ItemType Directory -Force -Path $backupDir | Out-Null
$targetDll = Join-Path $ncmDir 'msimg32.dll'
$srcDll    = Join-Path $here 'ncm-inject\build\msimg32.dll'
if (-not (Test-Path $srcDll)) { throw "安装包不完整：缺少 $srcDll" }

$ourHash = (Get-FileHash $srcDll -Algorithm SHA256).Hash
$backup  = Join-Path $backupDir 'msimg32.dll.orig'

if (Test-Path $targetDll) {
  $curHash = (Get-FileHash $targetDll -Algorithm SHA256).Hash
  if ($curHash -ne $ourHash) {
    if (-not (Test-Path $backup)) {
      Copy-Item $targetDll $backup -Force
      Write-Step '3/4' '已备份原 msimg32.dll → backup\msimg32.dll.orig'
    } else {
      Write-Step '3/4' '原 DLL 已在早前备份过，保持不变'
    }
  } else {
    Write-Step '3/4' '当前已是本框架的 DLL，无需备份'
  }
} else {
  Write-Step '3/4' '目录内没有 msimg32.dll（首次安装），无需备份'
}

Copy-Item $srcDll $targetDll -Force
Write-Ok ("已安装 " + $targetDll)
Write-Ok ("SHA256 " + $ourHash.Substring(0,16) + '…')

# ── 4. 页面载荷 + 插件 ──────────────────────────────────────────────────
$payloadSrc = Join-Path $here 'ncm-inject\payload'
$payloadDst = Join-Path $profileDir 'payload'
New-Item -ItemType Directory -Force -Path $payloadDst | Out-Null
Copy-Item (Join-Path $payloadSrc '*.js') $payloadDst -Force

$pluginsSrc = Join-Path $here 'plugins'
$pluginsDst = Join-Path $profileDir 'plugins'
New-Item -ItemType Directory -Force -Path $pluginsDst | Out-Null
if (Test-Path $pluginsSrc) { Copy-Item (Join-Path $pluginsSrc '*') $pluginsDst -Recurse -Force }

$names = (Get-ChildItem $pluginsDst -Directory -ErrorAction SilentlyContinue).Name -join ', '
Write-Step '4/4' ("载荷 → " + $payloadDst)
Write-Ok ("插件 → " + $pluginsDst + "  (" + $names + ")")

# 局域网投送配置（不存在才放样例）
$bcast = Join-Path $profileDir 'broadcast.json'
if (-not (Test-Path $bcast)) {
  $ex = Join-Path $here 'config\broadcast.example.json'
  if (Test-Path $ex) { Copy-Item $ex $bcast -Force; Write-Ok '已放置 broadcast.json 样例（端口 8790）' }
}

Write-Host ''
Write-Host '=== 安装完成 ===' -ForegroundColor Green
Write-Host ("  插件数据目录：" + (Join-Path $profileDir 'data'))
Write-Host ("  运行日志：    " + (Join-Path $profileDir 'inject-*.log'))
Write-Host '  窗口右上角出现插件图标即表示注入成功。'
Write-Host ''

if (-not $NoLaunch) {
  $ans = Read-Host '现在启动网易云？(Y/n)'
  if ($ans -eq '' -or $ans -match '^[Yy]') {
    Start-Process -FilePath (Join-Path $ncmDir 'cloudmusic.exe')
    Write-Host '已启动。' -ForegroundColor Green
  }
}

Write-Host ''
Write-Host '按任意键关闭…'
$null = $Host.UI.RawUI.ReadKey('NoEcho,IncludeKeyDown')

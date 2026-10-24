# fetch-pet-assets.ps1 —— 拉取 dsh-pet 的 webm 动画素材，供 pet-override 插件使用
#
# 素材来源：https://github.com/PC2005-cloud/dsh-pet （MIT）
# 只下载 dsh-pet/assets/webm/*.webm（约 52 MB），不拉 GIF 预览与截图。
#
# 用法：
#   pwsh -File tools/fetch-pet-assets.ps1              # 下载到 %LOCALAPPDATA%\NCMPlugin\data\pet-override\assets
#   pwsh -File tools/fetch-pet-assets.ps1 -SkipExisting # 断点续传：已存在的跳过
#   pwsh -File tools/fetch-pet-assets.ps1 -Dest D:\pets  # 指定目标目录

param(
  [string]$Dest = (Join-Path $env:LOCALAPPDATA 'NCMPlugin\data\pet-override\assets'),
  [switch]$SkipExisting,
  [int]$Parallel = 8
)

$ErrorActionPreference = 'Stop'
$owner = 'PC2005-cloud'
$repo  = 'dsh-pet'
$base  = 'main'
$dirInRepo = 'dsh-pet/assets/webm'

New-Item -ItemType Directory -Force -Path $Dest | Out-Null
Write-Host "=== 拉取 dsh-pet 动画素材 ===" -ForegroundColor Cyan
Write-Host "  目标目录：$Dest"

# ── 1. 列出 webm 文件（一个 API 请求） ────────────────────────────────
$treeUrl = "https://api.github.com/repos/$owner/$repo/git/trees/$base`?recursive=1"
$headers = @{ 'User-Agent' = 'ncm-inject-fetch'; Accept = 'application/vnd.github+json' }
try {
  $tree = Invoke-RestMethod -Uri $treeUrl -Headers $headers -TimeoutSec 30
} catch {
  throw "列文件失败（可能是 API 限流，稍后重试）：$($_.Exception.Message)"
}

$files = $tree.tree | Where-Object {
  $_.type -eq 'blob' -and $_.path -like "$dirInRepo/*.webm"
}
if (-not $files) { throw "仓库里没找到 $dirInRepo/*.webm（仓库结构可能变了）" }
Write-Host "  找到 $($files.Count) 个片段" -ForegroundColor Green

# ── 2. 下载（raw.githubusercontent.com，支持并行） ────────────────────
$jobs = @()
foreach ($f in $files) {
  $name = Split-Path $f.path -Leaf
  $out  = Join-Path $Dest $name
  if ($SkipExisting -and (Test-Path $out) -and (Get-Item $out).Length -gt 0) { continue }
  $jobs += [pscustomobject]@{
    Name = $name
    Url  = "https://raw.githubusercontent.com/$owner/$repo/$base/" + ($f.path -replace ' ', '%20')
    Out  = $out
  }
}

if (-not $jobs.Count) { Write-Host "  全部已存在，无需下载" -ForegroundColor Green; return }

Write-Host "  待下载：$($jobs.Count) 个"

$ok = 0; $fail = 0
if ($PSVersionTable.PSVersion.Major -ge 7) {
  $jobs | ForEach-Object -Parallel {
    try {
      Invoke-WebRequest -Uri $_.Url -OutFile $_.Out -UseBasicParsing -TimeoutSec 120
      return @{ ok = $true; name = $_.Name }
    } catch {
      return @{ ok = $false; name = $_.Name; err = $_.Exception.Message }
    }
  } -ThrottleLimit $Parallel | ForEach-Object {
    if ($_.ok) { $ok++ } else { $fail++; Write-Host ("    ✗ " + $_.name + " : " + $_.err) -ForegroundColor Yellow }
  }
} else {
  $i = 0
  foreach ($j in $jobs) {
    $i++
    try {
      Invoke-WebRequest -Uri $j.Url -OutFile $j.Out -UseBasicParsing -TimeoutSec 120
      $ok++
      if ($i % 10 -eq 0) { Write-Host ("    ... $i / $($jobs.Count)") }
    } catch {
      $fail++
      Write-Host ("    ✗ " + $j.Name + " : " + $_.Exception.Message) -ForegroundColor Yellow
    }
  }
}

# ── 3. 汇总 ──────────────────────────────────────────────────────────
$all = Get-ChildItem $Dest -Filter *.webm -ErrorAction SilentlyContinue
$totalMb = if ($all) { [math]::Round(($all | Measure-Object Length -Sum).Sum / 1MB, 1) } else { 0 }
Write-Host ""
Write-Host "=== 完成 ===" -ForegroundColor Green
Write-Host ("  本次成功 " + $ok + "，失败 " + $fail)
Write-Host ("  目录内共 " + $all.Count + " 个片段，" + $totalMb + " MB")
Write-Host "  重启网易云（或点插件面板的「重载」）后生效。"

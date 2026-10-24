param()

# uninstall.ps1 — 恢复原始 msimg32.dll，并移除载荷/插件
# 由 uninstall.cmd 以管理员身份调用。

$ErrorActionPreference = 'Continue'
$profileDir = Join-Path $env:LOCALAPPDATA 'NCMPlugin'
$backup     = Join-Path $profileDir 'backup\msimg32.dll.orig'

Write-Host ''
Write-Host '=== NCM Plugin Framework 卸载 ===' -ForegroundColor White
Write-Host ''

# 找网易云目录（与安装脚本相同的策略，但只用于定位 DLL）
$candidates = @(
  'C:\Program Files\NetEase\CloudMusic',
  'C:\Program Files (x86)\NetEase\CloudMusic',
  (Join-Path $env:LOCALAPPDATA 'Programs\NetEase\CloudMusic')
) | Where-Object { $_ }

$ncmDir = $null
foreach ($c in $candidates) {
  if (Test-Path (Join-Path $c 'cloudmusic.exe')) { $ncmDir = $c; break }
}
if (-not $ncmDir) {
  $ncmDir = Read-Host '请输入网易云安装目录'
}

$running = Get-Process -Name cloudmusic -ErrorAction SilentlyContinue
if ($running) {
  Write-Host '  正在退出网易云…' -ForegroundColor Cyan
  for ($i = 0; $i -lt 15; $i++) {
    $running | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 700
    if (-not (Get-Process -Name cloudmusic -ErrorAction SilentlyContinue)) { break }
  }
}

$targetDll = Join-Path $ncmDir 'msimg32.dll'
if (Test-Path $backup) {
  Copy-Item $backup $targetDll -Force
  Write-Host ('  已恢复原始 msimg32.dll ← ' + $backup) -ForegroundColor Green
} elseif (Test-Path $targetDll) {
  Remove-Item $targetDll -Force
  Write-Host '  目录里没有备份，已直接删除 msimg32.dll' -ForegroundColor Yellow
  Write-Host '  （若原版本系统自带 msimg32.dll，可从系统目录复制回来）' -ForegroundColor Yellow
}

foreach ($sub in @('payload', 'plugins')) {
  $p = Join-Path $profileDir $sub
  if (Test-Path $p) {
    Remove-Item $p -Recurse -Force
    Write-Host ('  已移除 ' + $p) -ForegroundColor Green
  }
}

Write-Host ''
Write-Host ('  插件数据保留在：' + (Join-Path $profileDir 'data')) -ForegroundColor Cyan
Write-Host '  （含自定义 CSS、禁用列表等，不需要可手动删除整个 NCMPlugin 目录）'
if ($Host.Name -eq 'ConsoleHost' -and -not [Console]::IsInputRedirected) {
  Write-Host ''
  Write-Host '按任意键关闭…'
  $null = $Host.UI.RawUI.ReadKey('NoEcho,IncludeKeyDown')
}

@echo off
rem Start NetEase Cloud Music with the remote debugging port enabled.
rem Then open http://127.0.0.1:9222 in Chrome/Edge.
setlocal
set "NCM="
for %%D in ("C:\Program Files\NetEase\CloudMusic" "C:\Program Files (x86)\NetEase\CloudMusic") do (
  if exist "%%~D\cloudmusic.exe" set "NCM=%%~D"
)
if not defined NCM (
  echo [!] NetEase Cloud Music not found in the default locations.
  set /p NCM=Enter install dir:
)
start "" "%NCM%\cloudmusic.exe" --remote-debugging-port=9222 --remote-allow-origins=*
echo Started with DevTools: http://127.0.0.1:9222

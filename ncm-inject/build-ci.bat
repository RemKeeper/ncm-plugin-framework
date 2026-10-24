@echo off
rem CI build: assumes the MSVC environment is already configured (INCLUDE/LIB/PATH),
rem e.g. by ilammy/msvc-dev-cmd. No hardcoded toolchain paths.
rem Output: build\msimg32.dll
setlocal enabledelayedexpansion

cd /d "%~dp0"
if not exist build mkdir build

set CFLAGS=/nologo /c /utf-8 /O2 /W3 /GS- /D_CRT_SECURE_NO_WARNINGS
set LIBS=kernel32.lib user32.lib gdi32.lib ws2_32.lib psapi.lib shell32.lib version.lib ole32.lib

echo === toolchain ===
where cl   || goto :fail_env
where ml64 || goto :fail_env
cl 2>&1 | findstr /C:"Microsoft" || goto :fail_env

echo === assemble msimg32 trampolines ===
ml64 /nologo /c /Fo build\proxy_asm.obj src\proxy_msimg32.asm || goto :fail_asm

echo === compile sources ===
for %%F in (util plugins host inject mitm overlay ihook resdump probe_server bcast devtools pet_hook proxy dllmain) do (
  echo   - %%F.c
  cl %CFLAGS% /Fo:build\%%F.obj src\%%F.c || goto :fail_compile
)

echo === link msimg32.dll ===
link /nologo /DLL /OUT:build\msimg32.dll /DEF:src\proxy_msimg32.def ^
     build\util.obj build\plugins.obj build\host.obj build\inject.obj build\mitm.obj ^
     build\overlay.obj build\ihook.obj build\resdump.obj build\probe_server.obj ^
     build\bcast.obj build\devtools.obj build\pet_hook.obj build\proxy.obj build\proxy_asm.obj ^
     build\dllmain.obj %LIBS% || goto :fail_link

if not exist build\msimg32.dll goto :fail_link
echo.
echo [OK] build\msimg32.dll
dir /b build
endlocal
exit /b 0

:fail_env
echo [x] MSVC environment not found (cl / ml64 not on PATH)
exit /b 1
:fail_asm
echo [x] ml64 failed
exit /b 1
:fail_compile
echo [x] cl failed
exit /b 1
:fail_link
echo [x] link failed
exit /b 1

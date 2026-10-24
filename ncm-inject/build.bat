@echo off
rem Build ncm-inject: the msimg32 proxy DLL that installs the CEF injection layer.
rem Same toolchain strategy as probe/build.bat: set INCLUDE/LIB/PATH explicitly
rem instead of calling vcvars64 (vswhere is broken on this box).
rem Output: build\msimg32.dll
setlocal

set "VCT=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207"
set "SDK=C:\Program Files (x86)\Windows Kits\10"
set "SDKV=10.0.26100.0"

set "PATH=%VCT%\bin\Hostx64\x64;%PATH%"
set "INCLUDE=%VCT%\include;%SDK%\Include\%SDKV%\ucrt;%SDK%\Include\%SDKV%\um;%SDK%\Include\%SDKV%\shared;%SDK%\Include\%SDKV%\winrt"
set "LIB=%VCT%\lib\x64;%SDK%\Lib\%SDKV%\ucrt\x64;%SDK%\Lib\%SDKV%\um\x64"

cd /d "%~dp0"
if not exist build mkdir build

set CFLAGS=/nologo /c /utf-8 /O2 /W3 /GS- /D_CRT_SECURE_NO_WARNINGS
set LIBS=kernel32.lib user32.lib gdi32.lib ws2_32.lib psapi.lib shell32.lib version.lib ole32.lib

echo === toolchain ===
where cl
where ml64
if errorlevel 1 goto :fail_env

echo === assemble MSIMG32 trampolines ===
rem NOTE: output must NOT be build\proxy.obj -- src\proxy.c also compiles to that
rem name and would clobber the assembler output (cost us one confusing LNK2001).
ml64 /nologo /c /Fo build\proxy_asm.obj src\proxy_msimg32.asm
if errorlevel 1 goto :fail_ml

echo === compile sources ===
for %%F in (util plugins host inject mitm overlay ihook resdump probe_server bcast devtools proxy dllmain) do (
  echo   - %%F.c
  cl %CFLAGS% /Fo:build\%%F.obj src\%%F.c
  if errorlevel 1 goto :fail_cl
)

echo === link msimg32.dll ===
link /nologo /DLL /OUT:build\msimg32.dll /DEF:src\proxy_msimg32.def ^
     build\util.obj build\plugins.obj build\host.obj build\inject.obj build\mitm.obj build\overlay.obj build\ihook.obj build\resdump.obj build\probe_server.obj build\bcast.obj build\devtools.obj build\proxy.obj build\proxy_asm.obj build\dllmain.obj ^
     %LIBS%
if errorlevel 1 goto :fail_link

echo.
echo [OK] artifacts:
dir /b build
endlocal
exit /b 0

:fail_env
echo [x] toolchain not on PATH
exit /b 1
:fail_ml
echo [x] ml64 failed
exit /b 1
:fail_cl
echo [x] cl failed
exit /b 1
:fail_link
echo [x] link failed
exit /b 1

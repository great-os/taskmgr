@echo off
setlocal EnableDelayedExpansion
cd /d "%~dp0"

set "SRC=src\taskmgr.c"
set "OUT=taskmgr.exe"
set "OBJDIR=obj"
if not exist "%OBJDIR%" mkdir "%OBJDIR%"

set "GCC="
if exist "C:\msys64\ucrt64\bin\gcc.exe" set "GCC=C:\msys64\ucrt64\bin\gcc.exe"
if not defined GCC (
  where gcc >nul 2>&1 && set "GCC=gcc"
)

set "WINDRES="
if exist "C:\msys64\ucrt64\bin\windres.exe" set "WINDRES=C:\msys64\ucrt64\bin\windres.exe"
if not defined WINDRES (
  where windres >nul 2>&1 && set "WINDRES=windres"
)

set "BUILT_MINGW="
set "BUILT_MSVC="
set "BEST="
set "BESTSIZE=999999999"

rem ---- MinGW-w64 (primary; -nostdlib, no CRT) ----
if defined WINDRES if defined GCC (
  "%WINDRES%" -O coff src\app.rc "%OBJDIR%\app_res.o"
  if not errorlevel 1 (
    "%GCC%" -Os -flto -mwindows -ffunction-sections -fdata-sections ^
      -fno-asynchronous-unwind-tables -fno-ident -fno-stack-protector ^
      -mno-stack-arg-probe -fno-exceptions -fno-builtin -Wall ^
      -DUNICODE -D_UNICODE -Isrc %SRC% "%OBJDIR%\app_res.o" ^
      -o "%OBJDIR%\taskmgr_mingw.exe" ^
      -nostdlib -Wl,--gc-sections -Wl,--no-insert-timestamp -Wl,-e,Entry -s ^
      -lkernel32 -luser32 -lshell32 -lcomctl32 -lpsapi
    if not errorlevel 1 set "BUILT_MINGW=1"
  )
)

rem ---- MSVC /NODEFAULTLIB (only if cl + Windows SDK headers exist) ----
set "MSVC_VER="
set "WINSDK="
for /f "delims=" %%i in ('dir /b /ad /o-n "C:\Program Files\Microsoft Visual Studio\2022\*\VC\Tools\MSVC\*" 2^>nul') do (
  if not defined MSVC_VER set "MSVC_VER=%%i"
)
if defined MSVC_VER (
  for /f "delims=" %%i in ('dir /b /ad /o-n "C:\Program Files (x86)\Windows Kits\10\Include\*" 2^>nul') do (
    if not defined WINSDK set "WINSDK=%%i"
  )
)
if defined MSVC_VER if defined WINSDK (
  if exist "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\MSVC\%MSVC_VER%\include\windows.h" (
    set "VCROOT=C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\MSVC\%MSVC_VER%"
    set "SDKROOT=C:\Program Files (x86)\Windows Kits\10"
    set "PATH=!VCROOT!\bin\Hostx64\x64;!PATH!"
    set "INCLUDE=!VCROOT!\include"
    set "LIB=!VCROOT!\lib\x64;!SDKROOT!\Lib\%WINSDK%\um\x64;!SDKROOT!\Lib\%WINSDK%\ucrt\x64"
    cl /nologo /O1 /GL /GS- /Zl /DUNICODE /D_UNICODE /Isrc %SRC% ^
      /Fo"%OBJDIR%\taskmgr_msvc.obj" /Fe"%OBJDIR%\taskmgr_msvc.exe" ^
      /link /NODEFAULTLIB /ENTRY:Entry /SUBSYSTEM:WINDOWS,6.0 ^
      /OPT:REF /OPT:ICF /MERGE:.rdata=.data ^
      kernel32.lib user32.lib shell32.lib comctl32.lib psapi.lib >nul 2>&1
    if not errorlevel 1 set "BUILT_MSVC=1"
  )
)

rem ---- pick smaller binary ----
if defined BUILT_MINGW if exist "%OBJDIR%\taskmgr_mingw.exe" (
  for %%F in ("%OBJDIR%\taskmgr_mingw.exe") do (
    echo MinGW: %%~zF bytes
    if %%~zF LSS !BESTSIZE! (
      set "BEST=%OBJDIR%\taskmgr_mingw.exe"
      set "BESTSIZE=%%~zF"
    )
  )
)
if defined BUILT_MSVC if exist "%OBJDIR%\taskmgr_msvc.exe" (
  for %%F in ("%OBJDIR%\taskmgr_msvc.exe") do (
    echo MSVC:  %%~zF bytes
    if %%~zF LSS !BESTSIZE! (
      set "BEST=%OBJDIR%\taskmgr_msvc.exe"
      set "BESTSIZE=%%~zF"
    )
  )
)

if not defined BEST (
  echo Build failed: no toolchain produced an output.
  exit /b 1
)

copy /Y "%BEST%" "%OUT%" >nul
if errorlevel 1 (
  echo Failed to copy %BEST% to %OUT%
  exit /b 1
)
for %%F in ("%OUT%") do echo Built %%~nxF: %%~zF bytes
exit /b 0

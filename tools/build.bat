@echo off
rem ============================================================
rem  AmbientVoiceSubtitles -- NPC随口台词头顶字幕
rem  Build AmbientVoiceSubtitles.asi from src\AmbientVoiceSubtitles.c
rem
rem  Two ways, either is fine:
rem    A) zig cc   -- no Visual Studio needed, just download zig
rem                   https://ziglang.org/download/  (unzip, add to PATH)
rem    B) MSVC cl  -- run this file from a "x64 Native Tools Command
rem                   Prompt for VS" so cl.exe is on PATH
rem
rem  The script uses zig if it finds it, otherwise MSVC.
rem  Regenerate src\ini_template.h first if you edited the .ini:
rem      python tools\gen_ini_template.py
rem ============================================================
setlocal
cd /d "%~dp0.."
if not exist build mkdir build

if not exist src\ini_template.h (
  echo [ERROR] src\ini_template.h is missing.
  echo         Run:  python tools\gen_ini_template.py
  exit /b 1
)

where zig >nul 2>nul
if %errorlevel%==0 (
  echo [build] zig cc
  zig cc -shared -O2 -fms-extensions -o build\AmbientVoiceSubtitles.asi src\AmbientVoiceSubtitles.c -lkernel32 -lpsapi
) else (
  where cl >nul 2>nul
  if %errorlevel%==0 (
    echo [build] MSVC cl
    cl /nologo /LD /O2 /EHa /D_CRT_SECURE_NO_WARNINGS src\AmbientVoiceSubtitles.c ^
       /Fe:build\AmbientVoiceSubtitles.asi /link kernel32.lib psapi.lib
  ) else (
    echo [ERROR] Neither zig nor cl was found on PATH.
    echo         Install zig ^(https://ziglang.org/download/^) or open the
    echo         x64 Native Tools Command Prompt for Visual Studio.
    exit /b 1
  )
)

if not exist build\AmbientVoiceSubtitles.asi (
  echo [ERROR] build failed, no output
  exit /b 1
)
echo [ok] build\AmbientVoiceSubtitles.asi
endlocal

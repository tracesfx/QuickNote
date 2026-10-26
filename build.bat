@echo off
rem Builds build\QuickNote.exe with MSVC (Visual Studio 2022 or Build Tools, "Desktop development with C++").
setlocal
cd /d "%~dp0"

if defined VSCMD_VER goto build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
  echo Visual Studio C++ build tools not found.
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

:build
if not exist build mkdir build

rem /O1 /GS- /GR- /EHs-c-: optimise for size, drop security cookies, RTTI and exceptions.
rem /MT + ucrt.lib ("hybrid CRT"): the small VC runtime is linked in, while the C runtime comes from
rem ucrtbase.dll, which ships with Windows 10/11. No Visual C++ Redistributable is needed.
rem Keep these flags unchanged: Smart App Control verdicts depend on the exact build output.
cl /nologo /utf-8 /W4 /O1 /GS- /GR- /EHs-c- /MT /Gy /Gw /DUNICODE /D_UNICODE main.cpp ^
   /Fo:build\ /Fe:build\QuickNote.exe ^
   /link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /MANIFEST:EMBED /MANIFESTINPUT:quicknote.manifest ^
   /NODEFAULTLIB:libucrt.lib ucrt.lib
if errorlevel 1 exit /b 1
del build\*.obj 2>nul
for %%f in (build\QuickNote.exe) do echo Built %%~ff (%%~zf bytes)

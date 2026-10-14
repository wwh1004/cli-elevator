@echo off
rem ============================================================================
rem  cli-elevator build script
rem
rem    build.bat          build bin\cli-elevator.x64.exe and bin\cli-elevator.x86.exe
rem    build.bat clean    remove build\ and bin\
rem
rem  Uses vswhere to find the Visual C++ toolset; if you are already in a
rem  Developer Command Prompt the environment is used as is.
rem ============================================================================
setlocal

set "ROOT=%~dp0"
set "SRC=%ROOT%src"
set "OBJ=%ROOT%build"
set "BIN=%ROOT%bin"

if /i "%~1"=="clean" (
    if exist "%OBJ%" rmdir /s /q "%OBJ%"
    if exist "%BIN%" rmdir /s /q "%BIN%"
    echo [build] cleaned
    exit /b 0
)

if not exist "%OBJ%" mkdir "%OBJ%"
if not exist "%BIN%" mkdir "%BIN%"

rem --------------------------------------------------------------- toolchain --
rem Note: %ProgramFiles(x86)% is copied into a plain variable first. Spelled out
rem inside an if block or a for list its parentheses close the block early and
rem cmd produces nonsense.

set "PF86=%ProgramFiles(x86)%"
set "VSWHERE="
if exist "%PF86%\Microsoft Visual Studio\Installer\vswhere.exe" set "VSWHERE=%PF86%\Microsoft Visual Studio\Installer\vswhere.exe"
if not defined VSWHERE if exist "%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"

if defined VCINSTALLDIR set "VCVARS=%VCINSTALLDIR%Auxiliary\Build\vcvarsall.bat"
if defined VCVARS goto :have_toolset
if not defined VSWHERE goto :no_toolset

set "VSPATHFILE=%TEMP%\cli-elevator-vswhere.txt"
"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%VSPATHFILE%" 2>nul

set "VSPATH="
set /p VSPATH=<"%VSPATHFILE%"
del "%VSPATHFILE%" >nul 2>&1

if not defined VSPATH goto :no_toolset
set "VCVARS=%VSPATH%\VC\Auxiliary\Build\vcvarsall.bat"

:have_toolset
if not exist "%VCVARS%" goto :no_toolset
echo [build] toolset: %VCVARS%

set "CFLAGS=/nologo /W4 /O2 /MT /GS /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS"
set "LDFLAGS=/SUBSYSTEM:CONSOLE /INCREMENTAL:NO"

rem appinfo64.c and appinfo32.c both carry their own arch guard, only the
rem matching one produces code.
set "SOURCES=%SRC%\main.c %SRC%\broker.c %SRC%\uacme.c %SRC%\util.c %SRC%\appinfo\x64\appinfo64.c %SRC%\appinfo\x86-32\appinfo32.c"

rem ------------------------------------------------------------------- x64 ----
echo [build] cli-elevator.x64.exe
call "%VCVARS%" x64 >nul
if errorlevel 1 exit /b 1
pushd "%OBJ%"
if not exist x64 mkdir x64
pushd x64
cl %CFLAGS% /Fe:"%BIN%\cli-elevator.x64.exe" /Fd:"%BIN%\cli-elevator.x64.pdb" %SOURCES% /link %LDFLAGS% /MACHINE:X64 rpcrt4.lib ntdll.lib advapi32.lib user32.lib
set "RC=%ERRORLEVEL%"
popd
popd
if not "%RC%"=="0" exit /b %RC%

rem ------------------------------------------------------------------- x86 ----
echo [build] cli-elevator.x86.exe
call "%VCVARS%" x86 >nul
if errorlevel 1 exit /b 1
pushd "%OBJ%"
if not exist x86 mkdir x86
pushd x86
cl %CFLAGS% /Fe:"%BIN%\cli-elevator.x86.exe" /Fd:"%BIN%\cli-elevator.x86.pdb" %SOURCES% /link %LDFLAGS% /MACHINE:X86 rpcrt4.lib ntdll.lib advapi32.lib user32.lib
set "RC=%ERRORLEVEL%"
popd
popd
if not "%RC%"=="0" exit /b %RC%

echo [build] done
dir /b "%BIN%"
exit /b 0

:no_toolset
echo [build] Visual C++ toolset not found - run this from a Developer Command Prompt.
exit /b 1

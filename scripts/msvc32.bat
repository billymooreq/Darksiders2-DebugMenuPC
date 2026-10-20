@echo off
rem Sets up MSVC 2022 (Build Tools or Visual Studio) for an x86 (32-bit) target and runs the given command.
rem Usage: msvc32.bat <command...>   e.g. msvc32.bat cl /nologo /LD foo.c
rem VCVARS = full path to vcvarsamd64_x86.bat (optional; otherwise found with vswhere or the default Build Tools path).
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
if not defined VCVARS if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvarsamd64_x86.bat"
if not defined VCVARS set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsamd64_x86.bat"
if not exist "%VCVARS%" (echo msvc32: "%VCVARS%" not found - install MSVC 2022 x86/x64 tools or set VCVARS 1>&2 & exit /b 2)
call "%VCVARS%" >nul
%*

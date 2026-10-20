@echo off
rem Common settings for the .bat runners: no absolute paths in the repo.
rem   W            = repo root as an 8.3 short path (Ghidra rejects spaces in project paths and VMARGS)
rem   DS2_GAME_DIR = folder with Darksiders2.exe (default: <repo>\..\Darksiders II)
rem   GHIDRA_HOME  = Ghidra install (default: <repo>\tools\ghidra_12.1.3_PUBLIC)
rem   JAVA_HOME    = JDK 21 (default: <repo>\tools\jdk-21.0.12.1+1 if present)
rem Each value: environment variable -> <repo>\local_paths.ini (KEY=VALUE lines, not in git) -> default.
for %%I in ("%~dp0..") do set "W=%%~fsI"
if exist "%W%\local_paths.ini" for /f "usebackq eol=# tokens=1,* delims==" %%a in ("%W%\local_paths.ini") do if not defined %%a set "%%a=%%b"
if not defined DS2_GAME_DIR set "DS2_GAME_DIR=%W%\..\Darksiders II"
if exist "%DS2_GAME_DIR%" for %%I in ("%DS2_GAME_DIR%") do set "DS2_GAME_DIR=%%~fsI"
if not defined GHIDRA_HOME set "GHIDRA_HOME=%W%\tools\ghidra_12.1.3_PUBLIC"
if exist "%W%\tools\jdk-21.0.12.1+1\bin\java.exe" set "JAVA_HOME=%W%\tools\jdk-21.0.12.1+1"
exit /b 0

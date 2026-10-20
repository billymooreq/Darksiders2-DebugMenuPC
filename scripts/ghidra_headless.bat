@echo off
rem Portable Ghidra headless runner for the PC project <repo>\ghidra_project\ds2pc.
rem Usage: ghidra_headless.bat <args for analyzeHeadless after the project spec>
call "%~dp0env.bat"
if not exist "%GHIDRA_HOME%\support\analyzeHeadless.bat" (echo ghidra_headless: Ghidra not found at "%GHIDRA_HOME%" - set GHIDRA_HOME 1>&2 & exit /b 2)
if defined JAVA_HOME set "PATH=%JAVA_HOME%\bin;%PATH%"
if not defined GHIDRA_HEADLESS_MAXMEM set "GHIDRA_HEADLESS_MAXMEM=12G"
if not exist "%W%\ghidra_project" mkdir "%W%\ghidra_project"
call "%GHIDRA_HOME%\support\analyzeHeadless.bat" "%W%\ghidra_project" ds2pc %*

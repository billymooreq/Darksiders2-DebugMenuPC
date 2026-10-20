@echo off
rem Run postScripts on a local copy of the Xbox 360 DEBUG Ghidra project: <repo>\ghidra_xbox\ds2dbg (not in git).
rem Usage: ghidra_xbox.bat <script args...>
call "%~dp0env.bat"
if not exist "%GHIDRA_HOME%\support\analyzeHeadless.bat" (echo ghidra_xbox: Ghidra not found at "%GHIDRA_HOME%" - set GHIDRA_HOME 1>&2 & exit /b 2)
if not exist "%W%\ghidra_xbox\ds2dbg.gpr" (echo ghidra_xbox: "%W%\ghidra_xbox\ds2dbg.gpr" not found 1>&2 & exit /b 2)
if defined JAVA_HOME set "PATH=%JAVA_HOME%\bin;%PATH%"
if not defined GHIDRA_HEADLESS_MAXMEM set "GHIDRA_HEADLESS_MAXMEM=6G"
call "%GHIDRA_HOME%\support\analyzeHeadless.bat" "%W%\ghidra_xbox" ds2dbg -process DarksidersDebug.xex -noanalysis -scriptPath "%W%\scripts\ghidra" %* < nul

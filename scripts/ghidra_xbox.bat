@echo off
rem Run postScripts on OUR COPY of the Xbox DEBUG Ghidra project (ghidra_xbox\ds2dbg, copied from the Xbox project).
rem Usage: ghidra_xbox.bat <script args...>
set "W=<repo>"
set "JAVA_HOME=%W%\tools\jdk-21.0.12.1+1"
set "PATH=%JAVA_HOME%\bin;%PATH%"
if not defined GHIDRA_HEADLESS_MAXMEM set "GHIDRA_HEADLESS_MAXMEM=6G"
call "%W%\tools\ghidra_12.1.3_PUBLIC\support\analyzeHeadless.bat" "%W%\ghidra_xbox" ds2dbg -process DarksidersDebug.xex -noanalysis -scriptPath "%W%\scripts\ghidra" %* < nul

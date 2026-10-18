@echo off
rem Portable Ghidra headless runner. Usage: ghidra_headless.bat <args for analyzeHeadless after project spec>
rem 8.3 short path: Ghidra rejects spaces in project paths and VMARGS.
set "W=<repo>"
set "JAVA_HOME=%W%\tools\jdk-21.0.12.1+1"
set "PATH=%JAVA_HOME%\bin;%PATH%"
if not defined GHIDRA_HEADLESS_MAXMEM set "GHIDRA_HEADLESS_MAXMEM=12G"
call "%W%\tools\ghidra_12.1.3_PUBLIC\support\analyzeHeadless.bat" "%W%\ghidra_project" ds2pc %*

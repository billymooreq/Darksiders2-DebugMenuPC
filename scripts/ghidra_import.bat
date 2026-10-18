@echo off
rem Import + auto-analyze Darksiders2.exe (PC 2012) headless, log to ghidra_project\analyze.log
set "W=<repo>"
call "%W%\scripts\ghidra_headless.bat" -import "<game dir>\Darksiders2.exe" -overwrite -scriptPath "%W%\scripts\ghidra" < nul > "%W%\ghidra_project\analyze.log" 2>&1

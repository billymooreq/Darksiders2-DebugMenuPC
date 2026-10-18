@echo off
rem Run postScripts on the analyzed PC program (no re-analysis). Usage: ghidra_run.bat <script args...>
set "W=<repo>"
call "%W%\scripts\ghidra_headless.bat" -process Darksiders2.exe -noanalysis -scriptPath "%W%\scripts\ghidra" %* < nul

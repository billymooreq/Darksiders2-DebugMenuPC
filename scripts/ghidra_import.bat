@echo off
rem Import + auto-analyze Darksiders2.exe (PC 2012) headless into <repo>\ghidra_project\ds2pc (~20 min),
rem log to ghidra_project\analyze.log. The exe is taken from DS2_GAME_DIR (see env.bat).
call "%~dp0env.bat"
if not exist "%DS2_GAME_DIR%\Darksiders2.exe" (echo ghidra_import: "%DS2_GAME_DIR%\Darksiders2.exe" not found - set DS2_GAME_DIR 1>&2 & exit /b 2)
if not exist "%W%\ghidra_project" mkdir "%W%\ghidra_project"
call "%W%\scripts\ghidra_headless.bat" -import "%DS2_GAME_DIR%\Darksiders2.exe" -overwrite -scriptPath "%W%\scripts\ghidra" < nul > "%W%\ghidra_project\analyze.log" 2>&1

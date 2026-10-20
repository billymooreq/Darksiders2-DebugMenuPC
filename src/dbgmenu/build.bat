@echo off
rem Build dbgmenu dinput8.dll v0.12.1 (x86, /MT) into _claudework\build\dinput8.dll
setlocal
set "W=<repo>"
set "S=%W%\src\dbgmenu"
set "T=%W%\src\third_party"
set "OUT=%W%\build"
set "OBJ=%W%\build\obj\dbgmenu"
if not exist "%OBJ%" mkdir "%OBJ%"
call "%W%\scripts\msvc32.bat" cl /nologo /W3 /O2 /MT /EHsc /Zi /LD /DNDEBUG /DIMGUI_IMPL_WIN32_DISABLE_GAMEPAD ^
  /I"%T%\imgui" /I"%T%\imgui\backends" /I"%T%\minhook\include" ^
  "%S%\dllmain.cpp" "%S%\hooks.cpp" "%S%\input.cpp" "%S%\overlay.cpp" "%S%\menu.cpp" "%S%\game.cpp" "%S%\flight.cpp" ^
  "%S%\cursor.cpp" "%S%\triggers.cpp" "%S%\keys.cpp" "%S%\monsters.cpp" "%S%\reflect.cpp" "%S%\world.cpp" ^
  "%T%\imgui\imgui.cpp" "%T%\imgui\imgui_draw.cpp" "%T%\imgui\imgui_tables.cpp" "%T%\imgui\imgui_widgets.cpp" ^
  "%T%\imgui\backends\imgui_impl_dx9.cpp" "%T%\imgui\backends\imgui_impl_win32.cpp" ^
  "%T%\minhook\src\buffer.c" "%T%\minhook\src\hook.c" "%T%\minhook\src\trampoline.c" "%T%\minhook\src\hde\hde32.c" ^
  /Fo"%OBJ%\\" /Fd"%OBJ%\vc.pdb" /Fe"%OUT%\dinput8.dll" ^
  /link /DEF:"%S%\dinput8.def" /DEBUG /OPT:REF /OPT:ICF /PDB:"%OUT%\dinput8.pdb" /IMPLIB:"%OBJ%\dinput8.lib" ^
  /DYNAMICBASE /NXCOMPAT kernel32.lib user32.lib
endlocal

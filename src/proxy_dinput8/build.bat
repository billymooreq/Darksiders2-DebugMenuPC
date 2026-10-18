@echo off
rem Build minimal dinput8.dll proxy (x86) into _claudework\build\proxy_dinput8\
setlocal
set "W=<repo>"
set "OUT=%W%\build\proxy_dinput8"
if not exist "%OUT%" mkdir "%OUT%"
call "%W%\scripts\msvc32.bat" cl /nologo /W4 /O2 /MT /GS- /LD "%W%\src\proxy_dinput8\proxy.c" /Fo"%OUT%\\" /Fe"%OUT%\dinput8.dll" /link /DEF:"%W%\src\proxy_dinput8\proxy.def" /SAFESEH:NO /DYNAMICBASE /NXCOMPAT kernel32.lib
endlocal

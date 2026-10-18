@echo off
rem Sets up MSVC 2022 Build Tools for x86 (32-bit) target and runs the given command.
rem Usage: msvc32.bat <command...>   e.g. msvc32.bat cl /nologo /LD foo.c
set "PATH=%PATH%;C:\Program Files (x86)\Microsoft Visual Studio\Installer"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsamd64_x86.bat" >nul
%*

# tools/: версии и восстановление (папка не в git)

| что | версия | откуда | куда |
|---|---|---|---|
| JDK | Temurin 21.0.12.1+1 (x64 zip) | архив из Xbox-проекта `<xbox project>\tools\dl\jdk21.zip` (оригинал: github.com/adoptium/temurin21-binaries, OpenJDK21U-jdk_x64_windows_hotspot_21.0.12.1_1.zip) | `tools/jdk-21.0.12.1+1/` |
| Ghidra | 12.1.3 PUBLIC (20260817) | `…\tools\dl\ghidra.zip` (оригинал: github.com/NationalSecurityAgency/ghidra releases Ghidra_12.1.3_build) | `tools/ghidra_12.1.3_PUBLIC/` |
| XEXLoaderWV | 12.1.3 | скопирован из Ghidra Xbox-проекта (`Ghidra/Extensions/XEXLoaderWV`) — нужен только чтобы открыть копию Xbox-проекта | `tools/ghidra_12.1.3_PUBLIC/Ghidra/Extensions/XEXLoaderWV/` |
| Python venv | системный Python 3.14.8; pefile 2024.8.26, capstone 5.0.9 | PyPI (`requirements.txt`) | `tools/venv/` |
| MSVC | VS 2022 Build Tools 17.14 (cl 19.44.35229, x86), Windows SDK 10.0.26100 | уже установлены в системе | — (обёртка `scripts/msvc32.bat`) |

Шаги:
1. Распаковать JDK и Ghidra в `tools/`.
2. В `tools/ghidra_12.1.3_PUBLIC/support/launch.properties` строку `#VMARGS=-Dapplication.settingsdir=` заменить на
   `VMARGS=-Dapplication.settingsdir=<repo>/tools/ghidra_settings` (8.3-путь: пробел в пути проекта).
3. `python -m venv tools/venv` и `tools/venv/Scripts/python.exe -m pip install -r requirements.txt`.
4. Ghidra-проект PC: `cmd //c scripts\ghidra_import.bat` (~19 мин). Копия Xbox-проекта: скопировать `<xbox project>\ghidra_project\ds2dbg.{gpr,rep}` в `ghidra_xbox/`.

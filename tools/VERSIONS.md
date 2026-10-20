# tools/: versions and how to restore (the folder is not in git)

| what | version | where from | where to |
|---|---|---|---|
| JDK | Temurin 21.0.12.1+1 (x64 zip) | github.com/adoptium/temurin21-binaries, `OpenJDK21U-jdk_x64_windows_hotspot_21.0.12.1_1.zip` | `tools/jdk-21.0.12.1+1/` (or set `JAVA_HOME`) |
| Ghidra | 12.1.3 PUBLIC (20260817) | github.com/NationalSecurityAgency/ghidra releases, `Ghidra_12.1.3_build` | `tools/ghidra_12.1.3_PUBLIC/` (or set `GHIDRA_HOME`) |
| XEXLoaderWV | for Ghidra 12.1.3 | Ghidra extension for Xbox 360 XEX files — only needed to open an Xbox DEBUG build project | `<ghidra>/Ghidra/Extensions/XEXLoaderWV/` |
| Python venv | Python 3.14; pefile 2024.8.26, capstone 5.0.9 | PyPI (`requirements.txt`) | `tools/venv/` |
| git-filter-repo | a40bce548d2c | PyPI (`pip install git-filter-repo` into the venv) — only for history clean-ups | `tools/venv/` |
| MSVC | VS 2022 Build Tools 17.14 (cl 19.44, x86), Windows SDK 10.0.26100 | installed in the system | — (wrapper `scripts/msvc32.bat`) |

Steps:
1. Unpack the JDK and Ghidra into `tools/` (or point `JAVA_HOME` / `GHIDRA_HOME` at your installs, see `scripts/env.bat`).
2. Optional: keep Ghidra settings inside the repo — in `<ghidra>/support/launch.properties` replace
   `#VMARGS=-Dapplication.settingsdir=` with `VMARGS=-Dapplication.settingsdir=<repo, 8.3 short path>/tools/ghidra_settings`
   (no spaces allowed there).
3. `python -m venv tools/venv` and `tools/venv/Scripts/python.exe -m pip install -r requirements.txt`.
4. PC Ghidra project: `scripts\ghidra_import.bat` (~20 min, needs `DS2_GAME_DIR`). Xbox reference project (optional):
   put your own Ghidra project of the Xbox DEBUG build into `ghidra_xbox/ds2dbg.{gpr,rep}`.

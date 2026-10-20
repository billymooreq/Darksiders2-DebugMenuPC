# dumps/

This folder is for files made **from your own copy of the game** (decompiled code, disassembly, string dumps,
tables). They contain or are derived from the game's code, so they are **not stored in git** (`.gitignore`).
Make them locally:

| file | how |
|---|---|
| `decomp_pc/*.c` | `scripts\ghidra_run.bat -postScript Decomp.java <full path>\dumps\decomp_pc\out.c <addr>...` |
| `propmap_pc.txt` | reflection properties → field offsets: `python scripts\propmap.py > dumps\propmap_pc.txt` (or `propmap.py <name>`) |
| `classes_pc.txt` | reflection classes → parent → descriptor global: `python scripts\pc_classes.py` |
| `detector_vts_pc.txt` | vtables that pass the trigger filter: `python scripts\detector_vts.py > dumps\detector_vts_pc.txt` |
| `pe_*.txt` | PE summary: `python scripts\pe_info.py <exe or dll> dumps\pe_<name>.txt` |
| `strings_pc_*.txt` | `python scripts\strings_dump.py <exe> dumps\strings_pc` |

Run Python as `tools\venv\Scripts\python.exe -I scripts\<script>.py` (see `requirements.txt`).
The game exe is found through `DS2_GAME_DIR` / `DS2_GAME_EXE` (see `scripts\ds2paths.py`).

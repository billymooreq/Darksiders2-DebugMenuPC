# Developer overview (English)

The detailed notes in [`../notes/`](../notes) and the agent memory [`../CLAUDE.md.copy`](../CLAUDE.md.copy) are in Russian.
This page is a short English map of what is where, so you can find the right file and read it with a translator.
Marks used in the notes: **[ПОДТВ]** = confirmed (by a log from the game or by static analysis), **[ГИП]** = hypothesis.

## Project layout

| path | what |
|---|---|
| `src/dbgmenu/` | the mod (`dinput8.dll`), see the module table in the [README](../README.md#how-it-works) |
| `src/proxy_dinput8/` | the first minimal proxy (v0.1) used for reconnaissance; `test_load.c` loads it without the game |
| `src/third_party/` | Dear ImGui 1.91.9, MinHook 1.3.4 (`VERSIONS.md`: source commits, what was taken) |
| `scripts/` | build / Ghidra runners (`*.bat`), Python analysis tools, Ghidra scripts (`ghidra/*.java`), release packer |
| `notes/` | knowledge base (Russian) |
| `dumps/` | local only: files made from your exe ([`dumps/README.md`](../dumps/README.md)) |
| `release/INSTALL.txt` | the player guide that goes into the release zip (`scripts/make_release.py <version>`) |
| `tools/VERSIONS.md` | the portable tools (JDK, Ghidra, Python venv) and how to restore them |

## Notes index

| note | contents |
|---|---|
| [`xbox_pc_map.md`](../notes/xbox_pc_map.md) | Xbox → PC address table, field offset checks, the trigger (detector) filter |
| [`playerfly_xbox.md`](../notes/playerfly_xbox.md) | how `playerFly` works in the Xbox DEBUG build: addresses, pseudo-code, speeds, fields |
| [`camera_teleport_pc.md`](../notes/camera_teleport_pc.md) | PC: StateCamera, view / projection, main loop and frame, teleport |
| [`playerfly_pc.md`](../notes/playerfly_pc.md) | PC flight: render matrix, pause, setPos / yaw, cursor, camera reset, vertical speed, the hero state machine (PlayerFall, "second chance" jump, hover), time control, carrying the hero, detector phantoms (Havok) |
| [`triggers_pc.md`](../notes/triggers_pc.md) | Show triggers: world / EventManager, shapes and rotation, frame matrices hook, projection, HUD pass; trigger info via the game's reflection (class descriptor, property vtables, types); TriggerRegion fields; script links (listener → module → links → WorldLoadModule); map / regions |
| [`monsters_pc.md`](../notes/monsters_pc.md) | Monsters: Xbox `showCharacterInfo` levels, character fields (stats, AI, cooldowns, target), ActorManager, damage indicators option, `toggleMonsterUpdate`, Display toggles |
| [`cheats_pc.md`](../notes/cheats_pc.md) | Cheats: Xbox `playerInvincible` / `killMonsters`, PC invincible flag, the damage path (vtable slots, dispatcher, damage event), boss handling, kill-all |
| [`menu_pc.md`](../notes/menu_pc.md) | the menu engine: tree, command registry `g_commands[]`, item types, adding items / sections / folders, F10 master switch, Keys section and `dbgmenu_keys.ini`, offline tests |
| [`plan_architecture.md`](../notes/plan_architecture.md) | the original architecture plan: proxy, hooks, drawing, menu, flight, triggers |
| [`game_changes.md`](../notes/game_changes.md) | log of changes made in the local game folder (only the mod files) |
| `review_0N_*.md`, `progress_0N.md` | reports of the first tasks (setup, reconnaissance, GitHub, first overlay) |

## Most important facts

- Target: `Darksiders2.exe`, PC 2012, x86, MSVC 2010, base `0x00400000` without ASLR,
  SHA-1 `1402d08920a2343c0cfe261826ade47f008c6d42`, PE TimeDateStamp `0x50241AFA`, SizeOfImage `0x147D000`.
- Methods are `__thiscall`. Handles keep a refcount at `obj+4`; release = `0x00401017`.
- Struct **fields** match the Xbox build, but **vtable slots of WorldObject are 4 bytes lower on PC** (the Game vtable matches) —
  check every slot.
- Game classes have no RTTI; use the game's reflection (class descriptors, properties) — `scripts/pc_classes.py`,
  `scripts/propmap.py`, `scripts/proptypes.py`.
- 1 game unit = 1 cm.
- The game, D3D9 Present and the window procedure all run on one thread.

## Workflow that worked

1. Find the feature in the Xbox DEBUG build (console command → its handler).
2. Find the same code on PC by strings, constants, field offsets and call patterns (`scripts/pcfind.py`, Ghidra).
3. Write it down in a note with [ГИП] / [ПОДТВ] marks.
4. Implement in `src/dbgmenu/`, with a fake of the game objects for the offline test (`test/test_d3d.cpp`).
5. Test in the game, read `dbgmenu.log`, update the note.

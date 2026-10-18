# progress 01 — setup + recon

## Этап 0 — папка и инструменты (2026-10-18) — готово
- `_claudework\{tools,scripts,notes,dumps,src,build,backup,logs}`.
- Компилятор: **MSVC 19.44 (VS 2022 Build Tools 17.14, уже стоит в системе)** + Windows SDK 10.0.26100, цель x86 есть (`Hostx64\x86\cl.exe`). Ничего не ставил. Обёртка `scripts\msvc32.bat <команда>` (vcvarsamd64_x86). Проверено: тестовая DLL собирается, `machine (x86)`. Выбор: родной ABI игры (exe собран VC10, MSVCR100), нет скачиваний, dumpbin/lib/link в комплекте. MinGW/clang в системе нет.
- Python venv `tools\venv` (системный 3.14.8): pefile 2024.8.26, capstone 5.0.9.
- Ghidra 12.1.3 + JDK Temurin 21.0.12.1 — распакованы из архивов Xbox-проекта (`tools\dl` там) в `tools\`, архивы потом удалены. `support\launch.properties`: settingsdir → `tools\ghidra_settings` (8.3-путь). XEXLoaderWV скопирован в свою Ghidra (чтобы открывать копию Xbox-проекта).
- Пробел в пути проекта → раннеры используют 8.3: `<repo>`, игра `<game dir>\Darksiders2.exe`.
- Копия Xbox Ghidra-проекта: `ghidra_xbox\ds2dbg` (873 МБ) — чтобы не писать в оригинал (headless всё равно пишет lock/save). Раннер `scripts\ghidra_xbox.bat`.
- git init в `_claudework`, `.gitignore` (tools, ghidra_project, ghidra_xbox, backup, logs, бинарники сборки, большие дампы).

## Этап 2 — разведка PC-exe — готово
- Скрипты: `scripts\pe_info.py`, `scripts\strings_dump.py`, `scripts\check_cmds.py`. Дампы: `dumps\pe_*.txt`, `dumps\strings_pc_{ascii,utf16}.txt`, `dumps\keyword_hits_pc.txt`, `dumps\console_cmds_in_pc.md`.
- Итог: exe не упакован, VC10, без ASLR (base 0x400000 фиксирован), DX9 (D3D11 только строки/динамическая загрузка), ввод Raw Input + XInput + DirectInput8. Консоль/меню/DebugRenderer вырезаны (0/206 описаний команд). Подробно — в review_01.

## Этап 3 — playerFly на Xbox — готово
- `notes\playerfly_xbox.md`. 0x825B3790 = ctor команды; execute 0x825B42C8; состояние камеры тип 10 (vtable 0x820554D0); телепорт = Actor vt[0x38] setPosition(позиция камеры).

## Этап 4 — сопоставление Xbox→PC — готово
- Ghidra: импорт + автоанализ PC-exe 1149 с (`ghidra_project/ds2pc`, лог `analyze.log`). Многие функции, которые вызываются только через таблицы, Ghidra не создала → `Decomp.java` умеет `!addr` (создать функцию). Основная работа — быстрым x86-поиском `scripts/pcfind.py` (ref/calls/str/dis/dword) + `after_call.py` + `rtti_vt.py`.
- `notes/xbox_pc_map.md`: синглтон игры [0x184E95C] (instance 0x4058CF), getPlayerActor 0x4CDA8F, getCamera 0x745A0F (+0x138), setPaused vt[0x68] 0xB3C287, getEventManager 0x7735E8 (World+0x118), Detector/TriggerRegion vtable, onAdd/onRemove.
- Гипотеза о полях: подтверждена (+0x58/+0x80/+0x90/+0xA0/+0xB0/+0xD0/+0xD4/+0xD8, World+0x118, evm+0x14, Game+0x8/+0x138/+0xF8). Слоты vtable WorldObject на PC на 4 меньше.

## Этап 5 — план — готово
- `notes/plan_architecture.md`: dinput8-прокси + IAT-хуки (Direct3DCreate9, XInputGetState, GetRawInputData) + MinHook для игровых функций + ImGui DX9 для меню + свои линии для триггеров; playerFly — своя камера + setPaused + телепорт.

## Этап 6 — прокси-DLL — готово (офлайн-тест)
- `src/proxy_dinput8/{proxy.c,proxy.def,build.bat,test_load.c}` → `build/proxy_dinput8/dinput8.dll` (x86, 125 КБ, /MT, только KERNEL32).
- Офлайн-тест без игры (`test_load.c` в scratchpad): DLL грузится, DirectInput8Create пробрасывается в системную (hr=0), лог пишется, поток-наблюдатель переживает FreeLibrary (DLL закреплена PIN). В игре не проверялось.

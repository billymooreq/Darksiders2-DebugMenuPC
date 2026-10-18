# Прогресс задачи 03 — DLL v0.2 (меню-оверлей)

Контрольная точка 2026-10-18 ~23:35 (пауза по просьбе пользователя). Коммита ещё нет.

## Сделано
- `src/third_party/`: **Dear ImGui v1.91.9** (commit 97428e8a, core + backends dx9/win32, MIT) и **MinHook v1.3.4** (commit c3fcafdc, только x86/hde32, BSD-2). Склонированы с GitHub (ocornut/imgui, TsudaKageyu/minhook). `third_party/VERSIONS.md` — ЕЩЁ НЕ НАПИСАН.
- `src/dbgmenu/` (новый проект, v0.1 `proxy_dinput8` не трогал): `common.h`, `dllmain.cpp` (прокси + лог `logs/dbgmenu.log`, прошлый запуск → `dbgmenu.prev.log`, проверка exe; хуки только при MATCH или `DBGMENU_TEST=1`), `hooks.cpp`, `input.cpp`, `overlay.cpp`, `menu.cpp`, `game.cpp`, `dinput8.def`, `build.bat`.
- **Собирается без ошибок**: `cmd //c '<repo>\src\dbgmenu\build.bat'` → `build/dinput8.dll` (486 912 байт) + `build/dinput8.pdb`, obj в `build/obj/dbgmenu/`. Ни разу не запускалась (офлайн-теста ещё нет).

## Решения (обосновать в ревью)
- Хук **Present** (+ Present цепочки обмена, защита от двойного кадра счётчиком глубины), а не EndScene: ровно 1 раз за кадр, кадр уже готов. Рисую в бэкбуфер: сохраняю RT0/depth/viewport + StateBlock, свой BeginScene/EndScene, SRGBWRITE=0.
- Direct3DCreate9 — патч IAT exe (слот ищется по имени, сверяется с 0x01155730); CreateDevice/Present/Reset/Release — MinHook на код d3d9.dll из vtable (16 / 17 / 16 / 2; swapchain Present = 3).
- `Release`-хук: ImGui DX9 делает AddRef устройству; когда у устройства остаётся ≤2 ссылки — гашу бэкенд ImGui, чтобы игра могла уничтожить устройство. Reset: Invalidate до, ленивое пересоздание в NewFrame.
- ImGui только рисует: NoMouse/NoKeyboard/NoMouseCursorChange, без imgui.ini, без геймпада → курсор/мышь игры не трогаем (важно для exclusive fullscreen). Шрифт Consolas, размер = высота/45 (14–48 px).
- Ввод: игра регистрирует Raw Input клавиатуры с флагами 0x300/0x2300 (INPUTSINK|NOHOTKEYS[|DEVNOTIFY]) — **без NOLEGACY**, значит WM_KEYDOWN приходят → клавиши меню берём из WndProc (subclass, A/W по IsWindowUnicode). Игра читает Raw Input в своём WndProc (0x00403AF3 → 0x00913C78, единственный вызов GetRawInputData). `GetAsyncKeyState` вызывается только там же (L/R Shift/Ctrl/Alt) → хук не нужен. Фильтр: IAT-хук GetRawInputData — нажатие → «отпускание VK 0xFF», мышь: обнуление движения/колеса/нажатий. Парность нажатие/отпускание (нет залипших клавиш). F1/F2 игре не доходят никогда. F1 открывает меню только после первого кадра оверлея; при исключении в кадре оверлей выключается и меню закрывается.
- Чтение игры (`game.cpp`): `[0x0184E95C]` читаю напрямую (instance() не зову — ленивая инициализация); getPlayerActor 0x004CDA8F, getCamera 0x00745A0F, release 0x00401017 через `__fastcall`; vtable должна быть в образе exe, метод — в .text; каждый шаг в `__try`.

## Найдено статически (этап 3)
- **Камера `game+0x138` = класс StateCamera, vtable 0x01199EA0** (строка «StateCamera» сразу после): `vt[0x3C]` 0x005B5BD6 = матрица 4×4 (копия `cam+0x80`), `vt[0x40]` 0x005B5DB6 = направление (−строка 2), `vt[0x44]` 0x00585FB5 = позиция (строка 3 → `cam+0xB0..B8`). Обновление камеры 0x005B5D33 (= Xbox 0x82585650, там Xbox печатал «Camera: Pos(...)»; флаг showCameraPosition Xbox 0x83793EB0): состояние `cam+0x15C`, `state->vt[0x30]` пишет `cam+0x100`. Сдвиг слотов −4 как у WorldObject. Ещё 3 vtable с тем же getDir: 0x01172AA8, 0x01172B08, 0x0118FE60 (наследники?).
- Xbox Console::draw: префикс текущей строки `"> "`, у пунктов с клавишей `" (shortcut %c)"` — повторено.

## Осталось
1. Офлайн-тест: маленький D3D9-exe (`DBGMENU_TEST=1`, LoadLibrary dinput8.dll, окно, PostMessage F1/стрелки/Enter, Reset, пересоздание устройства, снимок бэкбуфера в BMP→PNG) — проверить, что меню рисуется и лог чистый.
2. `third_party/VERSIONS.md`, `build/README.md` (для игрока: заменить dinput8.dll v0.1), `notes/game_changes.md`.
3. Этап 4 (статика): матрица вида/проекции перед рендером (StateCamera `cam+0x80` уже есть; FOV/проекция — искать), игровой тик, телепорт → `notes/camera_teleport_pc.md`.
4. `review_03_menu_overlay.md` с чек-листом, CLAUDE.md §8/§9 + копия, коммит `03: menu overlay`, тег `review-03`, push.

## 2026-10-19 — офлайн-тест (этап 1–3 закрыты)
- `src/dbgmenu/test/test_d3d.cpp`: скрытое D3D9-окно, `DBGMENU_TEST=1`, клавиши через PostMessage, Reset 1280×720→1024×768, Release устройства и новое 800×600, снимки бэкбуфера (BMP→PNG скриптом). Сборка: `msvc32.bat cl /nologo /EHsc test_d3d.cpp d3d9.lib user32.lib`, запуск из папки с копией dinput8.dll.
- Результат: хуки ставятся (MH_OK ×5), WndProc подменён, меню/подсветка/[on]/значения/подсказки рисуются, «W» при закрытом меню доходит до окна, при открытом — нет; чтение памяти в чужом exe → `exception C0000005` поймано и показано; Reset ок; финальные Release устройства = 0 (раньше было 4).
- Найдена и исправлена ошибка: в D3D9 каждый ресурс ImGui (VB, IB, текстура шрифта) держит ссылку на устройство, плюс AddRef бэкенда; порог «≤2» не работал, и при пересоздании устройства было двойное выключение бэкенда → падение теста. Теперь: Release при счётчике ≤8 → освобождаю ресурсы ImGui, если осталось ≤2 → выключаю бэкенд; свои Release (кадр оверлея, Shutdown/Invalidate, swapchain GetDevice) помечены и не проверяются.
- Также: Release поверхностей бэкбуфера/RT идёт через счётчик устройства (implicit surfaces) — поэтому весь кадр оверлея помечен как «внутренний».

## 2026-10-19 — этап 4 и завершение
- Этап 4 (статика) → `notes/camera_teleport_pc.md`: вид/проекция 0x00925969, камера→рендер 0x00B3CD27, главный цикл 0x00B3D821 → кадр 0x00B3D3E4, телепорт = `vt[0x34]` + `vt[0x138]` (игровой `teleportTo` 0x00796B71). В панель showCameraPosition добавлены адреса setPos/setYaw игрока.
- `src/third_party/VERSIONS.md`, `build/README.md`, `notes/game_changes.md`, `notes/review_03_menu_overlay.md` (с чек-листом), CLAUDE.md §6–9 + копия.
- Итоговая DLL: `build/dinput8.dll` 487 936 байт, SHA-1 597265f110699b4ba77cd010612183d75a2957e1; офлайн-тест пройден повторно.

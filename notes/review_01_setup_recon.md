# Ревью 01: setup + разведка PC-exe + playerFly (Xbox) + карта Xbox→PC

Пути — от `_claudework\`. Адреса PC — VA в `Darksiders2.exe` (base 0x00400000, без ASLR); Xbox — VA в `DarksidersDebug.xex` (base 0x82000000). Журнал по этапам: `notes/progress_01.md`.

## 1. Что сделано
1. **Этап 0.** Папки `tools, scripts, notes, dumps, src, build, backup, logs`. Компилятор — **MSVC 2022 Build Tools (cl 19.44, x86) + Windows SDK 10.0.26100, уже стояли в системе** → ничего не качал; обёртка `scripts/msvc32.bat`. venv (pefile, capstone). Ghidra 12.1.3 + JDK 21 распакованы из архивов Xbox-проекта (`tools/VERSIONS.md`). Копия Xbox Ghidra-проекта → `ghidra_xbox/` (оригинал не трогал). `git init` + `.gitignore`. **Коммит не сделан — жду подтверждения** (общее правило пользователя).
2. **Этап 1.** `CLAUDE.md` в корне, копия `CLAUDE.md.copy`.
3. **Этап 2.** `scripts/pe_info.py`, `strings_dump.py`, `check_cmds.py` → `dumps/pe_*.txt`, `dumps/strings_pc_*.txt`, `dumps/console_cmds_in_pc.md`, `dumps/keyword_hits_pc.txt`.
4. **Этап 3.** `notes/playerfly_xbox.md` (+ декомпиляция `dumps/decomp_xbox/*.c`, ассемблер `flycam_update.dis`).
5. **Этап 4.** Ghidra-проект PC `ghidra_project/ds2pc` (автоанализ 1149 с). Инструменты `scripts/pcfind.py`, `after_call.py`, `rtti_vt.py`, `ghidra/{Decomp,XRefs,StrRefs}.java`. Таблица `notes/xbox_pc_map.md`.
6. **Этап 5.** `notes/plan_architecture.md`.
7. **Этап 6.** Прокси `src/proxy_dinput8/` → `build/proxy_dinput8/dinput8.dll` + `README.md` (инструкция). Офлайн-тест (`test_load.c`, без игры) пройден. В папку игры не копировал.

## 2. Ключевые находки
**PC-exe [ПОДТВ]:** PE32 x86, линкер 10.0 (VC2010, MSVCR100), 4 секции (.text энтропия 6.80, .rdata 5.65, .data 4.00, .rsrc 7.89), EP 0x00DADBCE в .text, TLS нет, overlay нет, **DllCharacteristics 0x8100 — без ASLR (DYNAMICBASE нет)**. Признаков упаковщика/DRM нет. Анализировать как есть можно. PDB-путь `C:\_Vigil\code\ds2\mainline\projects\PC\DarksidersPC\Retail\DarksidersPC.pdb`. exe экспортирует ~400 символов Scaleform и Wwise (AK::) — для нас бесполезны. Версионного ресурса нет; идентификация — SHA-1 `1402d08920a2343c0cfe261826ade47f008c6d42` (совпадает с `C:\Darksiders II\`), TimeDateStamp 0x50241AFA.

**Графика и ввод [ПОДТВ по импортам, ГИП по факту рантайма]:** импорт `d3d9!Direct3DCreate9` + 28 функций D3DX9 → основной рендер **D3D9**. DX11-код скомпилирован (RTTI `dx11Renderer@gfc` и др., строки `d3d11.dll`/`D3D11CreateDevice`/`dxgi.dll` — загрузка динамическая), но шейдерной библиотеки `Darksiders2.dx11sl` в игре нет, есть только DX9 `Darksiders2.dsl` → в работе DX9 [ГИП, проверит лог прокси]. Окно своё (`RegisterClassExA`/`CreateWindowExA`, `SetWindowLongW`/`CallWindowProcW`). Клавиатура/мышь — **Raw Input** (`RegisterRawInputDevices`, `GetRawInputData`) + `GetAsyncKeyState`; геймпад — **XInput 1.3** по ординалам (2 GetState, 3 SetState, 4 GetCapabilities), плюс `DirectInput8Create` и `joyGetPosEx`.

**Остатки отладки [ПОДТВ по строкам]:** из 206 Xbox-команд **ни одного описания** и ни одного «своего» имени (12 совпадений — общие слова вроде `lighting`, `gravity` и имена свойств, например `PlayerInvincible`). Нет `playerFly`, `ConsoleFunction`, `addFunction`, `Console`-класса, `DebugRenderer`, `OblivionGameDebug`, `drawGraphics`, `showCameraPosition`, `demoCam`. Есть: cfg-ключи `[Debug] DeveloperMode`, `DebugCamera`, `EnableVisScriptDebug`, `[Renderer] PhysicsDebugRender`, `PC.Debug`; `Mover::Teleport` (это NavPower-мувер ИИ). `DebugCamera` регистрируется, но его значение никто не читает (ни на PC, ни на Xbox). **Вывод: код меню/консоли/DebugRenderer удалён, остались только cfg-ключи.**

**Кандидаты на прокси:** не-KnownDLL и в статическом импорте: `dinput8`, `d3d9`, `xinput1_3`, `d3dx9_43`, `D3DCOMPILER_43`, `d3dx11_43`, `winmm`, `binkw32`(игровой), `steam_api`(игровой). `version.dll` exe не импортирует. Лучший — **dinput8.dll**: 6 экспортов, грузится на старте, не конфликтует с d3d9-модами; D3D9/XInput ловим через IAT exe (адреса фиксированы).

**playerFly на Xbox [ПОДТВ, подробности — `playerfly_xbox.md`]:** 0x825B3790 — не обработчик, а ctor команды (vtable 0x82052DE0, `+0x68 = 0x46` = клавиша **F**). execute **0x825B42C8**: берёт синглтон игры `[0x8376E9EC]`, актёра игрока (0x8242D9D0) и камеру (`game+0x138`); если уже летим — **телепорт**: `actor->vt[0x13C](yaw по направлению камеры)`, `actor->vt[0x38](позиция камеры)` (= setPosition 0x829AB738: пишет actor+0xF0 и узел сцены); затем переключает состояние камеры (0x82593790: push `FlyCameraState` тип 10, vtable 0x820554D0 / pop) и зовёт `game->vt[0x68](летим?)` = 0x82A67B10 — счётчик +0xF8, по коду это **пауза игры** (персонаж «стоит», потому что мир на паузе) [ГИП-сильная по названию]. Флаги 0x100/0x200 в `0x82A51828()+0x3C`. FlyCameraState: левый стик — полёт по направлению взгляда, `375 ед/с × dt × 2.0`, логическая кнопка 0x18 ×3, 0x19 ×0.5 (и фронт 0x19 — замедление времени), правый стик — обзор (мёртвая зона 0.25, наклон |fwd·up| ≤ 0.95), кнопки 4/5 — FOV. **Отпускание кнопки 0x14 → телепорт + выход; 0x15 → выход без телепорта.** Точка телепорта = ровно XYZ камеры (луча вниз нет). Коды кнопок — логические (раскладка из данных `base/GamepadControls`), физические кнопки статически не установлены.

**Карта Xbox→PC [ПОДТВ, таблица — `xbox_pc_map.md`]:** синглтон игры **[0x0184E95C]** (`instance()` 0x004058CF; найден через RTTI `ConcreteLifetimeTracker<Darksiders@gfc>` → регистратор 0x004080AA, приоритет 0x7F как на Xbox); **getPlayerActor 0x004CDA8F**; **getCamera 0x00745A0F (game+0x138)**; **setPaused = game vt[0x68] 0x00B3C287** (+0xF8, код совпадает); **getEventManager 0x007735E8** (World+0x118, ключ [0x015E7338]); EventManager add/remove 0x00ABDCC8/0x00ABDB8E; DetectorObject vtable **0x01269E40** (+скрипт 0x0126A040), onAdd 0x008B84FE, onRemove 0x008B888B; TriggerRegion vtable **0x012702A0** (+0x012703F0), onAdd 0x008C8E47; Actor onAdd/onRemove 0x00A40971/0x00A40A30.

**Гипотеза о смещениях:** поля **совпадают** [ПОДТВ]: Detector +0x58 World*, +0x80 Shape (switch 0..3), +0x90 Pos, +0xA0 Rot, +0xB0 Scale (·0.5), +0xD0 BodyType (=0x0B в ctor), +0xD4 тело, +0xD8 enabled (=1 в ctor); World+0x118; evm+0x14 {head,tail,count}, узел {next,obj}; Game +0x8/+0x10/+0xF8/+0x138. **Слоты vtable в иерархии WorldObject на PC на 4 меньше** (getPos 0x38, getRot 0x40, getScale 0x48, onAdd 0x50, onRemove 0x54; у локального игрока vt[0x6C]→0x68); у Game/OblivionGame — совпадают.

## 3. Что не сделано или сделано иначе
- **Коммит `01: setup + recon` не сделан**: общее правило пользователя требует показать план коммита и получить подтверждение. Всё подготовлено (`git status` чистый по составу).
- Не найдено на PC: объект/матрица вида камеры (слоты камеры), игровой тик (аналог 0x82A6A780), правильный телепорт актёра. На PC у всех 25 vtable актёров `setPosition` (vt 0x34 = 0x00A7F942) — базовый, пишет только +0x90; Xbox-актёр переопределял его (физика/узел). Кандидаты на PC: скриптовые биндинги `setPosition` (0x0117CE98, 5 мест), `TeleportHelper`, `teleportTo` — задача 03/04.
- Физические кнопки для кодов 0x14/0x15/0x18/0x19/4/5 не определены (раскладка в данных).
- Классы 11 прочих vtable детекторов на PC не подписаны (LoadRegion, CameraInfluence и т.д.) — для отрисовки не нужно (общий фильтр), для цветов по классам — позже.
- Ghidra не создала функции, вызываемые только через таблицы (много «no function») — доработал `Decomp.java` (`!addr`) и основную часть сделал своим x86-поиском.
- Копию Xbox Ghidra-проекта сделал вместо `-readOnly` (headless всё равно пишет lock/сохранение) — по правилу «если нужна запись — копия».

## 4. Допущения и решения
- Компилятор MSVC (родной ABI игры, всё уже в системе) вместо llvm-mingw.
- Прокси-DLL: /MT (без зависимостей от VC-рантайма), только KERNEL32, DLL закреплена (PIN), лог закрывается после каждой строки. Лог — в `..\_claudework\logs\` относительно папки игры, иначе рядом с exe.
- Прокси пишет, какие модули загружены на старте, через 15 и 60 с (d3d9/d3d11/dxgi/xinput/…) — ответ на вопрос DX9 vs DX11. Больше ничего не делает.
- Пауза Xbox `vt[0x68]` названа setPaused по коду (счётчик + рассылка подсистемам); это вывод, а не символ.

## 5. Проблемы и риски
- `setPosition` на PC может не двигать физическое тело персонажа → телепорт придётся делать иначе (риск средний).
- Если PC-рендер на паузе не обновляет вид, подход «пауза + своя камера» потребует замораживать только ввод игрока.
- Steam-библиотеки игры: возможны конфликты с хуками ввода/окна (не проверено).
- DLL в папке игры может ловить антивирус (неподписанная) — маловероятно.
- Объём: `tools/` ≈1.2 ГБ, `ghidra_project` 305 МБ, `ghidra_xbox` 873 МБ (вне git).

## 6. Технические данные
- Инструменты: MSVC 19.44.35229 (VS Build Tools 17.14.37710), SDK 10.0.26100; Python 3.14.8 (pefile 2024.8.26, capstone 5.0.9); Ghidra 12.1.3, JDK Temurin 21.0.12.1+1, XEXLoaderWV 12.1.3; git (системный).
- 8.3-пути: `_claudework` = `<repo>`, игра = `<game dir>`.
- PC IAT: Direct3DCreate9 0x01155730, XInputGetState(ord2) 0x011556D0, XInputSetState(ord3) 0x011556C8, XInputGetCapabilities(ord4) 0x011556CC, DirectInput8Create 0x01155028.
- PC: Darksiders [0x0184E95C] / объект 0x0184F720 / vtable 0x0116E230; instance 0x004058CF; init 0x00406441; getPlayerActor 0x004CDA8F; game+8 getter 0x00902F25; getCamera 0x00745A0F; setPaused 0x00B3C287; release 0x00401017; getEventManager 0x007735E8; EM key [0x015E7338]; EM add/remove 0x00ABDCC8/0x00ABDB8E; Detector vt 0x01269E40/0x0126A040, ctor ~0x008B7C2A, onAdd 0x008B84FE, onRemove 0x008B888B; getPos/getRot/getScale 0x008EF6B2/0x00A24AFB/0x00A24B3D; WorldObject::setPosition 0x00A7F942; TriggerRegion vt 0x012702A0/0x012703F0, onAdd 0x008C8E47; Actor onAdd/onRemove 0x00A40971/0x00A40A30; cfg-var ctor 0x00DA8270 (`{type, value, section, key, next}`), `DebugCamera` var 0x014D2534; float 0.03048 @0x011B97D4, helper 0x00639CC2.
- Xbox (новое): PlayerFly ctor 0x825B3790, vtable 0x82052DE0, execute 0x825B42C8; isFly 0x825914A0; toggle 0x82593790; FlyCameraState ctor 0x825BF0C0, vtable 0x820554D0, input 0x825BF1C8, update 0x825BF7F8, exitCheck 0x825C06E8, onEnter 0x825C0790, onExit 0x825BFC10; camera state push/pop 0x82584908/0x825926E8, getType via state(+0x15C)->vt[0x40]; InputMgr [0x837827F4]; Game::setPaused 0x82A67B10; Actor setPosition 0x829AB738, setYaw 0x82531290; yaw-from-dir 0x82EDDAB8; Darksiders vtable 0x8201A2A8.
- Прокси: `build/proxy_dinput8/dinput8.dll`, 125 440 байт, экспорт DirectInput8Create(@1), DllCanUnloadNow, DllGetClassObject, DllRegisterServer, DllUnregisterServer, GetdfDIJoystick(@6).

## 7. Предложение следующего шага
1. Пользователь проверяет прокси в игре (README в `build/proxy_dinput8/`), присылает лог.
2. **Задача 02:** прокси + MinHook (исходники в `src/third_party`) + IAT-хук `Direct3DCreate9` → хук `Present`/`Reset` → **ImGui (DX9+Win32)** «пустое» меню в Xbox-стиле, открытие клавишей (F1 или `), блокировка ввода игры (WndProc/`GetRawInputData`/`XInputGetState`), строка «позиция игрока/камеры» (через 0x004058CF → 0x004CDA8F → vt[0x38] getPos) — это сразу проверит карту адресов в игре.
3. Задача 03: найти матрицу вида у камеры (`game+0x138`) и правильный телепорт; 04 — playerFly; 05 — триггеры через EventManager.

## 8. Вопросы ко мне
1. **Разрешите коммит** в `_claudework` одним коммитом `01: setup + recon` (состав — всё, кроме tools/ghidra/logs/backup и .obj/.lib/.exp; включая `build/proxy_dinput8/dinput8.dll` 125 КБ)?
2. Проверите прокси в игре и пришлёте `_claudework\logs\proxy_dinput8.log`? Важно: запустилась ли игра как обычно, строки `exe identity`, `modules (+60 s)`.
3. Клавиши: какую клавишу открывать меню (предлагаю F1 или `), какую для телепорта на клавиатуре (на Xbox: F — включить/выключить полёт; телепорт — кнопкой геймпада)? Играете с геймпадом или с клавиатурой?
4. Ок ли MinHook (BSD-2) и Dear ImGui (MIT) как сторонний код в `src/third_party`?

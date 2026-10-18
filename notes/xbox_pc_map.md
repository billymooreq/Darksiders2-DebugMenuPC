# Карта соответствий Xbox DEBUG → PC 2012

Xbox — `DarksidersDebug.xex` (PPC BE, base 0x82000000). PC — `Darksiders2.exe` (x86, base 0x00400000, **без ASLR**, адреса абсолютные).
Уверенность: **В** — высокая (совпала структура кода и константы), **С** — средняя, **Н** — догадка. Инструменты: `scripts/pcfind.py` (ref/calls/str/dis/dword), `scripts/after_call.py`, `scripts/rtti_vt.py`, Ghidra-проект `ghidra_project/ds2pc`, декомпиляция `dumps/decomp_pc/*.c`.

## Соглашения вызова на PC
- Методы — `__thiscall` (this в ECX, аргументы на стеке, callee чистит). Возврат структуры/handle — скрытый указатель первым стековым аргументом (как `out` в r3 на Xbox).
- Handle с refcount: `obj+4` — счётчик; release = **0x00401017** (thiscall, ECX=obj) ⇔ Xbox `0x82380000`.

## Таблица
| что | Xbox | PC | уверен. | как найдено |
|---|---|---|---|---|
| синглтон игры `Darksiders` (gfc::OblivionGame) — указатель | `[0x8376E9EC]` | **`[0x0184E95C]`** | В | RTTI `ConcreteLifetimeTracker<Darksiders@gfc>` → vtable 0x013D57DC → регистратор 0x004080AA → единственный вызов в 0x00406441 (приоритет 0x7F, как на Xbox) |
| ленивая инициализация | 0x82382D88 | 0x00406441 | В | то же |
| `Darksiders::instance()` (init + return) | inline | **0x004058CF** | В | читает [0x184E95C]; 10+ сотен вызовов |
| статический объект Darksiders | 0x8376DD80 | 0x0184F720 | В | ctor-обёртка 0x00406B29 → ctor 0x004CE4BA |
| vtable Darksiders | 0x8201A2A8 | 0x0116E230 | В | ctor; слоты 0x00–0xE0 совпадают по смыслу (на Xbox +1 слот в конце 0xE4) |
| `Game::setPaused(bool)` (счётчик +0xF8) — то, что playerFly зовёт как `S->vt[0x68]` | vt[0x68] = 0x82A67B10 | **vt[0x68] = 0x00B3C287** | В | тот же код: бит 6 в +0x10, счётчик +0xF8, `X->vt[0x60/0x64]`, +0x18 |
| `getPlayerActor(out, game)` | 0x8242D9D0 | **0x004CDA8F** (thiscall, ECX=game) | В | `game+8` (0x00902F25 = get handle +8 с addref) → `vt[0x68](out)` (на Xbox vt[0x6C]) |
| `getCamera(out, game)` = `game+0x138` | 0x82749DE0 | **0x00745A0F** | В | `mov eax,[ecx+0x138]` + addref; вызывается после `instance()` в тех же местах (onShowMerchant и др.) |
| DetectorObject vtable | 0x82114980 / скрипт 0x82114BC0 | **0x01269E40 / 0x0126A040** | В | ctor 0x008B7C2A пишет vtable, `+0xD0 = 0x0B`, `+0xD8 = 1` |
| DetectorObject::onAddToWorld | 0x8292FA88 (vt 0x54) | **0x008B84FE (vt 0x50)** | В | switch по +0x80, ·0.5 к +0xB0..B8, тело в +0xD4, регистрация в EventManager |
| DetectorObject::onRemoveFromWorld | 0x8292FE30 (vt 0x58) | **0x008B888B (vt 0x54)** | В | `[this+0x58]`→getEM→remove; удалить тело +0xD4 |
| TriggerRegion vtable | 0x82084B88 / 0x82084CE0 | **0x012702A0 / 0x012703F0** | В | свой onAdd 0x008C8E47 (vt 0x50), ctor 0x008C84FD / 0x008C8A4A рядом с регистрацией класса «TriggerRegion» (0x008C9184) |
| наследники TriggerRegion (onAdd 0x008C8E47) | — | 0x01228A50/0x01228E10 (KillRegionTrigger?), 0x0125BAB0, 0x0125BBF0 | С | по общему onAdd; имена — по соседним строкам |
| прочие детекторы (onAdd 0x008B84FE) | LoadRegion, CameraInfluence… | 0x011FA6A0, 0x011FA808, 0x011FA940, 0x011FAA78, 0x012193F0, 0x0123FBA8, 0x0123FCF0, 0x0125C6F8, 0x0125C8C0, 0x01266F98, 0x012670D0 | С | 13 vtable с onAdd детектора; классы не подписаны |
| SimpleShape getPosition / getRotation / getScale | vt 0x3C/0x44/0x4C = 0x827D6110 / 0x82DA69B8 / 0x82DA69D0 | **vt 0x38/0x40/0x48 = 0x008EF6B2 / 0x00A24AFB / 0x00A24B3D** | В | копируют this+0x90 / +0xA0 / +0xB0 (16 байт) |
| WorldObject::setPosition (база) | — | vt 0x34 = 0x00A7F942 (пишет только +0x90..+0x98) | В | дизассемблер |
| Detector setPosition / setRotation | vt 0x38 / 0x40 | vt 0x34 = 0x008B7C64 / vt 0x3C = 0x008B7C87 (база + обновить тело +0xD4) | В | |
| EventManager: ключ типа компонента | `[0x838898F0]` | **`[0x015E7338]`** | В | регистрация класса «EventManager» / «WorldComponent» 0x00ABDD32 (тот же порядок вызовов) |
| `getEventManager(world)` | 0x8292FA30 | **0x007735E8** (thiscall, ECX=world) | В | `map World+0x118` по ключу [0x015E7338] — смещение то же; 6 вызовов на обеих платформах |
| EventManager::add / remove детектора | 0x82E5B518 / 0x82E5B2C0 | 0x00ABDCC8 / 0x00ABDB8E | В | список `evm+0x14`, узел 8 байт {next, obj} |
| Actor::onAddToWorld / onRemove | 0x829C36A0 / 0x829C4C20 (vt 0x54/0x58) | 0x00A40971 / 0x00A40A30 (vt 0x50/0x54) | В | тоже регистрируется в EventManager |
| Actor setPosition (телепорт playerFly) | vt 0x38 = 0x829AB738 (пишет +0xF0 и узел +0xD4) | vt 0x34 = 0x00A7F942 — **базовый, без обновления физики** | С | у всех 25 PC-vtable актёров не переопределён. Телепорт на PC, вероятно, нужен через другую функцию (кандидаты: скриптовые `setPosition` 0x00A82268 и др., `TeleportHelper`, `teleportTo`) — задача 03/04 |
| Actor setYaw | vt 0x13C = 0x82531290 | vt 0x138 (зависит от класса) | Н | |
| конфиг: cfg-переменная | ctor 0x82D3AFE8 | ctor 0x00DA8270, объект {+0 тип, +4 значение, +8 секция, +C ключ, +10 next}, список [0x01836E34] | В | |
| `[Debug] DebugCamera` | obj 0x83722BD4 | obj 0x014D2534 | В | ключ регистрируется, **значение никто не читает** ни там, ни там (мёртвый ключ) |
| `[Debug] DeveloperMode`, `[Renderer] PhysicsDebugRender` | есть | есть (строки 0x013D53A0, 0x012B0BAC) | С | читатели не разбирались |
| консоль / ConsoleFunction / меню 206 команд | 0x82A77E28 и др. | **нет** | В | ни одного имени/описания команд; классов консоли нет |
| DebugRenderer / OblivionGameDebug / drawGraphics | 0x82AD76D0 / 0x82A60888 | **нет** | С | строк и RTTI нет; поле Game+0xF4 (на Xbox — указатель на OblivionGameDebug), вероятно, пустое |
| FlyCameraState (тип 10), execute playerFly | 0x820554D0 / 0x825B42C8 | **нет** [ГИП] | С | строк `playerFly`, `StateCameraUtil.cpp` нет; Xbox-состояния камеры 10/11 лежат среди отладочных данных |
| `Mover::Teleport` | есть | 0x0135E088 (строка) | С | рядом «TeleportToSpace called with AreaHandle…» — это мувер навигации (NavPower), не телепорт игрока |
| float 0.03048 (ед. → м) | 0x8201A0E0 (много ссылок) | 0x011B97D4 (2 ссылки: 0x00639CD3 — helper «матрица → Havok», и 0x009C6320) | В | на PC вызов вынесен в функцию 0x00639CC2 (15 вызовов) |
| IAT: Direct3DCreate9 / XInputGetState / DirectInput8Create | — | 0x01155730 / 0x011556D0 (ord 2) / 0x01155028 | В | pefile |

## Проверка гипотезы о смещениях полей [ПОДТВ]
Гипотеза «32 бита → смещения полей совпадают» **подтвердилась для данных**:
- DetectorObject: `+0x58 World*`, `+0x80 Shape`, `+0x90 Position`, `+0xA0 Rotation`, `+0xB0 Scale`, `+0xD0 BodyType (=0x0B)`, `+0xD4 тело`, `+0xD8 enabled (=1 в ctor)` — все найдены в PC-коде (ctor 0x008B7C2A, onAdd 0x008B84FE, onRemove 0x008B888B, getters).
- World: `+0x118` — map компонентов. EventManager: `+0x14` — список детекторов {head, tail, count}, узел {next, obj}.
- Darksiders (Game): `+0x8` — локальный игрок, `+0x138` — камера, `+0xF8` — счётчик паузы, `+0x10` флаги.

**Не совпадают номера виртуальных слотов** в иерархии WorldObject: на PC каждый слот **на 4 меньше** (на Xbox в базовом классе есть лишний — вероятно, отладочный — виртуальный метод до 0x34). Примеры: getPosition Xbox 0x3C → PC 0x38, onAddToWorld 0x54 → 0x50, onRemove 0x58 → 0x54; у «игрока» `vt[0x6C]` → `vt[0x68]`. У класса Darksiders/OblivionGame слоты **совпадают** (0x68 = setPaused на обеих). Правило: смещения полей переносим как есть, слоты vtable — проверять для каждого класса.

## Фильтр «это детектор» на PC (для обхода EventManager)
`vt = *(u32*)obj; vt[0x40] == 0x00A24AFB && vt[0x48] == 0x00A24B3D` (общие getRotation/getScale SimpleShape-наследников), детектор — дополнительно `vt[0x54] == 0x008B888B` (onRemove; у части наследников — свой, см. 0x0083171B). TriggerRegion и наследники — `vt[0x50] == 0x008C8E47`.

## Не найдено (следующие задачи)
- Матрица вида/камеры на PC (объект камеры `game+0x138`: на Xbox состояние в +0x15C, позиция `vt[0x48]`, направление `vt[0x44]` — на PC слоты не проверены).
- Кадр игры (аналог OblivionGame::tick 0x82A6A780, вызывается напрямую из 0x82A6AE98) — для PC хук кадра лучше брать в D3D9 Present; игровой тик — найти в задаче 02/03.
- Правильный «телепорт» актёра на PC (с обновлением character proxy Havok).

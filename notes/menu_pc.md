# Дебаг-меню (DLL v0.7–v0.12, задачи 08–13): устройство и как расширять

Код — `src\dbgmenu\menu.cpp` (реестр, клавиши меню, отрисовка, панель showCameraPosition), `keys.cpp` (с v0.9: F10, назначаемые клавиши, `dbgmenu_keys.ini`). Горячие клавиши ловит WndProc в `input.cpp` (`Keys_HotkeyNow` → `RunHotkey`), меню получает клавиши из очереди (`Input_PopKey` → `Menu_HandleKey`, поток рендера = поток игры): обычный vk — навигация, `KQ_ACTION|действие` — горячая клавиша уже выполнена (только лог), `KQ_CAPTURE|vk` — клавиша для раздела Keys.

## 1. Дерево меню v0.7 (раздел = функция, все её настройки внутри)
```
Main
> playerFly            6 >   (v0.8, задача 09: +3 пункта после playerFly)
    playerFly (shortcut F2)   действие, [flying] в полёте; описание как Xbox «Allow player to float in the air…» + R/T
    freeze time in flight     [off]              g_flyFreezeTime (с чего начинать полёт; T — в полёте; изменение в полёте — сразу)
    attach player             [off]              g_flyAttach (с чего начинать; R — в полёте; изменение в полёте — сразу)
    attach distance           < 200 >            g_attachDist 100…600 шаг 50, формат %.0f (поле fmt)
    hover after teleport      [on]               g_hoverAfterTeleport (по умолч. on)
    hover jump                < game | direct >  g_hoverJumpMode (по умолч. game)
> Triggers             7 >   (v0.12, задача 13: +4 строки info, triggers_pc.md §7)
    Show triggers (shortcut F3) [off]            g_showTriggers (по умолч. off)
    depth                     < through walls | depth >  g_trigDepthMode (по умолч. through walls)
    show disabled             [on]               g_trigShowDisabled (по умолч. on; v0.12 — и для подписей / цели)
    info: nearby              [off]              g_trigNearby (подписи у триггеров рядом с героем / камерой в полёте)
    info: target (shortcut F6) [off]             g_trigTarget (панель слева + жёлтый контур цели)
    nearby range              < 10 m … 30 m … 150 m | unlimited >  g_trigRange (по умолч. 30 m)
    Display              13 >   ПАПКА "Triggers/Display", g_trigShow[TF_*]
        name [on] · class [on] · leads to [on] · events / scripts · who triggers it · flags · state ·
        shape and size · position · distance (target only) · all properties (target only) · all on · all off
> Monsters             7 >   (v0.10, задача 11; v0.11, задача 12 — info level → папка Display; notes/monsters_pc.md)
    info: nearby              [off]              g_monNearby (подписи над мобами)
    info: target (shortcut F5) [off]             g_monTarget (панель моба в центре экрана)
    nearby range              < 10 m … 30 m … 150 m | unlimited >  g_monRange (по умолч. 30 m)
    show                      < monsters | monsters + NPC | all characters >  g_monWho
    Display              14 >   ПАПКА (CT_FOLDER, категория строк "Monsters/Display"), g_monShow[MF_*]
        name [on] · class (target only) [off] · HP [on] · level · damage / defense / resistance · move state ·
        AI behaviour · cooldowns · target · distance (target only) · position · hits [on с v0.12] · all on · all off
    damage info               [off]              g_monDamage (опция игры DamageIndicators, пока on)
    freeze monsters           [off]              g_monFreeze (байт 0x014D4278 = 0)
> Camera               1 >
    showCameraPosition        [off]              g_showCameraPosition (v0.12: + карта / регионы, triggers_pc.md §8)
> Menu                 2 >
    Panel opacity             < 0.80 >  0.30…1.00 шаг 0.05
    Text scale                < 1.00 >  0.75…2.00 шаг 0.25
> Keys                 9 >   (v0.9, задача 10, §6; v0.10 + info: target; v0.12 + triggers info: target)
    debug on/off / menu / playerFly / Show triggers / attach (in flight) / time (in flight) / info: target /
    triggers info: target   [F10] [F1] [F2] [F3] [R] [T] [F5] [F6]
    reset to defaults
```
До 08 (v0.2–v0.6) были Xbox-категории General / Camera / Graphics / Menu. В 08 изменились только раздел и подпись `triggers depth` → `depth`; значения по умолчанию, клавиши, колбэки — те же. Других пунктов в реестре не было (9 пунктов до и после).

## 2. Реестр `g_commands[]`
Одна строка = один пункт: `{раздел, имя, описание, тип, колбэк, bool*, float*, min, max, step, key, int* choice, names, numNames, fmt}` (`fmt` — формат CT_FLOAT, NULL = `%.2f`; с v0.8).
- `CT_ACTION` — Enter вызывает `action` (влево — назад к разделам);
- `CT_TOGGLE` — Enter переключает, влево/вправо = off/on, после изменения — `action` (обычно `Menu_Status`);
- `CT_FLOAT` — влево/вправо ± step в пределах [min, max];
- `CT_ENUM` — влево/вправо по кругу, Enter — следующее; статус «Раздел > пункт: значение», затем `action`.
- `CT_FOLDER` (v0.11) — папка: строка «Имя  N >», Enter/вправо — войти, влево/Esc/Backspace — на уровень выше;
- `CT_TOGGLE` с `action == NULL` (v0.11) — статус сам: «Раздел > папка > пункт: on».
`key` (с v0.9, было `const char *shortcut`) — действие `KeyAction`: у CT_ACTION/CT_TOGGLE — подпись « (shortcut X)» с текущей клавишей; у `CT_KEY` (раздел Keys) — какое действие переназначает пункт. Сама клавиша — `keys.cpp` + `input.cpp` (`RunHotkey`), строка в `Menu_HandleKey` для лога. В `desc` — токены `{fly}` и т. п. (§6).

**Новый пункт:** одна строка в `g_commands[]` с именем раздела своей функции (рядом с остальными строками раздела — порядок строк = порядок пунктов). Переменная — `extern` в `common.h` в блоке своего модуля.
**Новый раздел:** строки с новым именем раздела там, где раздел должен стоять в списке (порядок разделов = порядок первого появления в реестре; максимум 16 разделов, 64 пункта в разделе). Больше ничего менять не нужно: счётчик « N >», навигация и запоминание курсора работают сами.
**Новая подпапка (v0.11):** (1) в разделе — строка `{"Раздел", "Имя", "описание", CT_FOLDER, NULL, NULL, NULL, 0, 0, 0, KA_NONE}` там, где папка должна стоять; (2) её пункты — строки с категорией **`"Раздел/Имя"`** (порядок строк = порядок пунктов). Вложенность любая: `"Раздел/Имя/Под"` + строка CT_FOLDER «Под» с категорией `"Раздел/Имя"`. Категория со `/` в список разделов не попадает. Путь в шапке/статусе/логе — `PathText` (`/` → ` > `). Длина пути ≤ 95 символов, до 64 пунктов в папке, до 32 запомненных путей (разделы + папки).

## 3. Курсор и навигация
- `s_level` (0 — список разделов, 1 — пункты `s_path`), `s_cat` (раздел в Main), **`s_path`** (v0.11: `"Monsters"` или `"Monsters/Display"`), **`SelOf(path)`** — выбранная строка каждого раздела/папки (таблица `s_selMem[32]` по строке пути; было `s_itemSel[16]` по номеру раздела) — статические, при закрытии **не сбрасываются** → F1 открывает меню там, где закрыли, в т. ч. внутри папки (в пределах запуска игры; в файл не сохраняется).
- Разделы: вверх/вниз, Enter/вправо — войти, Esc/Backspace — закрыть. Пункты: вверх/вниз, Enter, влево/вправо, Esc/Backspace — **на уровень выше** (`GoBack`: из папки — в родителя, курсор на строке папки; из раздела — в Main). Влево на действии / клавише / папке — тоже назад; на переключателе — off.
- Шапка: `Main > Monsters > Display`; подсказка внизу: `Enter run/open`.
- Лог: `menu: opened at <Раздел[ > пункт]>`, `closed at …`, `open …`, `back to …`, `execute Раздел/пункт`, `Раздел > пункт = 0.75` (числа), статусы переключателей.

## 4. Офлайн-тест (test_d3d, кадры 1270–1320, снимки shot21–29)
08a F1 открывает на Triggers > depth (там закрыли в 07) · 08b Esc — четыре раздела (3/3/1/2) · 08c playerFly (запомнен hover jump), Enter на playerFly → полёт `[flying]`, Enter → выход · 08d Camera: showCameraPosition off/on · 08e Menu: opacity 0.75, text 1.25 и обратно · 08f Triggers: Enter на Show triggers → on (контуры), F3 при открытом меню → off · 08g Esc Esc, F1 → открыто на списке разделов, Triggers. Старые сценарии 03–07 подправлены под новый порядок разделов (кадры 8, 27, 1183) и прошли.

## 5. v0.8 (задача 09)
Раздел playerFly — 6 пунктов (дерево в §1). Клавиши полёта R/T — в `input.cpp` (WM_KEYDOWN при `g_flying && !g_menuOpen`), не пункты меню. Офлайн-тест: в старых сценариях навигация по playerFly сдвинута (кадр 400 — 4 раза вниз до hover after teleport; кадр 1282 — вниз с hover jump на playerFly); новые 09a–09l — кадры 1325–1515 (`playerfly_pc.md` §7).
Решения пользователя (2026-10-19, после 08): **не нужны** сохранение настроек между запусками, смена клавиш F1/F2/F3, смена оформления меню — не предлагать без запроса.
Решение пользователя (2026-10-19, задача 10) **частично меняет это:** между запусками сохраняются **только клавиши** (`dbgmenu_keys.ini`, §6); остальные настройки меню — по-прежнему не сохраняются. Смена оформления меню — по-прежнему не нужна.

## 6. v0.9 (задача 10): F10 — главный выключатель, раздел Keys, `dbgmenu_keys.ini`
- **`g_debugOn`** (`keys.cpp`) = 0 после запуска: `Menu_WantsDraw` → только строка HOVER (если герой висит); все горячие клавиши, кроме клавиши debug, отдаются игре (`Keys_HotkeyNow`: KA_DEBUG всегда, KA_MENU/KA_FLY/KA_TRIGGERS — только при on; attach/time — только в полёте, когда ввод и так заблокирован). Хуки стоят, лог «trigger fired» и защита работают как раньше.
- **`Debug_Toggle`** (WndProc, F10 приходит как **WM_SYSKEYDOWN**, глотаем и нажатие, и отпускание → нет меню окна): on — лог; off — `Input_SetMenuOpen(false)`, если полёт — **`Flight_RequestStop()`** (`s_reqToggle = 2`: следующий кадр делает выход как F2 — с привязанным героем `StayCarried` → зависание, без — `Exit`; полёт с 2 никогда не начинается; `Enter()` вообще только при `g_debugOn`), `g_showTriggers = g_showCameraPosition = false`, сброс статуса и ожидания клавиши. Настройки разделов (дистанция, режимы, прозрачность) не трогаются. После повторного F10 функции выключены. Зависание после F10 off остаётся (так просил пользователь), его строка видна без подсказки «[F2 - fly again]»; отпускает его любая клавиша управления, в т. ч. бывшие горячие (T/R/буквы — при off это клавиши игры).
- **Надпись** `DEBUG MENU ON  (F1 menu · F10 off)` — по центру у **самого верхнего края** (y = 0, шрифт ×0.75, полупрозрачный фон, без рамки): выше меню/тоста (они с 1.5 высоты шрифта), не задевает панель showCameraPosition (справа, тоже с 1.5), строку полёта (низ, центр) и строку триггеров (низ, слева). Видна всегда, пока on (и при открытом меню).
- **Действия** `KeyAction`: KA_DEBUG F10, KA_MENU F1, KA_FLY F2, KA_TRIGGERS F3, KA_ATTACH R, KA_TIME T. Имена в файле: `debug, menu, playerFly, showTriggers, attach, time`.
- **Допустимые клавиши** — таблица `kNames[]` (она же имена в файле): F1–F24 кроме F4, буквы кроме W A S D C, цифры, Numpad0–9 и Numpad * + − . /, Insert/Delete/Home/End/PageUp/PageDown, знаки (Minus, Equals, скобки, Semicolon, Apostrophe, Backslash, Comma, Period, Slash, Backquote). Всё остальное — «key not allowed» (Esc, Enter, стрелки, Space, Shift, Ctrl, Alt, Win, Tab, Backspace, F4 — из-за Alt+F4, Print Screen, Pause, Lock-клавиши, мышь). Так как WASD/C/Space/Shift/Ctrl запрещены всем, attach/time не могут столкнуться с клавишами полёта; все 6 действий — разные клавиши.
- **Раздел Keys** (последний, 7 пунктов): тип `CT_KEY` (поле `key` в `Command` = действие; у CT_ACTION/CT_TOGGLE то же поле даёт подпись « (shortcut X)» — бывшее `const char *shortcut`). Enter → `g_keyCapture = действие`, значение `[press a key... (Esc - cancel)]`; WndProc отдаёт следующее нажатие (WM_KEYDOWN/WM_SYSKEYDOWN кроме Alt+F4, кнопки мыши, отпускание Print Screen) как `KQ_CAPTURE|vk` и глотает его — горячие клавиши в это время не срабатывают. Esc — отмена; запрещённая — статус «key not allowed: X - press another key», ждём дальше; иначе `Keys_Assign`: занята другим действием → **обмен**, статус «playerFly = F5, menu = F2 (swapped)». Закрытие меню / F10 отменяет ожидание. Последний пункт — **reset to defaults**.
- **Подписи с текущими клавишами:** в описаниях пунктов токены `{debug} {menu} {fly} {triggers} {attach} {time}` (`ExpandKeys`), строка полёта/HOVER, подсказка меню (`Esc/F1 close`), надпись F10, тосты и лог выхода из полёта (`F5 - the carried player stays there`). `HoverExcluded` (что не отпускает зависание) включает текущие горячие клавиши.
- **Файл `dbgmenu_keys.ini`** рядом с `dinput8.dll` (`GetModuleFileName(g_self)`): читается в DllMain (`Keys_Load`, до хуков); **нет файла → создаётся с клавишами по умолчанию** (чтобы игрок сразу нашёл его и мог править Блокнотом); перезаписывается целиком (комментарии + 6 строк) при каждом изменении и reset. Разбор: `;`/`#`/`[` — комментарии, `имя = клавиша [; комментарий]`, регистр не важен. Нет `=`, неизвестное действие, повтор действия — строка игнорируется; неизвестное/запрещённое имя клавиши, клавиша уже взята более ранней строкой → у этой строки клавиша по умолчанию. Действия без годной строки получают своё значение по умолчанию, а если его заняла строка файла — первое свободное значение по умолчанию другого действия (`menu = F2` → playerFly = F1). Битый файл **не перезаписывается** до изменения в меню. Всё — в лог (`keys: line N …`, `keys: in use: …`).
- Лог: `debug: ON|OFF (клавиша) | menu closed | flight ends … | Show triggers off | showCameraPosition off`, `menu: Keys > X: waiting for a key`, `keys: menu = F5 (was F1); swapped: …`, `keys: … written`, `menu: key not allowed: …`, `menu: key change cancelled (…)`.
- **Офлайн-тест** (`test_d3d.cpp`; файл ключей — в папке теста; тест-команда **16** = перечитать файл): 10a при старте F1/F2/F3 доходят до «игры», экран пустой (shot0), F10 (WM_SYSKEYDOWN) не доходит; 10b showCameraPosition + F3 + полёт с R + меню → F10: меню закрыто, герой остался и висит, триггеры/панель выключены (shot38/39), F1/F2/F3 доходят до игры, полёт не начинается, T (уже клавиша игры) отпускает зависание; 10c F10 → только надпись (shot41); 10d Keys > menu: Space/W/ЛКМ — отказ (shot43), F5 → menu = F5, F1 теперь доходит до игры; playerFly = F5 → обмен (menu = F2, shot45); attach = Y, time = R; 10e файл записан, полёт F5 / Y / R(время), T — ничего, подсказки с новыми клавишами (shot46/47); 10f битый файл (Space, дубль, неизвестное имя, мусор, повтор, неизвестное действие, `debug = F11`) → по умолчанию где надо, файл не переписан, F10 доходит до игры, F11 выключает/включает; 10g файл `menu = F2` → playerFly = F1; reset to defaults (shot48) → перечитанный файл без проблем; 10h F10 off → F1 до игры. Старые сценарии 03–09 прошли (в начале добавлено F10; кадр 400 — лишний «вниз», т. к. после Menu теперь Keys).
- **Проверено в игре (2026-10-19):** «всё отлично работает». Лог: файл создан при первом запуске, debug переназначен F10 → F9 (выкл/вкл по F9, F9 в полёте с несомым героем → `flight ends, the carried player stays (hover)`) и обратно на F10, время T, пауза сбалансирована, ошибок нет.

## 7. v0.10 (задача 11): раздел Monsters, клавиша info: target
- Раздел **Monsters** — 7 строк в `g_commands[]` между Triggers и Camera (порядок разделов: playerFly, Triggers, **Monsters**, Camera, Menu, Keys). Устройство и поля — `monsters_pc.md`.
- Новое действие **KA_TARGET** (после KA_TIME, перед KA_COUNT): имя в файле `infoTarget`, подпись `info: target`, по умолчанию **F5** (F4 запрещена из-за Alt+F4), токен `{target}`, работает при `g_debugOn` (`Keys_HotkeyNow`), `RunHotkey` → `Mon_ToggleTarget()`. В разделе Keys — строка перед reset to defaults; reset — F10, F1, F2, F3, R, T, F5.
- **Файл старой версии**: если единственные «проблемы» — нет строк действий новее v0.9 (`a >= KA_TARGET`), файл **дописывается** (лог `… action of a newer version added`), строки игрока не меняются; занята его клавиша по умолчанию (напр. `menu = F5`) → первое свободное значение по умолчанию (F1). Остальные правила разбора — как в §6.
- **F10 off** дополнительно: `Mon_OnDebugOff()` — nearby/target/damage/freeze off, флаги игры назад; лог `| monsters info off | freeze monsters off | damage info off`.
- Сводка Monsters — внизу слева над строкой триггеров (при включённых триггерах поднимается на 1.45 строки); панель цели — справа с 40 % высоты.
- Офлайн-тест: старые сценарии — навигация к Camera +1 «вниз» (кадры 8, 1292, 1540), к Triggers от Menu +1 «вверх» (1304), Keys > reset +1 «вниз» (1643); в 10d/10e клавиша F5 заменена на F6 (F5 теперь info: target). Новые 11a–11n — `monsters_pc.md` §4. Особенность теста: клавиши меню читает следующий кадр оверлея, а F1 срабатывает сразу — закрывать меню надо отдельным кадром.

## 8. v0.11 (задача 12): подпапки, Monsters > Display
- Движок: `CT_FOLDER`, `s_path`, `SelOf`, `OpenPath`/`GoBack`, `PathText`, `FolderPath`, `ToggleDone` (§2–§3). Лог: `menu: open Monsters > Display > hits`, `back to Monsters > Display`, `closed at Monsters > Display > all off`, `execute Monsters/Display/HP`, `Monsters > Display > HP: on`.
- Раздел Monsters: пункт info level убран, на его месте по смыслу (после nearby range / show) — папка **Display** (дерево в §1, устройство полей — `monsters_pc.md` §9). Число пунктов раздела не изменилось (7): индексы damage info (5) и freeze (6) те же.
- Офлайн-тест (кадры 1682–1720 переписаны под Display; новые 12a–12i — кадры 1800–1860, снимки shot63–73): путь в шапке, Esc → Monsters с курсором на Display, Enter → снова на запомненной строке (hits), F1 закрывает/открывает внутри папки, поля по одному (name → +class/HP/level → +stats/move/AI/cooldowns → +target/distance/pos/hits), влево на переключателе = off (не назад), all off → подписей 0, панель «all lines are off», all on, влево на действии = назад, damage info + удар → строка hits, вправо открывает папку, Esc Esc → Main, F1 открывает на Main. Старые сценарии 03–11 прошли.
- Особенность ImGui (было и раньше): высота панели меню подстраивается со сдвигом в 1 кадр — на снимке сразу после перехода панель бывает выше/ниже нужного (в игре — 1 кадр).

## 9. v0.12 (задача 13): Triggers info, KA_TRIGTARGET, карта
- Раздел **Triggers** — 7 строк: после show disabled — info: nearby, info: target (F6), nearby range (те же шаги, что у Monsters, `kMonRangeNames`), папка **Display** (13 строк, дерево в §1). Устройство — `triggers_pc.md` §7. Старые 3 строки не менялись (индексы те же).
- Новое действие **KA_TRIGTARGET** (после KA_TARGET): имя в файле `triggerTarget`, подпись `triggers info: target`, по умолчанию **F6** (свободна), токен `{trigtarget}`, `Keys_HotkeyNow` при `g_debugOn`, `RunHotkey` → `Trig_ToggleTarget()`. В Keys — строка перед reset (теперь 9 строк); reset — F10, F1, F2, F3, R, T, F5, F6. Старый файл (v0.9 / v0.10–v0.11) дописывается строками новых действий (`a >= KA_TARGET`), занятая F6 → первая свободная клавиша по умолчанию.
- **F10 off** → `Trig_OnDebugOff()` (nearby / target off), лог `| triggers info off`.
- `Menu_PanelBottom()` — низ открытого меню (прошлый кадр): панель цели триггеров встаёт под меню.
- showCameraPosition: после координат — `World_Panel()` (карта, активный и загруженные регионы, `triggers_pc.md` §8).
- Офлайн-тест: `triggers_pc.md` §7 (кадры 1858–1955).

## 10. v0.13 (задача 15): раздел Cheats
- Раздел **Cheats** — 3 строки в `g_commands[]` сразу после строк `Monsters/Display` (порядок разделов: playerFly, Triggers, Monsters, **Cheats**, Camera, Menu, Keys — 7):
  ```
  > Cheats               3 >
      invincible                [off]   g_cheatInvincible (CT_TOGGLE -> Cheats_MenuChanged(true): байт игры сразу)
      one-hit kill              [off]   g_cheatOneHit     (CT_TOGGLE -> Cheats_MenuChanged(false))
      kill all monsters                 CT_ACTION -> Cheats_RequestKillAll(), статус "kill all monsters..." -> итог пишет cheats.cpp
  ```
  Горячих клавиш нет (Keys не менялся), не сохраняются. Устройство — `cheats_pc.md`.
- **F10 off** → `Cheats_OnDebugOff()` (оба off, байт игры назад), лог `| cheats off`.
- Офлайн-тест: старые сценарии — +1 «вниз»/«вверх» при переходе через Cheats в списке разделов (кадры 8, 1292, 1304, 1540, 1673, 1770, 1802, 1909); новые 15a–15k — кадры 1958–2025 (`cheats_pc.md` §4). Проверка совпадения навигации: строки `menu: open/execute/opened/closed/back/…` лога нового прогона построчно сравнены с прогоном старой DLL (из папки игры) на том же тесте до вставки сценариев 15.

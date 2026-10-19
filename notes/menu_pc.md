# Дебаг-меню (DLL v0.7–v0.8, задачи 08–09): устройство и как расширять

Код — `src\dbgmenu\menu.cpp` (реестр, клавиши, отрисовка, панель showCameraPosition). Клавиши F1/F2/F3 ловит WndProc в `input.cpp` (`IsHotkey`), меню получает остальные клавиши из очереди (`Input_PopKey` → `Menu_HandleKey`, поток рендера).

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
> Triggers             3 >
    Show triggers (shortcut F3) [off]            g_showTriggers (по умолч. off)
    depth                     < through walls | depth >  g_trigDepthMode (по умолч. through walls)
    show disabled             [on]               g_trigShowDisabled (по умолч. on)
> Camera               1 >
    showCameraPosition        [off]              g_showCameraPosition
> Menu                 2 >
    Panel opacity             < 0.80 >  0.30…1.00 шаг 0.05
    Text scale                < 1.00 >  0.75…2.00 шаг 0.25
```
До 08 (v0.2–v0.6) были Xbox-категории General / Camera / Graphics / Menu. В 08 изменились только раздел и подпись `triggers depth` → `depth`; значения по умолчанию, клавиши, колбэки — те же. Других пунктов в реестре не было (9 пунктов до и после).

## 2. Реестр `g_commands[]`
Одна строка = один пункт: `{раздел, имя, описание, тип, колбэк, bool*, float*, min, max, step, shortcut, int* choice, names, numNames, fmt}` (`fmt` — формат CT_FLOAT, NULL = `%.2f`; с v0.8).
- `CT_ACTION` — Enter вызывает `action` (влево — назад к разделам);
- `CT_TOGGLE` — Enter переключает, влево/вправо = off/on, после изменения — `action` (обычно `Menu_Status`);
- `CT_FLOAT` — влево/вправо ± step в пределах [min, max];
- `CT_ENUM` — влево/вправо по кругу, Enter — следующее; статус «Раздел > пункт: значение», затем `action`.
`shortcut` — только подпись « (shortcut X)»; сама клавиша — в `input.cpp` (WndProc) + строка в `Menu_HandleKey` для лога.

**Новый пункт:** одна строка в `g_commands[]` с именем раздела своей функции (рядом с остальными строками раздела — порядок строк = порядок пунктов). Переменная — `extern` в `common.h` в блоке своего модуля.
**Новый раздел:** строки с новым именем раздела там, где раздел должен стоять в списке (порядок разделов = порядок первого появления в реестре; максимум 16 разделов, 64 пункта в разделе). Больше ничего менять не нужно: счётчик « N >», навигация и запоминание курсора работают сами.

## 3. Курсор и навигация
- `s_level` (0 — список разделов, 1 — пункты), `s_cat`, `s_itemSel[16]` (свой выбранный пункт у каждого раздела) — статические, при закрытии **не сбрасываются** → F1 открывает меню там, где закрыли (в пределах запуска игры; в файл не сохраняется).
- Разделы: вверх/вниз, Enter/вправо — войти, Esc/Backspace — закрыть. Пункты: вверх/вниз, Enter, влево/вправо, Esc/Backspace — назад к разделам.
- Лог: `menu: opened at <Раздел[ > пункт]>`, `closed at …`, `open …`, `back to …`, `execute Раздел/пункт`, `Раздел > пункт = 0.75` (числа), статусы переключателей.

## 4. Офлайн-тест (test_d3d, кадры 1270–1320, снимки shot21–29)
08a F1 открывает на Triggers > depth (там закрыли в 07) · 08b Esc — четыре раздела (3/3/1/2) · 08c playerFly (запомнен hover jump), Enter на playerFly → полёт `[flying]`, Enter → выход · 08d Camera: showCameraPosition off/on · 08e Menu: opacity 0.75, text 1.25 и обратно · 08f Triggers: Enter на Show triggers → on (контуры), F3 при открытом меню → off · 08g Esc Esc, F1 → открыто на списке разделов, Triggers. Старые сценарии 03–07 подправлены под новый порядок разделов (кадры 8, 27, 1183) и прошли.

## 5. v0.8 (задача 09)
Раздел playerFly — 6 пунктов (дерево в §1). Клавиши полёта R/T — в `input.cpp` (WM_KEYDOWN при `g_flying && !g_menuOpen`), не пункты меню. Офлайн-тест: в старых сценариях навигация по playerFly сдвинута (кадр 400 — 4 раза вниз до hover after teleport; кадр 1282 — вниз с hover jump на playerFly); новые 09a–09l — кадры 1325–1515 (`playerfly_pc.md` §7).
Решения пользователя (2026-10-19, после 08): **не нужны** сохранение настроек между запусками, смена клавиш F1/F2/F3, смена оформления меню — не предлагать без запроса.

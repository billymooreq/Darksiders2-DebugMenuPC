# PC: камера, матрицы вида/проекции, игровой кадр, телепорт (задача 03, этап 4 — статика)

Только статический разбор (игра не запускалась). Декомпиляция: `dumps/decomp_pc/{proj,proj_callers,winmain,apprun,gamerun,gameframe,tickcand,tick2,setpos_bind,setpos_native}.c`.

## 1. Игровая камера `game+0x138` = StateCamera [ПОДТВ статически]
- vtable **0x01199EA0** (сразу за ней строка «StateCamera», RTTI нет). Ещё 3 vtable с тем же `getDir` 0x005B5DB6 — 0x01172AA8, 0x01172B08, 0x0118FE60 (вероятно наследники; какая у игрока — покажет панель showCameraPosition, `vt`).
- Слоты (Xbox −4, как у WorldObject):
  | слот | функция | что |
  |---|---|---|
  | vt[0x38] | 0x005B5D33 | update(arg, Matrix*): `state(+0x15C)->vt[0x30](arg, cam+0x100)`, копии 0x40 байт в `cam+0x100` (= Xbox 0x82585650, там же Xbox печатал «Camera: Pos(%f,…) Rot(…)» по флагу showCameraPosition 0x83793EB0) |
  | vt[0x3C] | 0x005B5BD6 | getMatrix(out) = копия **`cam+0x80`** (4×4 float) |
  | vt[0x40] | 0x005B5DB6 | getDirection(out) = **−строка 2** матрицы |
  | vt[0x44] | 0x00585FB5 | getPosition(out) = **строка 3** (`cam+0xB0..0xB8`) |
- Поля: `+0x80` мировая матрица камеры (строки = оси, строка 3 = позиция), `+0x100` матрица, которую пишет состояние, `+0x15C` текущее состояние камеры (тип — `state->vt[0x40]`, обёртки 0x005B5E7E и проверки типа 0x005D4D83/…DA0/…E14), `+0x160` состояние по умолчанию. Совпадает с Xbox (+0x15C/+0x160).
- Мир Z-вверх [ГИП, по Xbox setYaw {0,0,yaw}].

## 2. Рендер-камера, вид и проекция [ПОДТВ статически]
- **0x00925969** `buildViewProj(renderCam, outView, outProj, zNear, aspect)` (cdecl): `outView = transpose(renderCam+0x40)` (через 0x004EE2E3); тип `renderCam+0xD0`: 2/7 → `D3DXMatrixPerspectiveFovLH(outProj, fov = renderCam+0xE8 × [0x015352AC], aspect, zn, far = renderCam+0xDC)`; 4/8/0xC → орто (0x00905131, `+0xEC/+0xF4` размеры, `+0xD8` near). `[0x015352AC]` — множитель, задаётся в рантайме (в .data 0; по смыслу градусы→радианы [ГИП]). Единственный вызов D3DXMatrixPerspectiveFovLH в exe.
- Вызывается из рендера: 0x009280DA, 0x00928C4F, 0x0092AAE2 (+ 0x0092B139, 0x0092B93A). Там же рендер-камера отдаёт шейдерам `renderCam+0x80` → VS c103 (4 регистра, вероятно view×proj) и `renderCam+0xC0` → c119 (1 регистр, вероятно позиция глаза) [ГИП]; устройство D3D — `renderer+0x34` (vt 0x178 = SetVertexShaderConstantF, 0x1B4 = SetPixelShaderConstantF, 0x114 = SetSamplerState).
- **0x00B3CD27** — перенос игровой камеры в рендер (в кадре): камера = `0x00B3B15C(game)` (= `game+0xBC ? 0x00AC153E() : 0`), берёт у неё `vt[0x7C]` (матрица), `vt[0x70]` FOV × `[0x015352AC]`, `vt[0x78]`/`vt[0x74]` (near/far?). Это **лучшая точка для playerFly**: подменить вход (матрицу камеры) до или после этой функции. Какой объект возвращает 0x00AC153E (StateCamera или «камера-узел») — проверить в 04.
- Варианты для свободной камеры (задача 04): (а) после `StateCamera::update` (vt[0x38] 0x005B5D33) переписывать `cam+0x80` своей матрицей — рендер возьмёт её по цепочке; (б) хук 0x00B3CD27 / 0x00925969 и подмена `renderCam+0x40` (вид) — не трогает логику камеры; (в) запасной — патч констант c103/c119 (хрупко). Рекомендация: (а), при проблемах — (б).

## 3. Главный цикл и игровой кадр [ПОДТВ статически]
- WinMain 0x00F92AE0 → приложение `[0x0184FAF0]` (vtable 0x013D5700) → run 0x00403C8F → в конце `instance()` + **0x00B3D821** `Darksiders::run`: `while (!(game+0x10 & 0x10)) { app->vt[1]() /*0x00AE3CCC — PeekMessage-насос*/; 0x00B3D3E4(game); }`.
- **0x00B3D3E4** — кадр игры (`__fastcall`, ECX = game) = Xbox 0x82A6AE98; Xbox-тик 0x82A6A780 на PC, похоже, **встроен** в него (кадр сам пишет `game+0xD8 = [0x01533008]`, `game+0x30 = dt` — как Xbox-тик пишет +0xD8/+0x3C) [ГИП]. Внутри: `game->vt[0xBC]`, `game->vt[0x50]`, 0x00B3D066, 0x00B3CD27 (камера → рендер), 0x00B3C0FB (настройки рендера), 0x00B3B8D2 (2D/HUD-проход).
- **Поток рендера:** флаг `game+0x10 & 1` включает ветку с `SetEvent(game+0xEC)`/`SetEvent(param[0x3B])` и `renderer->vt[0x4C/0x48]` — у игры, вероятно, **может быть отдельный поток рендера** [ГИП]. Тогда `Present` (наш оверлей) и логика игры идут в разных потоках. Лог v0.2 покажет: «render thread N» (первый Present) vs «WndProc subclassed … thread=M».
- Хук «раз в кадр в потоке игры» для 04: MinHook на **0x00B3D3E4** (ECX = game).

## 4. Телепорт актёра [ПОДТВ статически]
- Все скриптовые `setPosition` (5 регистраций строки 0x0117CE98) сводятся к **`obj->vt[0x34](Vec*)`**: 0x00A7E0DB (x,y,z → vt[0x34]), 0x00A6C1EB (`jmp [vt+0x34]`), 0x00503BE3 (`vt[0xB8]` — другой класс).
- Скриптовый **`teleportTo`** (строка 0x012172EC, обработчик **0x00796B71**, метод самого двигаемого объекта): отдельно проверяет «это игрок?» (`instance()` → `getPlayerActor` и сравнение), считает точку, затем **`this->vt[0x34](&pos)`** (0x00796DB2) и **`this->vt[0x138](yaw)`** (0x00796EC9/…F01), yaw из направления — 0x00ACB5FC [ГИП]. Это ровно схема Xbox-playerFly (setPosition vt[0x38] + setYaw vt[0x13C], на PC −4).
- Значит, **игра сама телепортирует игрока через vt[0x34]**, хотя базовый WorldObject::setPosition 0x00A7F942 пишет только `+0x90`. Физическое тело персонажа, вероятно, подтягивается из `+0x90` при обновлении актёра [ГИП]. В v0.2 панель showCameraPosition показывает `setPos`/`setYaw` игрока (адреса vt[0x34]/vt[0x138]) — проверить в игре, что setPos = 0x00A7F942 или своё.
- Остальное: `TeleportHelper` (0x0116E474, 4 места: 0x004D169D, 0x00591212/683/A30), `doTeleport` (0x0057A568, 0x00591A68) — меню телепорта по карте (узлы `TeleportLocationNode`, переход экрана), для playerFly не нужно. `Mover::Teleport` — NavPower (ИИ).

## 5. План для задачи 04 (кратко)
1. Хук кадра 0x00B3D3E4 (поток игры) — там вся логика полёта: ввод (WASD/мышь из нашего фильтра Raw Input), скорость 375×2 ед/с (Shift ×3, Ctrl ×0.5 как Xbox), пауза `game->vt[0x68](true)` 0x00B3C287.
2. Камера: писать свою матрицу в `StateCamera+0x80` после его update (хук vt[0x38] 0x005B5D33 или после 0x00B3CD27-источника) — проверить, что рендер берёт её на паузе.
3. Телепорт: `actor->vt[0x34](&camPos)`, `actor->vt[0x138](yaw)`, затем `setPaused(false)`.

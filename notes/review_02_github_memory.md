# Ревью 02: GitHub + память для нового чата

## Что сделано
1. Коммит `01: setup + recon` (c352a85), аннотированный тег `review-01`. Ветка переименована `master` → `main`.
2. `origin` = `git@github.com:billymooreq/Darksiders2-DebugMenu.git` (приватный, до push был пустой — `ls-remote` ничего не вернул). `git push -u origin main --follow-tags`, `ls-remote` показал `refs/heads/main` и `refs/tags/review-01`.
3. `README.md` репо (по образцу Xbox): проект, структура, что исключено и как восстановить, схема конца задачи, откат.
4. `scripts/sync_brain.ps1` — robocopy /MIR `..\brain\` → `_claudework\brain\` (источник только читается). Прогнан: 9 файлов (README_FOR_CLAUDE, LOG, prompts 01/02/03, reviews).
5. `scripts/ghidra/ExportGzf.java` (из Xbox-проекта) → `ghidra_export/Darksiders2.exe.gzf` (94.6 МБ) в **Git LFS** (`.gitattributes`).
6. `.gitignore` дополнен: файлы игры (`*.exe`, `*.upak`, `*.obsp`, `*.mnfst`, `*.dsl`, `worlds.bin`…), логи Ghidra, `dumps/**/run.log`; `tools/*` кроме `tools/VERSIONS.md`.
7. `CLAUDE.md` в корне переписан для нового чата (+ копия `CLAUDE.md.copy`); `notes/game_changes.md` — записан `dinput8.dll` v0.1 в папке игры; `plan_architecture.md` — решения пользователя.

## Что в репо и что исключено
- В репо: `notes/`, `src/`, `build/proxy_dinput8/{dinput8.dll,README.md}`, `scripts/`, `dumps/` (строки, PE-сводки, декомпиляция; ~2.3 МБ), `brain/` (зеркало), `ghidra_export/*.gzf` (LFS), `README.md`, `CLAUDE.md.copy`, `tools/VERSIONS.md`, `requirements.txt`.
- Исключено: `tools/` (~1.2 ГБ), `ghidra_project/` (~300 МБ, сырой), `ghidra_xbox/` (873 МБ, есть в Xbox-репо), `backup/`, `logs/`, файлы игры, `.obj/.lib/.exp/.pdb`, логи Ghidra.

## Размеры, LFS
- Обычные файлы в git ≈ 2.7 МБ; самый крупный не-LFS — `dumps/strings_pc_ascii.txt` (~2 МБ) и `dinput8.dll` (125 КБ).
- LFS: 1 объект, `Darksiders2.exe.gzf` 94 585 652 байт (квота LFS бесплатного аккаунта — 1 ГБ хранения; одна версия ≈ 9%). Перевыгружать только при заметной новой разметке.
- Секреты: `git grep` по key/secret/password/token/ssh-ключам/PAT/email — ничего. Файлов >50 МБ вне LFS нет.
- Внимание: `.gzf` содержит байты `Darksiders2.exe` (внутри базы Ghidra) — допустимо только потому, что репо **приватный** (так же сделано в Xbox-репо).

## Что записано в CLAUDE.md
Проект и цель; схема работы (промпты от пользователя, `progress_NN` по этапам, `review_NN` в конце, `work_report.md`); жёсткие правила; **решения пользователя** (коммит+тег+push в конце задачи без вопроса; репо; только клавиатура, F1 — меню, F2 — playerFly; MinHook/ImGui разрешены; стиль Xbox-меню); структура; инструменты и команды (8.3-пути, venv, MSVC-обёртка, сборка и офлайн-тест прокси, Ghidra PC/Xbox, `pcfind.py` и др.); ключевые адреса PC (коротко, ссылка на `xbox_pc_map.md`); текущее состояние; **следующий шаг — задача 03, DLL v0.2** (MinHook + ImGui DX9, Direct3DCreate9 → CreateDevice → EndScene/Present + Reset, WndProc / GetRawInputData, меню F1, заготовки playerFly F2 и Show triggers, позиция игрока/камеры на экране, лог, `__try/__except`); индекс заметок; git-схема конца задачи.

## Проблемы
- Глобальное правило пользователя (`~\.claude\CLAUDE.md`: «перед коммитом — план и подтверждение, перед тегом — отдельное подтверждение») противоречит решению «коммить и пушь без вопроса». В проектный `CLAUDE.md` записано решение для **этого** проекта; новый чат увидит оба — приоритет у проектного, явно указано в §4.
- В `brain\prompts\` два файла с номером 02 (`prompt_02_github_memory.md`, `prompt_02_menu_overlay.md`) — последний, видимо, старый черновик; актуальный следующий — `prompt_03_menu_overlay.md`. Я их не трогал (папка только для чтения).
- Предупреждения git «LF will be replaced by CRLF» — безвредны.

## Вопросы ко мне
1. Оставить `dumps/strings_pc_ascii.txt` (~2 МБ) в репо или убрать (пересоздаётся `scripts/strings_dump.py` за секунды)?
2. Добавить в глобальный `~\.claude\CLAUDE.md` исключение для этого проекта про коммиты, чтобы не было двусмысленности? (Сам не правлю — это ваш файл.)

# Darksiders2-DebugMenu

Порт маленькой части дебаг-меню из DEBUG-сборки Xbox 360 в **PC-версию Darksiders II 2012 года** (x86, не Deathinitive Edition): свободная камера с телепортом (`playerFly`) и показ триггеров. Внедрение — прокси-DLL `dinput8.dll` рядом с игрой, меню — свой оверлей поверх D3D9.
Это приватная копия рабочей папки `_claudework\` с историей по задачам (тег `review-NN` = конец задачи NN). Память ассистента — `CLAUDE.md.copy` (оригинал лежит в корне проекта, на уровень выше).

## Структура
| путь | что |
|---|---|
| `notes/` | ревью задач `review_NN_*.md`, журналы `progress_NN.md`, заметки: `xbox_pc_map.md` (адреса PC), `playerfly_pc.md`, `triggers_pc.md`, `playerfly_xbox.md`, `plan_architecture.md`, `game_changes.md` |
| `src/` | исходники DLL: `proxy_dinput8/` (v0.1), `dbgmenu/` (меню, сейчас v0.7: разделы playerFly / Triggers / Camera / Menu; тест `dbgmenu/test/`), `third_party/` (ImGui, MinHook — `VERSIONS.md`) |
| `build/` | собранные DLL + `README.md` для пользователя (как поставить/убрать) |
| `scripts/` | Python-инструменты (x86-поиск `pcfind.py`, PE/строки, RTTI, Xbox-дизассемблер), раннеры `ghidra_*.bat`, `msvc32.bat`, `ghidra/*.java`, `sync_brain.ps1` |
| `dumps/` | PE-сводки, строки exe, декомпиляция (`decomp_pc/`, `decomp_xbox/`) |
| `ghidra_export/Darksiders2.exe.gzf` | упакованная программа Ghidra PC-exe с анализом (Git LFS, ~90 МБ) |
| `brain/` | зеркало папки планировщика `..\brain\` (README_FOR_CLAUDE, LOG, prompts, reviews). Вручную не править — обновляется `scripts/sync_brain.ps1` |
| `tools/VERSIONS.md` | версии инструментов, которых нет в репо, и как их вернуть |
| `requirements.txt` | пакеты Python venv |

## Что не включено и как восстановить
- **`tools/`** (JDK 21, Ghidra 12.1.3 + XEXLoaderWV, venv; ≈1.2 ГБ) — по `tools/VERSIONS.md`. Компилятор — MSVC 2022 Build Tools из системы.
- **Сырой проект Ghidra `ghidra_project/`** (~300 МБ) — вместо него `ghidra_export/*.gzf`: Ghidra → File → Import File, или заново `scripts\ghidra_import.bat` (~19 мин).
- **`ghidra_xbox/`** — копия Ghidra-проекта Xbox (873 МБ); он хранится в репо Xbox-проекта (`billymooreq/ds2-trigger-overlay`, `ghidra_export/DarksidersDebug.xex.gzf`). Восстановить: скопировать `<xbox project>\ghidra_project\ds2dbg.{gpr,rep}` в `ghidra_xbox/`.
- **Файлы игры** (`Darksiders2.exe`, `media/`, DLL игры) — нужна своя копия игры в `..\Darksiders II\`. Исключение: `.gzf` содержит байты exe внутри базы Ghidra (репо приватный).
- `backup/` (копии файлов игры), `logs/` (логи DLL из игры), промежуточные файлы сборки (`.obj/.lib/.exp/.pdb`), логи Ghidra.

## Схема конца задачи NN
```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\sync_brain.ps1
# обновить ..\CLAUDE.md (состояние, индекс) и скопировать в CLAUDE.md.copy
git status                       # нет файлов игры, tools/, ghidra_project/
git add -A
git commit -m "NN: <тема>"
git tag -a review-NN -m "NN: <тема>"   # именно -a: --follow-tags пушит только аннотированные
git push --follow-tags
git ls-remote origin             # должен быть refs/tags/review-NN
```
Если Ghidra-проект PC заметно изменился — перевыгрузить: `scripts\ghidra_run.bat -readOnly -postScript ExportGzf.java <8.3-путь>\ghidra_export\Darksiders2.exe.gzf` (каждая версия ~90 МБ квоты LFS).

**Откат:** `git log --oneline --decorate`; файл — `git checkout review-NN -- <путь>`; целиком — `git switch -c tmp review-NN`. `reset --hard` и переписывание истории — только по просьбе владельца.

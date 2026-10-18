# Сторонний код (разрешён пользователем, задача 02)

| библиотека | версия | источник | commit | лицензия | что взято |
|---|---|---|---|---|---|
| Dear ImGui | **1.91.9** (тег `v1.91.9`, 2025-03-14) | https://github.com/ocornut/imgui | `97428e8ac99e339ce05eee531cf55b77b29ea709` | MIT (`imgui/LICENSE.txt`) | ядро (`imgui*.cpp/h`, `imconfig.h`, `imstb_*.h`) + бэкенды `backends/imgui_impl_dx9.*`, `backends/imgui_impl_win32.*` |
| MinHook | **1.3.4** (тег `v1.3.4`, 2025-03-28) | https://github.com/TsudaKageyu/minhook | `c3fcafdc10146beb5919319d0683e44e3c30d537` | BSD-2-Clause (`minhook/LICENSE.txt`, включает HDE32 — BSD) | `include/MinHook.h`, `src/*.c/h`, `src/hde/hde32.*`, `pstdint.h`, `table32.h` (x64-часть не нужна) |

Файлы скопированы без изменений. Сборка — `src/dbgmenu/build.bat` (флаг `IMGUI_IMPL_WIN32_DISABLE_GAMEPAD`: XInput ImGui не грузит).
Обновление: склонировать нужный тег во временную папку и заменить те же файлы; записать новую версию сюда.

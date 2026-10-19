// Debug menu in the style of the Xbox DEBUG build console menu (Console::draw 0x82A76DE8):
// dark translucent panel, categories -> items, "> " + highlight bar on the current row,
// toggles show their state, items with a hotkey show " (shortcut X)".
#include "common.h"
#include <stdio.h>
#include <stdarg.h>
#include <float.h>
#include "imgui.h"

bool g_showCameraPosition;
static float s_panelAlpha = 0.80f;
static float s_textScale = 1.00f;

enum CmdType { CT_ACTION, CT_TOGGLE, CT_FLOAT, CT_ENUM };
struct Command {
    const char *category;
    const char *name;
    const char *desc;
    CmdType type;
    void (*action)();  // CT_ACTION: run; CT_TOGGLE: called after the flag changed
    bool *flag;        // CT_TOGGLE
    float *value;      // CT_FLOAT (Left/Right)
    float vmin, vmax, step;
    const char *shortcut;
    int *choice;                // CT_ENUM (Left/Right, Enter = next)
    const char *const *names;
    int numNames;
    const char *fmt;            // CT_FLOAT value format (NULL = "%.2f")
};

static void Cmd_PlayerFly();
static void On_ShowTriggers();
static void On_TrigDepth();
static void On_ShowDisabled();
static void On_ShowCameraPosition();
static void On_Hover();
static void On_FreezeTime();
static void On_Attach();

static const char *const kHoverJumpNames[] = {"game", "direct"};  // order of HJ_GAME / HJ_DIRECT
static const char *const kTrigDepthNames[] = {"through walls", "depth"};  // order of TD_WALLS / TD_DEPTH

// Command registry (like the Xbox console registry): one line = one menu item.
// Task 08: one category = one feature, all its settings inside it. Categories are shown in order of their first
// appearance here, items in their order inside the category.
//   new item    -> one line with the category of its feature (keep it next to the other lines of that category);
//   new section -> lines with a new category name, placed where the section should appear (max 16 sections).
static const Command g_commands[] = {
    // ---- playerFly ----
    {"playerFly", "playerFly", "Allow player to float in the air for debugging. WASD + mouse fly, Shift x3, Ctrl x0.5, "
     "Space/C up/down, Enter or left click: teleport the player here, R: carry the player, T: freeze / run time, "
     "F2: back (a carried player stays where he is).",
     CT_ACTION, Cmd_PlayerFly, NULL, NULL, 0, 0, 0, "F2"},
    {"playerFly", "freeze time in flight", "On: the game is paused while you fly (as before). Off: the world, enemies "
     "and triggers keep running. T switches it during a flight.",
     CT_TOGGLE, On_FreezeTime, &g_flyFreezeTime, NULL, 0, 0, 0, NULL},
    {"playerFly", "attach player", "The flight starts with the player carried in front of the camera: he passes "
     "through walls, does not fall and touches triggers. R switches it during a flight.",
     CT_TOGGLE, On_Attach, &g_flyAttach, NULL, 0, 0, 0, NULL},
    {"playerFly", "attach distance", "How far in front of the camera the carried player is.",
     CT_FLOAT, NULL, NULL, &g_attachDist, 100.0f, 600.0f, 50.0f, NULL, NULL, NULL, 0, "%.0f"},
    {"playerFly", "hover after teleport", "After the teleport the player hangs in the air until you press a move, "
     "jump or attack key (the camera works). Off: he falls at once.",
     CT_TOGGLE, On_Hover, &g_hoverAfterTeleport, NULL, 0, 0, 0, NULL},
    {"playerFly", "hover jump", "Space while hanging: a normal jump, then the double jump. game: the game jumps by "
     "itself (its jump from a fall is kept ready); direct: the menu starts the jump. Also after W/attack: one jump "
     "in the air while falling.",
     CT_ENUM, NULL, NULL, NULL, 0, 0, 0, NULL, &g_hoverJumpMode, kHoverJumpNames, 2},
    // ---- Triggers ----
    {"Triggers", "Show triggers", "Draw the live trigger volumes (detectors of the loaded level): magenta = "
     "TriggerRegion, cyan = other detectors, gray = switched off.",
     CT_TOGGLE, On_ShowTriggers, &g_showTriggers, NULL, 0, 0, 0, "F3"},
    {"Triggers", "depth", "through walls: the lines are on top of everything. depth: walls hide the lines "
     "(drawn into the 3D scene before the HUD).",
     CT_ENUM, On_TrigDepth, NULL, NULL, 0, 0, 0, NULL, &g_trigDepthMode, kTrigDepthNames, 2},
    {"Triggers", "show disabled", "Also draw the switched-off triggers (gray, paler).",
     CT_TOGGLE, On_ShowDisabled, &g_trigShowDisabled, NULL, 0, 0, 0, NULL},
    // ---- Camera ----
    {"Camera", "showCameraPosition", "Display the current position of the player and the camera.",
     CT_TOGGLE, On_ShowCameraPosition, &g_showCameraPosition, NULL, 0, 0, 0, NULL},
    // ---- Menu ----
    {"Menu", "Panel opacity", "Background opacity of the menu panel.",
     CT_FLOAT, NULL, NULL, &s_panelAlpha, 0.30f, 1.00f, 0.05f, NULL},
    {"Menu", "Text scale", "Size of the menu text.",
     CT_FLOAT, NULL, NULL, &s_textScale, 0.75f, 2.00f, 0.25f, NULL},
};
static const int kNumCommands = sizeof(g_commands) / sizeof(g_commands[0]);

static const char *s_cats[16];
static int s_numCats;
// Cursor: never reset when the menu closes, so F1 opens it where it was closed (for the whole game run).
static int s_level;         // 0 = categories, 1 = items of s_cat
static int s_cat;
static int s_itemSel[16];   // remembered selection per category
static char s_status[256];
static DWORD s_statusUntil;

static void InitCategories()
{
    if (s_numCats) return;
    for (int i = 0; i < kNumCommands; i++) {
        int c = 0;
        while (c < s_numCats && strcmp(s_cats[c], g_commands[i].category) != 0) c++;
        if (c == s_numCats && s_numCats < 16) s_cats[s_numCats++] = g_commands[i].category;
    }
}

static int ItemsOf(int cat, const Command **out, int max)
{
    int n = 0;
    for (int i = 0; i < kNumCommands && n < max; i++)
        if (strcmp(g_commands[i].category, s_cats[cat]) == 0) out[n++] = &g_commands[i];
    return n;
}

// "Triggers" (section list) or "Triggers > depth" (items) - for the log
static const char *Where()
{
    static char buf[96];
    if (!s_numCats) return "-";
    if (s_level == 0) return s_cats[s_cat];
    const Command *items[64];
    int n = ItemsOf(s_cat, items, 64);
    int sel = s_itemSel[s_cat] < n ? s_itemSel[s_cat] : n - 1;
    sprintf_s(buf, "%s > %s", s_cats[s_cat], sel >= 0 ? items[sel]->name : "-");
    return buf;
}

void Menu_Status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(s_status, sizeof(s_status), _TRUNCATE, fmt, ap);
    va_end(ap);
    s_statusUntil = GetTickCount() + 3000;
    Log("menu: %s", s_status);
}

static bool StatusActive() { return s_status[0] && (LONG)(s_statusUntil - GetTickCount()) > 0; }

bool Menu_WantsDraw()
{
    return g_menuOpen || g_showCameraPosition || StatusActive() || g_flying || Flight_Hovering() || g_showTriggers;
}

// ---- commands ----
static void Cmd_PlayerFly() { Flight_RequestToggle(); }  // done by the next game frame
static void On_ShowTriggers() { Menu_Status("Show triggers %s", g_showTriggers ? "on" : "off"); }
static void On_TrigDepth()
{
    if (g_trigDepthMode == TD_DEPTH) Trig_StartCapture();  // one frame of render target calls into the log
}
static void On_ShowDisabled() { Menu_Status("show disabled %s", g_trigShowDisabled ? "on" : "off"); }
static void On_ShowCameraPosition() { Menu_Status("Show Camera Position %s", g_showCameraPosition ? "on" : "off"); }
static void On_Hover() { Menu_Status("hover after teleport %s", g_hoverAfterTeleport ? "on" : "off"); }
// task 09: the start state of a flight; changed during a flight it also applies at once
static void On_FreezeTime()
{
    Menu_Status("freeze time in flight %s", g_flyFreezeTime ? "on" : "off");
    Flight_RequestTime(g_flyFreezeTime ? 1 : 0);
}
static void On_Attach()
{
    Menu_Status("attach player %s", g_flyAttach ? "on" : "off");
    Flight_RequestAttach(g_flyAttach ? 1 : 0);
}

static void Execute(const Command *c)
{
    switch (c->type) {
    case CT_ACTION:
        if (c->action) c->action();
        break;
    case CT_TOGGLE:
        *c->flag = !*c->flag;
        if (c->action) c->action();
        break;
    case CT_FLOAT:
        break;
    case CT_ENUM:
        *c->choice = (*c->choice + 1) % c->numNames;
        Menu_Status("%s > %s: %s", c->category, c->name, c->names[*c->choice]);
        if (c->action) c->action();
        break;
    }
}

static void Change(const Command *c, int dir)
{
    if (c->type == CT_ENUM) {
        *c->choice = (*c->choice + c->numNames + dir) % c->numNames;
        Menu_Status("%s > %s: %s", c->category, c->name, c->names[*c->choice]);
        if (c->action) c->action();
    } else if (c->type == CT_TOGGLE) {
        bool v = dir > 0;
        if (*c->flag != v) {
            *c->flag = v;
            if (c->action) c->action();
        }
    } else if (c->type == CT_FLOAT) {
        float v = *c->value + dir * c->step;
        *c->value = v < c->vmin ? c->vmin : (v > c->vmax ? c->vmax : v);
        Log("menu: %s > %s = %.2f", c->category, c->name, *c->value);
    }
}

// ---- keys (render thread; F1 itself already toggled g_menuOpen in the WndProc) ----
void Menu_HandleKey(int vk)
{
    InitCategories();
    if (vk == VK_F1) {
        if (g_menuOpen) Log("menu: opened at %s", Where());
        else Log("menu: closed at %s", Where());
        return;
    }
    if (vk == VK_F2) {  // the WndProc already requested the flight toggle
        Log("menu: F2 -> playerFly");
        return;
    }
    if (vk == VK_F3) {  // the WndProc already toggled the triggers
        Log("menu: F3 -> Show triggers %s", g_showTriggers ? "on" : "off");
        return;
    }
    if (!g_menuOpen) return;

    if (s_level == 0) {
        switch (vk) {
        case VK_UP: s_cat = (s_cat + s_numCats - 1) % s_numCats; break;
        case VK_DOWN: s_cat = (s_cat + 1) % s_numCats; break;
        case VK_RETURN:
        case VK_RIGHT:
            s_level = 1;
            Log("menu: open %s", Where());
            break;
        case VK_ESCAPE:
        case VK_BACK:
            Input_SetMenuOpen(false);
            Log("menu: closed at %s", Where());
            break;
        }
        return;
    }

    const Command *items[64];
    int n = ItemsOf(s_cat, items, 64);
    int &sel = s_itemSel[s_cat];
    if (n == 0) {
        s_level = 0;
        return;
    }
    if (sel >= n) sel = n - 1;
    const Command *c = items[sel];
    switch (vk) {
    case VK_UP: sel = (sel + n - 1) % n; break;
    case VK_DOWN: sel = (sel + 1) % n; break;
    case VK_RETURN:
        Log("menu: execute %s/%s", c->category, c->name);
        Execute(c);
        break;
    case VK_LEFT:
        if (c->type == CT_ACTION) s_level = 0;
        else Change(c, -1);
        break;
    case VK_RIGHT:
        if (c->type != CT_ACTION) Change(c, +1);
        break;
    case VK_ESCAPE:
    case VK_BACK:
        s_level = 0;
        Log("menu: back to %s", Where());
        break;
    }
}

// ---- drawing ----
static const ImU32 kHighlight = IM_COL32(40, 90, 160, 200);
static const ImVec4 kTitle(1.00f, 0.80f, 0.30f, 1.0f);
static const ImVec4 kOn(0.40f, 1.00f, 0.40f, 1.0f);
static const ImVec4 kOff(0.60f, 0.62f, 0.65f, 1.0f);
static const ImVec4 kStatus(1.00f, 0.90f, 0.40f, 1.0f);

static const ImGuiWindowFlags kPanelFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
                                            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                            ImGuiWindowFlags_NoScrollbar;

// one menu row: highlight bar + "> " marker + label + right-aligned value
static void Row(bool selected, const char *label, const char *value, const ImVec4 &valueColor)
{
    ImVec2 p = ImGui::GetCursorScreenPos();
    float x0 = ImGui::GetCursorPosX();
    float w = ImGui::GetContentRegionAvail().x;
    float h = ImGui::GetTextLineHeightWithSpacing();
    if (selected) ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x - 4, p.y), ImVec2(p.x + w + 4, p.y + h), kHighlight);
    ImGui::Text("%s%s", selected ? "> " : "  ", label);
    if (value && value[0]) {
        float vw = ImGui::CalcTextSize(value).x;
        ImGui::SameLine(x0 + w - vw);
        ImGui::TextColored(valueColor, "%s", value);
    }
}

static void DrawMenu()
{
    float fs = ImGui::GetFontSize();
    float width = fs * 26.0f * s_textScale;
    ImGui::SetNextWindowPos(ImVec2(fs * 1.5f, fs * 1.5f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);  // height auto-fits
    ImGui::SetNextWindowBgAlpha(s_panelAlpha);
    ImGui::Begin("##dbgmenu", NULL, kPanelFlags);
    ImGui::SetWindowFontScale(s_textScale);

    ImGui::TextColored(kTitle, "DEBUG MENU");
    ImGui::SameLine();
    ImGui::TextDisabled("Darksiders II PC");
    ImGui::TextDisabled(s_level == 0 ? "Main" : "Main > %s", s_cats[s_cat]);
    ImGui::Separator();

    const char *desc = NULL;
    if (s_level == 0) {
        const Command *items[64];
        for (int c = 0; c < s_numCats; c++) {
            char cnt[16];
            sprintf_s(cnt, "%d >", ItemsOf(c, items, 64));
            Row(c == s_cat, s_cats[c], cnt, kOff);
        }
    } else {
        const Command *items[64];
        int n = ItemsOf(s_cat, items, 64);
        int sel = s_itemSel[s_cat] < n ? s_itemSel[s_cat] : n - 1;
        for (int i = 0; i < n; i++) {
            const Command *c = items[i];
            char label[128], value[48] = "";
            ImVec4 col = kOff;
            if (c->shortcut) sprintf_s(label, "%s (shortcut %s)", c->name, c->shortcut);
            else sprintf_s(label, "%s", c->name);
            if (c->type == CT_TOGGLE) {
                sprintf_s(value, "[%s]", *c->flag ? "on" : "off");
                col = *c->flag ? kOn : kOff;
            } else if (c->type == CT_FLOAT) {
                char num[32];
                sprintf_s(num, c->fmt ? c->fmt : "%.2f", *c->value);
                sprintf_s(value, "< %s >", num);
                col = kOn;
            } else if (c->type == CT_ENUM) {
                sprintf_s(value, "< %s >", c->names[*c->choice]);
                col = kOn;
            } else if (c->action == Cmd_PlayerFly && g_flying) {
                sprintf_s(value, "[flying]");
                col = kOn;
            }
            Row(i == sel, label, value, col);
            if (i == sel) desc = c->desc;
        }
    }

    ImGui::Separator();
    if (desc) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", desc);
        ImGui::PopTextWrapPos();
    }
    if (StatusActive()) ImGui::TextColored(kStatus, "%s", s_status);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled(s_level == 0 ? "Up/Down select  Enter open  Esc close"
                                     : "Up/Down select  Enter run  Left/Right change  Esc back");
    ImGui::PopTextWrapPos();
    ImGui::End();
}

// status line when the menu is closed (e.g. F2)
static void DrawToast()
{
    float fs = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(ImVec2(fs * 1.5f, fs * 1.5f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(s_panelAlpha);
    ImGui::Begin("##dbgtoast", NULL, kPanelFlags | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::TextColored(kStatus, "%s", s_status);
    ImGui::End();
}

// playerFly / hover status line, bottom centre
static void DrawFlyStatus()
{
    GameVec p;
    bool flying = Flight_GetCamPos(&p);
    if (!flying && !Flight_Hovering()) return;
    float fs = ImGui::GetFontSize();
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y - fs * 1.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowBgAlpha(s_panelAlpha * 0.85f);
    ImGui::Begin("##dbgfly", NULL, kPanelFlags | ImGuiWindowFlags_AlwaysAutoResize);
    if (!flying) {  // the ImGui font has no em dash: plain hyphen
        ImGui::TextColored(kTitle, "HOVER");
        ImGui::SameLine();
        ImGui::Text("- Space: jump, move/attack: let go");
        ImGui::SameLine();
        ImGui::TextDisabled("[F2 - fly again]");
    } else if (Flight_NoFocus()) {
        ImGui::TextColored(kTitle, "FLY");
        ImGui::SameLine();
        ImGui::Text("(paused, no focus)");
    } else {
        bool frozen = Flight_TimeFrozen();
        ImGui::TextColored(kTitle, "FLY");
        ImGui::SameLine();
        ImGui::Text(" speed \xC3\x97%g  pos (%.0f %.0f %.0f) ", Flight_SpeedMul(), p.x, p.y, p.z);
        ImGui::SameLine();
        ImGui::TextColored(frozen ? kStatus : kOn, "time: %s", frozen ? "frozen" : "running");
        ImGui::SameLine();
        if (Flight_Attached()) ImGui::TextColored(kOn, " attached: dist %.0f ", g_attachDist);
        else ImGui::Text(" free ");
        ImGui::SameLine();
        ImGui::TextDisabled(g_menuOpen ? "[menu open: flight stopped, F1 closes the menu]"
                                       : "[R attach \xC2\xB7 T time \xC2\xB7 Enter/LMB teleport \xC2\xB7 F2 exit]");
    }
    ImGui::End();
}

void Menu_Draw()
{
    InitCategories();
    DrawFlyStatus();
    if (g_menuOpen) DrawMenu();
    else if (StatusActive()) DrawToast();
}

// ---- showCameraPosition panel ----
static void VecLine(const char *label, bool ok, const GameVec &v, const char *src)
{
    if (ok) ImGui::Text("  %-5s %10.2f %10.2f %10.2f  %s", label, v.x, v.y, v.z, src);
    else ImGui::TextDisabled("  %-5s %10s %10s %10s  %s", label, "-", "-", "-", src);
}

// log the first reading and every status change (for the in-game check-list)
static void LogTransitions(const GameInfo &gi)
{
    static int lastGame = -1, lastActor = -1, lastCam = -1;
    if (gi.gameSt == lastGame && gi.actorSt == lastActor && gi.camSt == lastCam) return;
    lastGame = gi.gameSt; lastActor = gi.actorSt; lastCam = gi.camSt;
    Log("info: game=%08X %s | actor=%08X %s vt=%08X getPos=%08X setPos=%08X setYaw=%08X pos=(%.2f %.2f %.2f) raw+90=(%.2f %.2f %.2f) | "
        "camera=%08X %s vt=%08X getPos=%08X pos=(%.2f %.2f %.2f) dir=(%.3f %.3f %.3f) raw+B0=(%.2f %.2f %.2f)%s",
        (unsigned)(UINT_PTR)gi.game, StName(gi.gameSt),
        (unsigned)(UINT_PTR)gi.actor, StName(gi.actorSt), (unsigned)(UINT_PTR)gi.actorVt, (unsigned)(UINT_PTR)gi.actorGetPos,
        (unsigned)(UINT_PTR)gi.actorSetPos, (unsigned)(UINT_PTR)gi.actorSetYaw, gi.actorPos.x, gi.actorPos.y, gi.actorPos.z, gi.actorRaw.x, gi.actorRaw.y, gi.actorRaw.z,
        (unsigned)(UINT_PTR)gi.cam, StName(gi.camSt), (unsigned)(UINT_PTR)gi.camVt, (unsigned)(UINT_PTR)gi.camGetPos,
        gi.camPos.x, gi.camPos.y, gi.camPos.z, gi.camDir.x, gi.camDir.y, gi.camDir.z, gi.camRaw.x, gi.camRaw.y, gi.camRaw.z,
        gi.exCode ? " (exception)" : "");
    if (gi.exCode) Log("info: last exception %08X at %08X", gi.exCode, gi.exAddr);
}

void Info_Draw()
{
    if (!g_showCameraPosition) return;
    GameInfo gi;
    Game_Read(&gi);
    LogTransitions(gi);

    float fs = ImGui::GetFontSize();
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - fs * 1.5f, fs * 1.5f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(s_panelAlpha * 0.85f);
    ImGui::Begin("##dbginfo", NULL, kPanelFlags | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::TextColored(kTitle, "showCameraPosition");
    if (!g_gameMatch) ImGui::TextDisabled("unknown exe: game objects are not read");
    ImGui::Text("game   %08X  %s", (unsigned)(UINT_PTR)gi.game, StName(gi.gameSt));
    ImGui::Text("player %08X  %s  vt %08X", (unsigned)(UINT_PTR)gi.actor, StName(gi.actorSt), (unsigned)(UINT_PTR)gi.actorVt);
    VecLine("pos", gi.actorPosOk, gi.actorPos, "vt[0x38]");
    VecLine("+0x90", gi.actorSt == ST_OK || gi.actorSt == ST_BADPTR, gi.actorRaw, "raw");
    if (gi.actorVt) ImGui::TextDisabled("  getPos %08X  setPos %08X  setYaw %08X", (unsigned)(UINT_PTR)gi.actorGetPos,
                                        (unsigned)(UINT_PTR)gi.actorSetPos, (unsigned)(UINT_PTR)gi.actorSetYaw);
    ImGui::Text("camera %08X  %s  vt %08X", (unsigned)(UINT_PTR)gi.cam, StName(gi.camSt), (unsigned)(UINT_PTR)gi.camVt);
    VecLine("pos", gi.camPosOk, gi.camPos, "vt[0x44]");
    VecLine("dir", gi.camPosOk, gi.camDir, "vt[0x40]");
    VecLine("+0xB0", gi.camSt == ST_OK || gi.camSt == ST_BADPTR, gi.camRaw, "raw");
    GameVec fly;
    if (Flight_GetCamPos(&fly)) VecLine("fly", true, fly, "free camera");
    if (gi.exCode) ImGui::TextColored(kStatus, "exception %08X at %08X", gi.exCode, gi.exAddr);
    ImGui::End();
}

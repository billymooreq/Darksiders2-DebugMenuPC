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

enum CmdType { CT_ACTION, CT_TOGGLE, CT_FLOAT };
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
};

static void Cmd_PlayerFly();
static void Cmd_ShowTriggers();
static void On_ShowCameraPosition();

// Command registry (like the Xbox console registry): one line = one menu item; categories in order of appearance.
static const Command g_commands[] = {
    {"General", "playerFly", "Allow player to float in the air for debugging (free camera, teleport). Task 04.",
     CT_ACTION, Cmd_PlayerFly, NULL, NULL, 0, 0, 0, "F2"},
    {"Camera", "showCameraPosition", "Display the current position of the player and the camera.",
     CT_TOGGLE, On_ShowCameraPosition, &g_showCameraPosition, NULL, 0, 0, 0, NULL},
    {"Graphics", "Show triggers", "Draw trigger volumes (EventManager detectors). Task 05.",
     CT_ACTION, Cmd_ShowTriggers, NULL, NULL, 0, 0, 0, NULL},
    {"Menu", "Panel opacity", "Background opacity of the menu panel.",
     CT_FLOAT, NULL, NULL, &s_panelAlpha, 0.30f, 1.00f, 0.05f, NULL},
    {"Menu", "Text scale", "Size of the menu text.",
     CT_FLOAT, NULL, NULL, &s_textScale, 0.75f, 2.00f, 0.25f, NULL},
};
static const int kNumCommands = sizeof(g_commands) / sizeof(g_commands[0]);

static const char *s_cats[16];
static int s_numCats;
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

bool Menu_WantsDraw() { return g_menuOpen || g_showCameraPosition || StatusActive(); }

// ---- commands ----
static void Cmd_PlayerFly() { Menu_Status("playerFly: not implemented yet"); }
static void Cmd_ShowTriggers() { Menu_Status("Show triggers: not implemented yet"); }
static void On_ShowCameraPosition() { Menu_Status("Show Camera Position %s", g_showCameraPosition ? "on" : "off"); }

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
    }
}

static void Change(const Command *c, int dir)
{
    if (c->type == CT_TOGGLE) {
        bool v = dir > 0;
        if (*c->flag != v) {
            *c->flag = v;
            if (c->action) c->action();
        }
    } else if (c->type == CT_FLOAT) {
        float v = *c->value + dir * c->step;
        *c->value = v < c->vmin ? c->vmin : (v > c->vmax ? c->vmax : v);
    }
}

// ---- keys (render thread; F1 itself already toggled g_menuOpen in the WndProc) ----
void Menu_HandleKey(int vk)
{
    InitCategories();
    if (vk == VK_F1) {
        Log("menu: %s", g_menuOpen ? "opened" : "closed");
        return;
    }
    if (vk == VK_F2) {
        Log("menu: F2 -> playerFly");
        Cmd_PlayerFly();
        return;
    }
    if (!g_menuOpen) return;

    if (s_level == 0) {
        switch (vk) {
        case VK_UP: s_cat = (s_cat + s_numCats - 1) % s_numCats; break;
        case VK_DOWN: s_cat = (s_cat + 1) % s_numCats; break;
        case VK_RETURN:
        case VK_RIGHT: s_level = 1; break;
        case VK_ESCAPE:
        case VK_BACK:
            Input_SetMenuOpen(false);
            Log("menu: closed");
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
    case VK_BACK: s_level = 0; break;
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
                sprintf_s(value, "< %.2f >", *c->value);
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

void Menu_Draw()
{
    InitCategories();
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
    if (gi.exCode) ImGui::TextColored(kStatus, "exception %08X at %08X", gi.exCode, gi.exAddr);
    ImGui::End();
}

// Debug menu in the style of the Xbox DEBUG build console menu (Console::draw 0x82A76DE8):
// dark translucent panel, categories -> items, "> " + highlight bar on the current row,
// toggles show their state, items with a hotkey show " (shortcut X)".
// Task 10: everything is drawn only while the debug menu is on (F10; a small label at the top edge of the screen);
// section Keys changes the hotkeys (keys.cpp); every key shown on screen is the current one ({menu} etc. below).
#include "common.h"
#include <stdio.h>
#include <stdarg.h>
#include <float.h>
#include "imgui.h"

bool g_showCameraPosition;
volatile LONG g_keyCapture;
static float s_panelAlpha = 0.80f;
static float s_textScale = 1.00f;

enum CmdType { CT_ACTION, CT_TOGGLE, CT_FLOAT, CT_ENUM, CT_KEY };
struct Command {
    const char *category;
    const char *name;
    const char *desc;           // {debug} {menu} {fly} {triggers} {attach} {time} = the current key
    CmdType type;
    void (*action)();  // CT_ACTION: run; CT_TOGGLE: called after the flag changed
    bool *flag;        // CT_TOGGLE
    float *value;      // CT_FLOAT (Left/Right)
    float vmin, vmax, step;
    int key;                    // KeyAction: CT_ACTION / CT_TOGGLE show " (shortcut X)"; CT_KEY: the action it changes
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
static void Cmd_KeysDefault();

static const char *const kHoverJumpNames[] = {"game", "direct"};  // order of HJ_GAME / HJ_DIRECT
static const char *const kTrigDepthNames[] = {"through walls", "depth"};  // order of TD_WALLS / TD_DEPTH

#define KEY_DESC "Enter, then press the new key (Esc - cancel). A key another action has is swapped. Not allowed: " \
                 "Esc, Enter, arrows, W A S D C, Space, Shift, Ctrl, Alt, Win, Tab, Backspace, F4, Print Screen, " \
                 "mouse buttons. Saved in dbgmenu_keys.ini next to dinput8.dll."

// Command registry (like the Xbox console registry): one line = one menu item.
// Task 08: one category = one feature, all its settings inside it. Categories are shown in order of their first
// appearance here, items in their order inside the category.
//   new item    -> one line with the category of its feature (keep it next to the other lines of that category);
//   new section -> lines with a new category name, placed where the section should appear (max 16 sections).
static const Command g_commands[] = {
    // ---- playerFly ----
    {"playerFly", "playerFly", "Allow player to float in the air for debugging. WASD + mouse fly, Shift x3, Ctrl x0.5, "
     "Space/C up/down, Enter or left click: teleport the player here, {attach}: carry the player, {time}: freeze / "
     "run time, {fly}: back. A carried player stays where he is on {fly}, Enter or click.",
     CT_ACTION, Cmd_PlayerFly, NULL, NULL, 0, 0, 0, KA_FLY},
    {"playerFly", "freeze time in flight", "On: the game is paused while you fly (as before). Off: the world, enemies "
     "and triggers keep running. {time} switches it during a flight.",
     CT_TOGGLE, On_FreezeTime, &g_flyFreezeTime, NULL, 0, 0, 0, KA_NONE},
    {"playerFly", "attach player", "The flight starts with the player carried in front of the camera: he passes "
     "through walls, does not fall and touches triggers; Enter, click or {fly} leave him there. {attach} switches it "
     "in flight.",
     CT_TOGGLE, On_Attach, &g_flyAttach, NULL, 0, 0, 0, KA_NONE},
    {"playerFly", "attach distance", "How far in front of the camera the carried player is.",
     CT_FLOAT, NULL, NULL, &g_attachDist, 100.0f, 600.0f, 50.0f, KA_NONE, NULL, NULL, 0, "%.0f"},
    {"playerFly", "hover after teleport", "After the teleport the player hangs in the air until you press a move, "
     "jump or attack key (the camera works). Off: he falls at once.",
     CT_TOGGLE, On_Hover, &g_hoverAfterTeleport, NULL, 0, 0, 0, KA_NONE},
    {"playerFly", "hover jump", "Space while hanging: a normal jump, then the double jump. game: the game jumps by "
     "itself (its jump from a fall is kept ready); direct: the menu starts the jump. Also after W/attack: one jump "
     "in the air while falling.",
     CT_ENUM, NULL, NULL, NULL, 0, 0, 0, KA_NONE, &g_hoverJumpMode, kHoverJumpNames, 2},
    // ---- Triggers ----
    {"Triggers", "Show triggers", "Draw the live trigger volumes (detectors of the loaded level): magenta = "
     "TriggerRegion, cyan = other detectors, gray = switched off.",
     CT_TOGGLE, On_ShowTriggers, &g_showTriggers, NULL, 0, 0, 0, KA_TRIGGERS},
    {"Triggers", "depth", "through walls: the lines are on top of everything. depth: walls hide the lines "
     "(drawn into the 3D scene before the HUD).",
     CT_ENUM, On_TrigDepth, NULL, NULL, 0, 0, 0, KA_NONE, &g_trigDepthMode, kTrigDepthNames, 2},
    {"Triggers", "show disabled", "Also draw the switched-off triggers (gray, paler).",
     CT_TOGGLE, On_ShowDisabled, &g_trigShowDisabled, NULL, 0, 0, 0, KA_NONE},
    // ---- Camera ----
    {"Camera", "showCameraPosition", "Display the current position of the player and the camera.",
     CT_TOGGLE, On_ShowCameraPosition, &g_showCameraPosition, NULL, 0, 0, 0, KA_NONE},
    // ---- Menu ----
    {"Menu", "Panel opacity", "Background opacity of the menu panel.",
     CT_FLOAT, NULL, NULL, &s_panelAlpha, 0.30f, 1.00f, 0.05f, KA_NONE},
    {"Menu", "Text scale", "Size of the menu text.",
     CT_FLOAT, NULL, NULL, &s_textScale, 0.75f, 2.00f, 0.25f, KA_NONE},
    // ---- Keys (task 10): the hotkeys, kept in dbgmenu_keys.ini ----
    {"Keys", "debug on/off", "Turns the whole debug menu on and off. Off (after the game starts): nothing on the "
     "screen, every other key goes to the game. Off again ends a flight like {fly} and hides the triggers and "
     "panels. " KEY_DESC,
     CT_KEY, NULL, NULL, NULL, 0, 0, 0, KA_DEBUG},
    {"Keys", "menu", "Opens and closes this menu. " KEY_DESC, CT_KEY, NULL, NULL, NULL, 0, 0, 0, KA_MENU},
    {"Keys", "playerFly", "Starts and ends the flight. " KEY_DESC, CT_KEY, NULL, NULL, NULL, 0, 0, 0, KA_FLY},
    {"Keys", "Show triggers", "Shows and hides the triggers. " KEY_DESC, CT_KEY, NULL, NULL, NULL, 0, 0, 0, KA_TRIGGERS},
    {"Keys", "attach (in flight)", "In a flight: carry the player in front of the camera / let him go. " KEY_DESC,
     CT_KEY, NULL, NULL, NULL, 0, 0, 0, KA_ATTACH},
    {"Keys", "time (in flight)", "In a flight: freeze / run the time. " KEY_DESC,
     CT_KEY, NULL, NULL, NULL, 0, 0, 0, KA_TIME},
    {"Keys", "reset to defaults", "All keys back to F10, F1, F2, F3, R, T (the file is rewritten).",
     CT_ACTION, Cmd_KeysDefault, NULL, NULL, 0, 0, 0, KA_NONE},
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

// task 10: "{fly}" -> the current key of that action ("F2")
static void ExpandKeys(const char *src, char *dst, int size)
{
    static const char *const kTokens[KA_COUNT] = {"", "{debug}", "{menu}", "{fly}", "{triggers}", "{attach}", "{time}"};
    int o = 0;
    while (*src && o < size - 1) {
        int a = KA_COUNT;
        if (*src == '{')
            for (a = 1; a < KA_COUNT; a++)
                if (strncmp(src, kTokens[a], strlen(kTokens[a])) == 0) break;
        if (a < KA_COUNT) {
            const char *k = Keys_KeyName(a);
            while (*k && o < size - 1) dst[o++] = *k++;
            src += strlen(kTokens[a]);
        } else {
            dst[o++] = *src++;
        }
    }
    dst[o] = 0;
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

// task 10: the debug menu off shows only the hover line (the player may still hang after a flight ended by F10)
bool Menu_WantsDraw()
{
    if (!g_debugOn) return g_flying || Flight_Hovering();
    return true;  // the label at the top edge
}

static void CaptureEnd()
{
    InterlockedExchange(&g_keyCapture, 0);
}

void Menu_OnDebugSwitch(bool on)
{
    if (g_keyCapture) Log("menu: key change cancelled (debug %s)", on ? "on" : "off");
    CaptureEnd();
    s_status[0] = 0;  // no old status line when it comes back
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
static void Cmd_KeysDefault()
{
    Keys_ResetDefaults();
    Menu_Status("keys: defaults (%s debug, %s menu, %s playerFly, %s triggers, %s attach, %s time)",
                Keys_KeyName(KA_DEBUG), Keys_KeyName(KA_MENU), Keys_KeyName(KA_FLY), Keys_KeyName(KA_TRIGGERS),
                Keys_KeyName(KA_ATTACH), Keys_KeyName(KA_TIME));
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
    case CT_KEY:  // task 10: the next key press goes to us (input.cpp)
        InterlockedExchange(&g_keyCapture, c->key);
        Log("menu: Keys > %s: waiting for a key (now %s)", c->name, Keys_KeyName(c->key));
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

// task 10: a key pressed while the menu Keys waits for one
static void CaptureKey(int vk)
{
    int a = g_keyCapture, swapped = KA_NONE;
    if (!a) return;
    if (vk == VK_ESCAPE) {
        CaptureEnd();
        Menu_Status("%s: key not changed (%s)", Keys_Label(a), Keys_KeyName(a));
        return;
    }
    if (!Keys_Allowed(vk)) {
        Menu_Status("key not allowed: %s - press another key (Esc - cancel)", Keys_Name(vk));
        return;  // still waiting
    }
    char old[24];
    strcpy_s(old, Keys_KeyName(a));
    Keys_Assign(a, vk, &swapped);
    CaptureEnd();
    if (swapped) Menu_Status("%s = %s, %s = %s (swapped)", Keys_Label(a), Keys_KeyName(a), Keys_Label(swapped),
                             Keys_KeyName(swapped));
    else if (strcmp(old, Keys_KeyName(a)) == 0) Menu_Status("%s = %s (unchanged)", Keys_Label(a), Keys_KeyName(a));
    else Menu_Status("%s = %s (was %s)", Keys_Label(a), Keys_KeyName(a), old);
}

// ---- keys (render thread; the hotkeys were already run in the WndProc) ----
void Menu_HandleKey(int vk)
{
    InitCategories();
    if (vk & KQ_CAPTURE) {
        if (g_menuOpen) CaptureKey(vk & 0xFF);
        return;
    }
    if (vk & KQ_ACTION) {
        int a = vk & 0xFF;
        if (a == KA_DEBUG) Log("menu: %s -> debug %s", Keys_KeyName(a), g_debugOn ? "on" : "off");
        else if (a == KA_MENU) Log(g_menuOpen ? "menu: opened at %s" : "menu: closed at %s", Where());
        else if (a == KA_FLY) Log("menu: %s -> playerFly", Keys_KeyName(a));
        else if (a == KA_TRIGGERS) Log("menu: %s -> Show triggers %s", Keys_KeyName(a), g_showTriggers ? "on" : "off");
        return;
    }
    if (!g_menuOpen || g_keyCapture) return;

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
        if (c->type == CT_ACTION || c->type == CT_KEY) s_level = 0;
        else Change(c, -1);
        break;
    case VK_RIGHT:
        if (c->type != CT_ACTION && c->type != CT_KEY) Change(c, +1);
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
            char label[128], value[64] = "";
            ImVec4 col = kOff;
            if (c->key && c->type != CT_KEY) sprintf_s(label, "%s (shortcut %s)", c->name, Keys_KeyName(c->key));
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
            } else if (c->type == CT_KEY) {  // the ImGui font has no ellipsis: three dots
                if (g_keyCapture == c->key) sprintf_s(value, "[press a key... (Esc - cancel)]");
                else sprintf_s(value, "[%s]", Keys_KeyName(c->key));
                col = g_keyCapture == c->key ? kStatus : kOn;
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
        char text[1024];
        ExpandKeys(desc, text, sizeof(text));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", text);
        ImGui::PopTextWrapPos();
    }
    if (StatusActive()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(kStatus, "%s", s_status);
        ImGui::PopTextWrapPos();
    }
    ImGui::PushTextWrapPos(0.0f);
    if (g_keyCapture) ImGui::TextDisabled("Press the new key  Esc cancel");
    else if (s_level == 0) ImGui::TextDisabled("Up/Down select  Enter open  Esc/%s close", Keys_KeyName(KA_MENU));
    else ImGui::TextDisabled("Up/Down select  Enter run  Left/Right change  Esc back");
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

// task 10: the debug menu is on - a small label at the very top edge, centred: above the menu / toast panels
// (they start at 1.5 font heights), away from the showCameraPosition panel (top right, same 1.5 font heights),
// the flight line (bottom centre) and the triggers line (bottom left)
static void DrawDebugLabel()
{
    float fs = ImGui::GetFontSize();
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 0.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.35f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(fs * 0.3f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##dbglabel", NULL, kPanelFlags | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::SetWindowFontScale(0.75f);
    ImGui::TextColored(ImVec4(0.75f, 0.77f, 0.80f, 0.85f), "DEBUG MENU ON  (%s menu \xC2\xB7 %s off)",
                       Keys_KeyName(KA_MENU), Keys_KeyName(KA_DEBUG));
    ImGui::End();
    ImGui::PopStyleVar(2);
}

// playerFly / hover status line, bottom centre
static void DrawFlyStatus()
{
    GameVec p;
    bool flying = Flight_GetCamPos(&p);
    if (!flying && !Flight_Hovering()) return;
    float fs = ImGui::GetFontSize();
    ImGuiIO &io = ImGui::GetIO();
    char hint[160];
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y - fs * 1.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowBgAlpha(s_panelAlpha * 0.85f);
    ImGui::Begin("##dbgfly", NULL, kPanelFlags | ImGuiWindowFlags_AlwaysAutoResize);
    if (!flying) {  // the ImGui font has no em dash: plain hyphen
        ImGui::TextColored(kTitle, "HOVER");
        ImGui::SameLine();
        ImGui::Text("- Space: jump, move/attack: let go");
        if (g_debugOn) {  // task 10: the debug menu off -> the playerFly key belongs to the game
            ImGui::SameLine();
            ExpandKeys("[{fly} - fly again]", hint, sizeof(hint));
            ImGui::TextDisabled("%s", hint);
        }
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
        GameVec ap;
        bool attached = Flight_AttachedPos(&ap);
        if (attached) ImGui::TextColored(kOn, " attached: dist %.0f  player (%.0f %.0f %.0f) ", g_attachDist, ap.x, ap.y, ap.z);
        else ImGui::Text(" free ");
        ImGui::SameLine();
        ExpandKeys(g_menuOpen ? "[menu open: flight stopped, {menu} closes the menu]"
                   : attached ? "[{attach} let go \xC2\xB7 {time} time \xC2\xB7 Enter/LMB/{fly} - leave the player here]"
                              : "[{attach} attach \xC2\xB7 {time} time \xC2\xB7 Enter/LMB teleport \xC2\xB7 {fly} exit]",
                   hint, sizeof(hint));
        ImGui::TextDisabled("%s", hint);
    }
    ImGui::End();
}

void Menu_Draw()
{
    InitCategories();
    if (g_keyCapture && !g_menuOpen) {  // the menu was closed while waiting for a key
        Log("menu: key change cancelled (menu closed)");
        CaptureEnd();
    }
    DrawFlyStatus();
    if (!g_debugOn) return;
    DrawDebugLabel();
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

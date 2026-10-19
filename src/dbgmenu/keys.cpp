// Task 10: the master switch (F10) and the hotkeys the player can change (menu Keys, notes/menu_pc.md §6).
// After the game starts the debug menu is OFF: no menu, no F2/F3, nothing on screen, every key and the mouse go to
// the game; only the debug key is ours. On: a small label at the top of the screen, the hotkeys work. Off again:
// the menu closes, a flight ends like F2, triggers and showCameraPosition go off (settings inside the sections
// stay as they are until the game exits).
// Hotkeys: debug, menu, playerFly, Show triggers (always ours while the debug menu is on) and attach / time (only
// in a flight, when the game gets no keys anyway). They live in dbgmenu_keys.ini next to dinput8.dll: created with
// the defaults when it is missing, read at start, rewritten on every change in the menu. Movement in the flight
// (W A S D, Space, C, Shift, Ctrl), Enter / mouse, Esc and the menu navigation are fixed and cannot be assigned.
#include "common.h"
#include <stdio.h>
#include <string.h>

volatile LONG g_debugOn;

// task 11: + info: target (Monsters), default F5
static const int kDefaults[KA_COUNT] = {0, VK_F10, VK_F1, VK_F2, VK_F3, 'R', 'T', VK_F5};
static int s_keys[KA_COUNT] = {0, VK_F10, VK_F1, VK_F2, VK_F3, 'R', 'T', VK_F5};
static const char *const kIniNames[KA_COUNT] = {"", "debug", "menu", "playerFly", "showTriggers", "attach", "time",
                                                "infoTarget"};
static const char *const kLabels[KA_COUNT] = {"", "debug on/off", "menu", "playerFly", "Show triggers",
                                              "attach (in flight)", "time (in flight)", "info: target"};
static char s_path[MAX_PATH];

// keys that can be assigned (name = the name in the file)
struct KeyName { int vk; const char *name; };
static const KeyName kNames[] = {
    {VK_F1, "F1"}, {VK_F2, "F2"}, {VK_F3, "F3"}, {VK_F5, "F5"}, {VK_F6, "F6"}, {VK_F7, "F7"}, {VK_F8, "F8"},
    {VK_F9, "F9"}, {VK_F10, "F10"}, {VK_F11, "F11"}, {VK_F12, "F12"}, {VK_F13, "F13"}, {VK_F14, "F14"},
    {VK_F15, "F15"}, {VK_F16, "F16"}, {VK_F17, "F17"}, {VK_F18, "F18"}, {VK_F19, "F19"}, {VK_F20, "F20"},
    {VK_F21, "F21"}, {VK_F22, "F22"}, {VK_F23, "F23"}, {VK_F24, "F24"},
    {'B', "B"}, {'E', "E"}, {'F', "F"}, {'G', "G"}, {'H', "H"}, {'I', "I"}, {'J', "J"}, {'K', "K"}, {'L', "L"},
    {'M', "M"}, {'N', "N"}, {'O', "O"}, {'P', "P"}, {'Q', "Q"}, {'R', "R"}, {'T', "T"}, {'U', "U"}, {'V', "V"},
    {'X', "X"}, {'Y', "Y"}, {'Z', "Z"},
    {'0', "0"}, {'1', "1"}, {'2', "2"}, {'3', "3"}, {'4', "4"}, {'5', "5"}, {'6', "6"}, {'7', "7"}, {'8', "8"},
    {'9', "9"},
    {VK_NUMPAD0, "Numpad0"}, {VK_NUMPAD1, "Numpad1"}, {VK_NUMPAD2, "Numpad2"}, {VK_NUMPAD3, "Numpad3"},
    {VK_NUMPAD4, "Numpad4"}, {VK_NUMPAD5, "Numpad5"}, {VK_NUMPAD6, "Numpad6"}, {VK_NUMPAD7, "Numpad7"},
    {VK_NUMPAD8, "Numpad8"}, {VK_NUMPAD9, "Numpad9"}, {VK_MULTIPLY, "NumpadMultiply"}, {VK_ADD, "NumpadAdd"},
    {VK_SUBTRACT, "NumpadSubtract"}, {VK_DECIMAL, "NumpadDecimal"}, {VK_DIVIDE, "NumpadDivide"},
    {VK_INSERT, "Insert"}, {VK_DELETE, "Delete"}, {VK_HOME, "Home"}, {VK_END, "End"}, {VK_PRIOR, "PageUp"},
    {VK_NEXT, "PageDown"},
    {VK_OEM_MINUS, "Minus"}, {VK_OEM_PLUS, "Equals"}, {VK_OEM_4, "LeftBracket"}, {VK_OEM_6, "RightBracket"},
    {VK_OEM_1, "Semicolon"}, {VK_OEM_7, "Apostrophe"}, {VK_OEM_5, "Backslash"}, {VK_OEM_COMMA, "Comma"},
    {VK_OEM_PERIOD, "Period"}, {VK_OEM_2, "Slash"}, {VK_OEM_3, "Backquote"},
};
static const int kNumNames = sizeof(kNames) / sizeof(kNames[0]);

bool Keys_Allowed(int vk)
{
    for (int i = 0; i < kNumNames; i++)
        if (kNames[i].vk == vk) return true;
    return false;
}

// any key, also the ones that cannot be assigned (for "key not allowed: Space")
const char *Keys_Name(int vk)
{
    static char buf[4][24];
    static int n;
    for (int i = 0; i < kNumNames; i++)
        if (kNames[i].vk == vk) return kNames[i].name;
    switch (vk) {
    case VK_ESCAPE: return "Esc";
    case VK_RETURN: return "Enter";
    case VK_SPACE: return "Space";
    case VK_BACK: return "Backspace";
    case VK_TAB: return "Tab";
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return "Shift";
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return "Ctrl";
    case VK_MENU: case VK_LMENU: case VK_RMENU: return "Alt";
    case VK_LWIN: case VK_RWIN: return "Win";
    case VK_APPS: return "Menu key";
    case VK_UP: return "Up";
    case VK_DOWN: return "Down";
    case VK_LEFT: return "Left";
    case VK_RIGHT: return "Right";
    case VK_F4: return "F4";
    case VK_SNAPSHOT: return "Print Screen";
    case VK_PAUSE: return "Pause";
    case VK_CAPITAL: return "Caps Lock";
    case VK_NUMLOCK: return "Num Lock";
    case VK_SCROLL: return "Scroll Lock";
    case VK_LBUTTON: return "left mouse button";
    case VK_RBUTTON: return "right mouse button";
    case VK_MBUTTON: return "middle mouse button";
    case VK_XBUTTON1: return "mouse X1";
    case VK_XBUTTON2: return "mouse X2";
    }
    char *b = buf[n++ & 3];
    if (vk >= 'A' && vk <= 'Z') sprintf_s(b, 24, "%c", vk);
    else sprintf_s(b, 24, "key %02X", vk & 0xFF);
    return b;
}

int Keys_Get(int a) { return a > KA_NONE && a < KA_COUNT ? s_keys[a] : 0; }
const char *Keys_KeyName(int a) { return Keys_Name(Keys_Get(a)); }
const char *Keys_Label(int a) { return a > KA_NONE && a < KA_COUNT ? kLabels[a] : ""; }

int Keys_HotkeyNow(int vk)
{
    if (!vk) return KA_NONE;
    if (vk == s_keys[KA_DEBUG]) return KA_DEBUG;
    if (!g_debugOn) return KA_NONE;
    if (vk == s_keys[KA_MENU]) return KA_MENU;
    if (vk == s_keys[KA_FLY]) return KA_FLY;
    if (vk == s_keys[KA_TRIGGERS]) return KA_TRIGGERS;
    if (vk == s_keys[KA_TARGET]) return KA_TARGET;
    return KA_NONE;
}

// ---- file ----
static void LogKeys(const char *what)
{
    char buf[200] = "";
    for (int a = 1; a < KA_COUNT; a++) {
        char one[40];
        sprintf_s(one, "%s%s=%s", a > 1 ? " " : "", kIniNames[a], Keys_Name(s_keys[a]));
        strcat_s(buf, one);
    }
    Log("keys: %s: %s", what, buf);
}

static bool Save(const char *why)
{
    FILE *f = NULL;
    if (!s_path[0] || fopen_s(&f, s_path, "w") != 0 || !f) {
        Log("keys: ERROR cannot write %s (%s)", s_path, why);
        return false;
    }
    fputs("; Darksiders II debug menu (dinput8.dll) - hotkeys.\n"
          "; The menu (section Keys) rewrites this file on every change. You can also edit it in Notepad\n"
          "; while the game is closed. A wrong line gets its default key (the reason is in dbgmenu.log).\n"
          "; Delete this file to go back to the default keys.\n"
          ";\n"
          "; debug        - debug menu on / off (default F10; while it is off the game gets every other key)\n"
          "; menu         - open / close the menu (default F1)\n"
          "; playerFly    - free camera flight (default F2)\n"
          "; showTriggers - draw the trigger volumes (default F3)\n"
          "; attach       - in a flight: carry the player in front of the camera (default R)\n"
          "; time         - in a flight: freeze / run the time (default T)\n"
          "; infoTarget   - Monsters: the info panel of the monster in the centre of the screen (default F5)\n"
          ";\n"
          "; Key names: F1-F24 (not F4), A-Z (not W A S D C), 0-9, Numpad0-Numpad9, NumpadMultiply, NumpadAdd,\n"
          ";   NumpadSubtract, NumpadDecimal, NumpadDivide, Insert, Delete, Home, End, PageUp, PageDown,\n"
          ";   Minus, Equals, LeftBracket, RightBracket, Semicolon, Apostrophe, Backslash, Comma, Period, Slash,\n"
          ";   Backquote. Every action needs its own key.\n"
          "; Not allowed: Esc, Enter, arrows, W A S D C, Space, Shift, Ctrl, Alt, Win, Tab, Backspace, F4,\n"
          ";   Print Screen, mouse buttons.\n"
          "\n",
          f);
    for (int a = 1; a < KA_COUNT; a++) fprintf(f, "%s = %s\n", kIniNames[a], Keys_Name(s_keys[a]));
    fclose(f);
    Log("keys: %s written (%s)", s_path, why);
    return true;
}

static char *Trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

// read dbgmenu_keys.ini: a line that is broken / names an unknown or forbidden key / repeats a key -> default
void Keys_Load()
{
    if (!s_path[0]) {
        char *p;
        GetModuleFileNameA(g_self, s_path, MAX_PATH);
        p = strrchr(s_path, '\\');
        if (p) p[1] = 0;
        else s_path[0] = 0;
        strcat_s(s_path, "dbgmenu_keys.ini");
    }
    FILE *f = NULL;
    if (fopen_s(&f, s_path, "r") != 0 || !f) {
        for (int a = 1; a < KA_COUNT; a++) s_keys[a] = kDefaults[a];
        Log("keys: %s not found -> default keys, the file is created", s_path);
        Save("created with the defaults");
        LogKeys("in use");
        return;
    }
    int want[KA_COUNT] = {0}, lineOf[KA_COUNT] = {0}, problems = 0, missing = 0, n = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        n++;
        char *s = Trim(line);
        if (!*s || *s == ';' || *s == '#' || *s == '[') continue;
        char *eq = strchr(s, '=');
        if (!eq) {
            Log("keys: line %d \"%s\": no '=' -> ignored", n, s);
            problems++;
            continue;
        }
        *eq = 0;
        char *name = Trim(s), *val = Trim(eq + 1);
        char *c = strchr(val, ';');  // a comment after the value
        if (c) *c = 0, val = Trim(val);
        int a = 1;
        while (a < KA_COUNT && _stricmp(name, kIniNames[a]) != 0) a++;
        if (a == KA_COUNT) {
            Log("keys: line %d: unknown action \"%s\" -> ignored", n, name);
            problems++;
            continue;
        }
        if (lineOf[a]) {
            Log("keys: line %d: %s is already set on line %d -> ignored", n, kIniNames[a], lineOf[a]);
            problems++;
            continue;
        }
        lineOf[a] = n;
        int vk = 0;
        for (int i = 0; i < kNumNames && !vk; i++)
            if (_stricmp(val, kNames[i].name) == 0) vk = kNames[i].vk;
        if (!vk) {
            bool known = false;  // a forbidden key written by its own name?
            static const int kForbidden[] = {VK_ESCAPE, VK_RETURN, VK_SPACE, VK_BACK, VK_TAB, VK_SHIFT, VK_CONTROL,
                                             VK_MENU, VK_LWIN, VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_F4, VK_SNAPSHOT,
                                             'W', 'A', 'S', 'D', 'C', VK_LBUTTON, VK_RBUTTON};
            for (int i = 0; i < (int)(sizeof(kForbidden) / sizeof(kForbidden[0])); i++)
                if (_stricmp(val, Keys_Name(kForbidden[i])) == 0) known = true;
            Log("keys: line %d: %s = \"%s\": %s -> default %s", n, kIniNames[a], val,
                known ? "key not allowed" : "unknown key name", Keys_Name(kDefaults[a]));
            problems++;
            continue;
        }
        for (int b = 1; b < KA_COUNT; b++)
            if (b != a && want[b] == vk) {
                Log("keys: line %d: %s = %s: the key is already used by %s (line %d) -> default %s", n, kIniNames[a],
                    val, kIniNames[b], lineOf[b], Keys_Name(kDefaults[a]));
                problems++;
                vk = 0;
                break;
            }
        want[a] = vk;
    }
    fclose(f);
    // the lines that were read win; an action without one gets its default, or (default taken by such a line)
    // the first free default key of another action
    int keys[KA_COUNT] = {0};
    for (int a = 1; a < KA_COUNT; a++) keys[a] = want[a];
    for (int a = 1; a < KA_COUNT; a++) {
        if (keys[a]) continue;
        if (!lineOf[a]) {
            Log("keys: no line for %s -> default", kIniNames[a]);
            problems++;
            if (a >= KA_TARGET) missing++;  // an action newer than v0.9 (a file of an older version)
        }
        for (int d = 0; d < KA_COUNT && !keys[a]; d++) {
            int cand = d == 0 ? kDefaults[a] : kDefaults[d];
            if (!cand) continue;
            bool used = false;
            for (int b = 1; b < KA_COUNT; b++)
                if (keys[b] == cand) used = true;
            if (!used) {
                keys[a] = cand;
                if (d) Log("keys: %s: its default %s is taken -> %s", kIniNames[a], Keys_Name(kDefaults[a]),
                           Keys_Name(cand));
            }
        }
    }
    for (int a = 1; a < KA_COUNT; a++) s_keys[a] = keys[a];
    if (problems && problems == missing) {
        // task 11: only the lines of actions added after v0.9 are missing (the file of an older version) -> the
        // file gets them with the default keys, the player's own lines stay as they are
        Log("keys: %s read (%d lines): %d action%s of a newer version added (keys above)", s_path, n, missing,
            missing == 1 ? "" : "s");
        Save("new actions added");
    } else {
        Log("keys: %s read (%d lines, %d problem%s; the file is not rewritten until a key is changed in the menu)",
            s_path, n, problems, problems == 1 ? "" : "s");
    }
    LogKeys("in use");
}

const char *Keys_FilePath() { return s_path; }

// menu Keys: new key for an action. false = not allowed. *swapped = the action that had that key and got the old one
bool Keys_Assign(int a, int vk, int *swapped)
{
    *swapped = KA_NONE;
    if (a <= KA_NONE || a >= KA_COUNT || !Keys_Allowed(vk)) return false;
    int old = s_keys[a];
    if (old == vk) {
        Log("keys: %s = %s (unchanged)", kIniNames[a], Keys_Name(vk));
        return true;
    }
    for (int b = 1; b < KA_COUNT; b++)
        if (b != a && s_keys[b] == vk) {
            s_keys[b] = old;
            *swapped = b;
        }
    s_keys[a] = vk;
    if (*swapped) Log("keys: %s = %s (was %s); swapped: %s = %s", kIniNames[a], Keys_Name(vk), Keys_Name(old),
                      kIniNames[*swapped], Keys_Name(old));
    else Log("keys: %s = %s (was %s)", kIniNames[a], Keys_Name(vk), Keys_Name(old));
    Save("menu Keys");
    return true;
}

void Keys_ResetDefaults()
{
    for (int a = 1; a < KA_COUNT; a++) s_keys[a] = kDefaults[a];
    LogKeys("reset to defaults");
    Save("menu Keys: reset to defaults");
}

// ---- the master switch (WndProc, game thread) ----
void Debug_Toggle(const char *why)
{
    if (!g_debugOn) {
        InterlockedExchange(&g_debugOn, 1);
        Menu_OnDebugSwitch(true);
        Log("debug: ON (%s) | keys: menu %s, playerFly %s, Show triggers %s, info: target %s, off %s", why,
            Keys_KeyName(KA_MENU), Keys_KeyName(KA_FLY), Keys_KeyName(KA_TRIGGERS), Keys_KeyName(KA_TARGET),
            Keys_KeyName(KA_DEBUG));
        return;
    }
    InterlockedExchange(&g_debugOn, 0);
    bool menu = g_menuOpen != 0, flying = g_flying != 0, trig = g_showTriggers, cam = g_showCameraPosition;
    bool mon = g_monNearby || g_monTarget, frz = g_monFreeze, dmg = g_monDamage;
    Input_SetMenuOpen(false);
    if (flying) Flight_RequestStop();  // like F2, done by the next game frame
    g_showTriggers = false;
    g_showCameraPosition = false;
    Mon_OnDebugOff();  // task 11: info off, monsters move again, the game's DamageIndicators back
    Menu_OnDebugSwitch(false);
    Log("debug: OFF (%s)%s%s%s%s%s%s%s - the game gets every key but %s", why, menu ? " | menu closed" : "",
        flying ? (Flight_Attached() ? " | flight ends, the carried player stays (hover)" : " | flight ends") : "",
        trig ? " | Show triggers off" : "", cam ? " | showCameraPosition off" : "", mon ? " | monsters info off" : "",
        frz ? " | freeze monsters off" : "", dmg ? " | damage info off" : "", Keys_KeyName(KA_DEBUG));
    Cursor_Update();
}

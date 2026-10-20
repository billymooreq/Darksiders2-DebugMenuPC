// Debug menu in the style of the Xbox DEBUG build console menu (Console::draw 0x82A76DE8):
// dark translucent panel, categories -> items, "> " + highlight bar on the current row,
// toggles show their state, items with a hotkey show " (shortcut X)".
// Task 10: everything is drawn only while the debug menu is on (F10; a small label at the top edge of the screen);
// section Keys changes the hotkeys (keys.cpp); every key shown on screen is the current one ({menu} etc. below).
// Task 12: sub-folders inside a section (CT_FOLDER, "Name  N >"; Enter goes in, Esc goes one level up).
// Task 13: Triggers info (nearby / target F6 / range / Display) and the map lines of showCameraPosition.
// Task 15: section Cheats (invincible / one-hit kill / kill all monsters) after Monsters.
#include "common.h"
#include <stdio.h>
#include <stdarg.h>
#include <float.h>
#include "imgui.h"

bool g_showCameraPosition;
volatile LONG g_keyCapture;
static float s_panelAlpha = 0.80f;
static float s_menuBottom;  // task 13: the bottom of the menu panel last frame (0 = closed)
static float s_textScale = 1.00f;

enum CmdType { CT_ACTION, CT_TOGGLE, CT_FLOAT, CT_ENUM, CT_KEY, CT_FOLDER };
struct Command {
    const char *category;       // section, or a folder path inside it: "Monsters/Display" (task 12)
    const char *name;
    const char *desc;           // {debug} {menu} {fly} {triggers} {attach} {time} = the current key
    CmdType type;
    void (*action)();  // CT_ACTION: run; CT_TOGGLE: called after the flag changed (NULL = a plain status line)
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
static void On_MonNearby();
static void On_MonTarget();
static void On_MonDamage();
static void On_MonFreeze();
static void Cmd_MonAllOn();
static void Cmd_MonAllOff();
static void On_TrigNearby();
static void On_TrigTarget();
static void Cmd_TrigAllOn();
static void Cmd_TrigAllOff();
static void On_CheatInvincible();
static void On_CheatOneHit();
static void Cmd_KillAll();

static const char *const kHoverJumpNames[] = {"game", "direct"};  // order of HJ_GAME / HJ_DIRECT
static const char *const kTrigDepthNames[] = {"through walls", "depth"};  // order of TD_WALLS / TD_DEPTH
// task 11: the nearby range (monsters.cpp kRangeM), who is shown (MW_*)
static const char *const kMonRangeNames[] = {"10 m", "20 m", "30 m", "50 m", "75 m", "100 m", "150 m", "unlimited"};
static const char *const kMonWhoNames[] = {"monsters", "monsters + NPC", "all characters"};

#define KEY_DESC "Enter, then press the new key (Esc - cancel). A key another action has is swapped. Not allowed: " \
                 "Esc, Enter, arrows, W A S D C, Space, Shift, Ctrl, Alt, Win, Tab, Backspace, F4, Print Screen, " \
                 "mouse buttons. Saved in dbgmenu_keys.ini next to dinput8.dll."

// Command registry (like the Xbox console registry): one line = one menu item.
// Task 08: one category = one feature, all its settings inside it. Categories are shown in order of their first
// appearance here, items in their order inside the category.
//   new item    -> one line with the category of its feature (keep it next to the other lines of that category);
//   new section -> lines with a new category name, placed where the section should appear (max 16 sections);
//   new folder  -> a CT_FOLDER line "Name" in its section + lines with the category "Section/Name" (task 12; folders
//                  can be nested: "Section/Name/Sub"; the rows of a folder are its lines in their order here).
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
    {"Triggers", "show disabled", "Also draw the switched-off triggers (gray, paler); also their labels and the "
     "target.",
     CT_TOGGLE, On_ShowDisabled, &g_trigShowDisabled, NULL, 0, 0, 0, KA_NONE},
    // task 13: the info about the triggers (like Monsters); works also without Show triggers
    {"Triggers", "info: nearby", "Labels on the triggers near the player (near the camera in a free flight): what "
     "they show - Display. How far: nearby range; nearest first, a label that would cover another one moves up or is "
     "left out.",
     CT_TOGGLE, On_TrigNearby, &g_trigNearby, NULL, 0, 0, 0, KA_NONE},
    {"Triggers", "info: target", "A panel on the left about the trigger in the centre of the screen: the nearest one "
     "the view enters; with the camera inside triggers - the smallest of them; else the nearest to the centre. It is "
     "drawn yellow (also without Show triggers). What it shows: Display. {trigtarget} switches it.",
     CT_TOGGLE, On_TrigTarget, &g_trigTarget, NULL, 0, 0, 0, KA_TRIGTARGET},
    {"Triggers", "nearby range", "How far from the player the labels go (to the nearest point of the volume).",
     CT_ENUM, NULL, NULL, NULL, 0, 0, 0, KA_NONE, &g_trigRange, kMonRangeNames, 8},
    {"Triggers", "Display", "What the labels (info: nearby) and the panel (info: target) show: one switch per line. "
     "Enter - open, Esc - back.",
     CT_FOLDER, NULL, NULL, NULL, 0, 0, 0, KA_NONE},
    // ---- Triggers > Display (task 13) ----
    {"Triggers/Display", "name", "The name of the trigger in the level; [off] when switched off, [inside] while the "
     "player is in it.",
     CT_TOGGLE, NULL, &g_trigShow[TF_NAME], NULL, 0, 0, 0, KA_NONE},
    {"Triggers/Display", "class", "Its class (TriggerRegion, LoadRegion, KillRegion, a script class...); the panel "
     "shows the whole chain of parents.",
     CT_TOGGLE, NULL, &g_trigShow[TF_CLASS], NULL, 0, 0, 0, KA_NONE},
    {"Triggers/Display", "leads to", "Where it sends you or what it loads: waypoint / spawn point / world / region "
     "fields, the regions it loads and unloads.",
     CT_TOGGLE, NULL, &g_trigShow[TF_DEST], NULL, 0, 0, 0, KA_NONE},
    {"Triggers/Display", "events / scripts", "What it calls: onEnter / onExit -> Object.Event, the script handlers "
     "listening to it, a script class (label: just the counts).",
     CT_TOGGLE, NULL, &g_trigShow[TF_EVENTS], NULL, 0, 0, 0, KA_NONE},
    {"Triggers/Display", "who triggers it", "The filter: classes (Player, Horse, Monster...) and object names; "
     "exclude = everyone but them.",
     CT_TOGGLE, NULL, &g_trigShow[TF_FILTER], NULL, 0, 0, 0, KA_NONE},
    {"Triggers/Display", "flags", "OneShot / every time, RequireInWorld and the other switches that are on, delay.",
     CT_TOGGLE, NULL, &g_trigShow[TF_FLAGS], NULL, 0, 0, 0, KA_NONE},
    {"Triggers/Display", "state", "Enabled, the player inside now, how many times he went in / out (since the start "
     "of the game), when, and whether a OneShot already fired.",
     CT_TOGGLE, NULL, &g_trigShow[TF_STATE], NULL, 0, 0, 0, KA_NONE},
    {"Triggers/Display", "shape and size", "Box / rotated box / sphere / capsule and its size.",
     CT_TOGGLE, NULL, &g_trigShow[TF_SHAPE], NULL, 0, 0, 0, KA_NONE},
    {"Triggers/Display", "position", "The centre in the world (x y z).",
     CT_TOGGLE, NULL, &g_trigShow[TF_POS], NULL, 0, 0, 0, KA_NONE},
    {"Triggers/Display", "distance (target only)", "Distance from the player and from the camera, how it was "
     "picked. Only in the info: target panel.",
     CT_TOGGLE, NULL, &g_trigShow[TF_DIST], NULL, 0, 0, 0, KA_NONE},
    {"Triggers/Display", "all properties (target only)", "Every field the game knows about it (its reflection), "
     "by class. Only in the panel; all of them go to dbgmenu.log when the target changes.",
     CT_TOGGLE, NULL, &g_trigShow[TF_ALL], NULL, 0, 0, 0, KA_NONE},
    {"Triggers/Display", "all on", "Switch every line on.", CT_ACTION, Cmd_TrigAllOn, NULL, NULL, 0, 0, 0, KA_NONE},
    {"Triggers/Display", "all off", "Switch every line off (no labels then; the panel only says so).",
     CT_ACTION, Cmd_TrigAllOff, NULL, NULL, 0, 0, 0, KA_NONE},
    // ---- Monsters (task 11: Xbox AI showCharacterInfo / showCharacterDamageInfo / toggleMonsterUpdate;
    //      task 12: the Xbox info levels are replaced by the folder Display - one switch per line) ----
    {"Monsters", "info: nearby", "Labels over the monsters around (like the Xbox showCharacterInfo); what they show: "
     "Display. How far: nearby range; nearest first, a label that would cover another one moves up or is left out.",
     CT_TOGGLE, On_MonNearby, &g_monNearby, NULL, 0, 0, 0, KA_NONE},
    {"Monsters", "info: target", "A panel on the right about the monster in the centre of the screen (the one the "
     "camera looks at, any distance; yellow corners mark it); what it shows: Display. {target} switches it.",
     CT_TOGGLE, On_MonTarget, &g_monTarget, NULL, 0, 0, 0, KA_TARGET},
    {"Monsters", "nearby range", "How far from the camera the labels go (1 m is about 33 game units).",
     CT_ENUM, NULL, NULL, NULL, 0, 0, 0, KA_NONE, &g_monRange, kMonRangeNames, 8},
    {"Monsters", "show", "monsters: only enemies; + NPC: also the other characters; all: also the player.",
     CT_ENUM, NULL, NULL, NULL, 0, 0, 0, KA_NONE, &g_monWho, kMonWhoNames, 3},
    {"Monsters", "Display", "What the labels (info: nearby) and the panel (info: target) show: one switch per line. "
     "Enter - open, Esc - back.",
     CT_FOLDER, NULL, NULL, NULL, 0, 0, 0, KA_NONE},
    {"Monsters", "damage info", "Like the Xbox showCharacterDamageInfo: the game's own damage numbers over the "
     "enemies you hit (and the hit direction on the screen); Display > hits adds the last hit and the total to the "
     "labels and the panel. Only while on: your saved game settings are not changed.",
     CT_TOGGLE, On_MonDamage, &g_monDamage, NULL, 0, 0, 0, KA_NONE},
    {"Monsters", "freeze monsters", "Like the Xbox toggleMonsterUpdate: the monsters stop - no moving, no attacks; "
     "the player and the rest of the world go on (you can hit them and walk around). Off: they go on from where "
     "they stopped. Also off by itself on a level load, the player's death and {debug} off.",
     CT_TOGGLE, On_MonFreeze, &g_monFreeze, NULL, 0, 0, 0, KA_NONE},
    // ---- Monsters > Display (task 12): the lines of the labels and the panel, always in this order ----
    {"Monsters/Display", "name", "The name of the monster (its object name in the level, else its script class); "
     "[frozen] after it while freeze monsters is on.",
     CT_TOGGLE, NULL, &g_monShow[MF_NAME], NULL, 0, 0, 0, KA_NONE},
    {"Monsters/Display", "class (target only)", "The script class, the object name and the kind (monster / NPC / "
     "player). Too long for a label: only in the info: target panel.",
     CT_TOGGLE, NULL, &g_monShow[MF_CLASS], NULL, 0, 0, 0, KA_NONE},
    {"Monsters/Display", "HP", "Health now / max (the panel adds %; \"base max\" while the game has not worked out "
     "the max for the difficulty yet).",
     CT_TOGGLE, NULL, &g_monShow[MF_HP], NULL, 0, 0, 0, KA_NONE},
    {"Monsters/Display", "level", "The level of the monster (lvl N; next to HP when both are on).",
     CT_TOGGLE, NULL, &g_monShow[MF_LEVEL], NULL, 0, 0, 0, KA_NONE},
    {"Monsters/Display", "damage / defense / resistance", "Its attack damage, defense and resistance (one line).",
     CT_TOGGLE, NULL, &g_monShow[MF_STATS], NULL, 0, 0, 0, KA_NONE},
    {"Monsters/Display", "move state", "The movement state (Xbox level 1), e.g. MonsterIdle.",
     CT_TOGGLE, NULL, &g_monShow[MF_MOVE], NULL, 0, 0, 0, KA_NONE},
    {"Monsters/Display", "AI behaviour", "The AI behaviour and its current part (Xbox level 2, the script state), "
     "e.g. base/tbehavior: Strafe. Monsters only.",
     CT_TOGGLE, NULL, &g_monShow[MF_AI], NULL, 0, 0, 0, KA_NONE},
    {"Monsters/Display", "cooldowns", "The attack cooldowns in seconds (Xbox level 3). Monsters only.",
     CT_TOGGLE, NULL, &g_monShow[MF_COOLDOWNS], NULL, 0, 0, 0, KA_NONE},
    {"Monsters/Display", "target", "Who the monster is after and how far it is (Xbox level 4).",
     CT_TOGGLE, NULL, &g_monShow[MF_TARGET], NULL, 0, 0, 0, KA_NONE},
    {"Monsters/Display", "distance (target only)", "Distance from the camera, the angle to the screen centre and "
     "the height of its body. Only in the info: target panel.",
     CT_TOGGLE, NULL, &g_monShow[MF_DIST], NULL, 0, 0, 0, KA_NONE},
    {"Monsters/Display", "position", "The position in the world (x y z).",
     CT_TOGGLE, NULL, &g_monShow[MF_POS], NULL, 0, 0, 0, KA_NONE},
    {"Monsters/Display", "hits", "The last hit and the total (our own count by the HP going down). Only while "
     "damage info is on (so it is on by default).",
     CT_TOGGLE, NULL, &g_monShow[MF_HITS], NULL, 0, 0, 0, KA_NONE},
    {"Monsters/Display", "all on", "Switch every line on.", CT_ACTION, Cmd_MonAllOn, NULL, NULL, 0, 0, 0, KA_NONE},
    {"Monsters/Display", "all off", "Switch every line off (no labels then; the panel only says so).",
     CT_ACTION, Cmd_MonAllOff, NULL, NULL, 0, 0, 0, KA_NONE},
    // ---- Cheats (task 15, cheats.cpp): not saved; {debug} off switches them off ----
    {"Cheats", "invincible", "Like the Xbox playerInvincible: the player takes no damage - his HP does not go down, "
     "also not from poison or fire. Hits still push and stagger him. A fall into a pit (a kill zone) does not kill: "
     "the game puts him back on safe ground. Stays on after deaths and level loads until you switch it off.",
     CT_TOGGLE, On_CheatInvincible, &g_cheatInvincible, NULL, 0, 0, 0, KA_NONE},
    {"Cheats", "one-hit kill", "Any hit of the player (weapons, abilities, the pistol) or of the creatures he "
     "summons kills an enemy at once - a normal death with its animation, loot and XP. Friends, NPC and the horse "
     "are not touched. A boss in its fight takes normal damage (an instant death could break the fight).",
     CT_TOGGLE, On_CheatOneHit, &g_cheatOneHit, NULL, 0, 0, 0, KA_NONE},
    {"Cheats", "kill all monsters", "Like the Xbox killMonsters (\"Kill all monsters and spawns\"): every enemy in "
     "the loaded level dies a normal death - loot, XP, and the doors that open after a fight open. Friends are not "
     "touched. Bosses get the same blow, as on the Xbox; a boss its fight protects stays alive.",
     CT_ACTION, Cmd_KillAll, NULL, NULL, 0, 0, 0, KA_NONE},
    // ---- Camera ----
    {"Camera", "showCameraPosition", "Display the current position of the player and the camera, the map (world) "
     "and its regions: the active one and the loaded ones.",
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
    {"Keys", "info: target", "Shows and hides the Monsters info: target panel. " KEY_DESC,
     CT_KEY, NULL, NULL, NULL, 0, 0, 0, KA_TARGET},
    {"Keys", "triggers info: target", "Shows and hides the Triggers info: target panel. " KEY_DESC,
     CT_KEY, NULL, NULL, NULL, 0, 0, 0, KA_TRIGTARGET},
    {"Keys", "reset to defaults", "All keys back to F10, F1, F2, F3, R, T, F5, F6 (the file is rewritten).",
     CT_ACTION, Cmd_KeysDefault, NULL, NULL, 0, 0, 0, KA_NONE},
};
static const int kNumCommands = sizeof(g_commands) / sizeof(g_commands[0]);

static const char *s_cats[16];  // the sections (top-level categories, no '/')
static int s_numCats;
// Cursor: never reset when the menu closes, so F1 opens it where it was closed (for the whole game run).
static int s_level;         // 0 = sections (Main), 1 = the rows of s_path
static int s_cat;           // the section selected in Main
static char s_path[96];     // task 12: the open section "Monsters" or a folder inside it "Monsters/Display"
struct SelMem { char path[96]; int sel; };
static SelMem s_selMem[32]; // the remembered row of every section / folder that was opened
static int s_numSelMem;
static char s_status[256];
static DWORD s_statusUntil;

static void InitCategories()
{
    if (s_numCats) return;
    for (int i = 0; i < kNumCommands; i++) {
        if (strchr(g_commands[i].category, '/')) continue;  // a folder: its row is a CT_FOLDER line of the section
        int c = 0;
        while (c < s_numCats && strcmp(s_cats[c], g_commands[i].category) != 0) c++;
        if (c == s_numCats && s_numCats < 16) s_cats[s_numCats++] = g_commands[i].category;
    }
}

static int ItemsIn(const char *path, const Command **out, int max)
{
    int n = 0;
    for (int i = 0; i < kNumCommands && n < max; i++)
        if (strcmp(g_commands[i].category, path) == 0) out[n++] = &g_commands[i];
    return n;
}

// the path of the folder a CT_FOLDER row opens: "Monsters" + "Display" -> "Monsters/Display"
static void FolderPath(const Command *c, char *out, int size) { sprintf_s(out, size, "%s/%s", c->category, c->name); }

// the remembered row of a section / folder (0 the first time)
static int &SelOf(const char *path)
{
    for (int i = 0; i < s_numSelMem; i++)
        if (strcmp(s_selMem[i].path, path) == 0) return s_selMem[i].sel;
    if (s_numSelMem == 32) return s_selMem[31].sel;  // never with today's registry: the last slot is shared then
    SelMem &m = s_selMem[s_numSelMem++];
    strcpy_s(m.path, path);
    m.sel = 0;
    return m.sel;
}

// "Monsters/Display" -> "Monsters > Display" (the header, the status line, the log)
static const char *PathText(const char *path)
{
    static char buf[2][160];
    static int k;
    char *o = buf[k ^= 1];
    int n = 0;
    for (; *path && n < 150; path++) {
        if (*path == '/') o[n++] = ' ', o[n++] = '>', o[n++] = ' ';
        else o[n++] = *path;
    }
    o[n] = 0;
    return o;
}

// "Triggers" (section list) or "Triggers > depth" / "Monsters > Display > HP" (rows) - for the log
static const char *Where()
{
    static char buf[200];
    if (!s_numCats) return "-";
    if (s_level == 0) return s_cats[s_cat];
    const Command *items[64];
    int n = ItemsIn(s_path, items, 64);
    int sel = SelOf(s_path) < n ? SelOf(s_path) : n - 1;
    sprintf_s(buf, "%s > %s", PathText(s_path), sel >= 0 ? items[sel]->name : "-");
    return buf;
}

// Enter on a section / on a CT_FOLDER row
static void OpenPath(const char *path)
{
    strcpy_s(s_path, path);
    s_level = 1;
    Log("menu: open %s", Where());
}

// Esc / Backspace (Left on an action or a folder row): one level up - a folder -> its parent, a section -> Main
static void GoBack()
{
    char *slash = strrchr(s_path, '/');
    if (slash) *slash = 0;
    else s_level = 0;
    Log("menu: back to %s", Where());
}

// task 10: "{fly}" -> the current key of that action ("F2")
static void ExpandKeys(const char *src, char *dst, int size)
{
    static const char *const kTokens[KA_COUNT] = {"", "{debug}", "{menu}", "{fly}", "{triggers}", "{attach}", "{time}",
                                                  "{target}", "{trigtarget}"};
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
    Menu_Status("keys: defaults (%s debug, %s menu, %s playerFly, %s triggers, %s attach, %s time, %s target, "
                "%s trigger target)",
                Keys_KeyName(KA_DEBUG), Keys_KeyName(KA_MENU), Keys_KeyName(KA_FLY), Keys_KeyName(KA_TRIGGERS),
                Keys_KeyName(KA_ATTACH), Keys_KeyName(KA_TIME), Keys_KeyName(KA_TARGET), Keys_KeyName(KA_TRIGTARGET));
}
// task 11: Monsters (the flags change in the next game frame, monsters.cpp)
static void On_MonNearby() { Menu_Status("info: nearby %s", g_monNearby ? "on" : "off"); }
static void On_MonTarget() { Menu_Status("info: target %s", g_monTarget ? "on" : "off"); }
static void On_MonDamage() { Menu_Status("damage info %s", g_monDamage ? "on" : "off"); }
static void On_MonFreeze() { Menu_Status("freeze monsters %s", g_monFreeze ? "on" : "off"); }
// task 13: Triggers info (read in the next game frame, triggers.cpp)
static void On_TrigNearby() { Menu_Status("Triggers > info: nearby %s", g_trigNearby ? "on" : "off"); }
static void On_TrigTarget() { Menu_Status("Triggers > info: target %s", g_trigTarget ? "on" : "off"); }
static void Cmd_TrigAllOn() { Trig_ShowAll(true); Menu_Status("Triggers > Display: all on"); }
static void Cmd_TrigAllOff() { Trig_ShowAll(false); Menu_Status("Triggers > Display: all off"); }
// task 12: Monsters > Display
static void Cmd_MonAllOn() { Mon_ShowAll(true); Menu_Status("Monsters > Display: all on"); }
static void Cmd_MonAllOff() { Mon_ShowAll(false); Menu_Status("Monsters > Display: all off"); }
// task 15: Cheats (cheats.cpp; kill all runs before the next game frame and writes the status itself)
static void On_CheatInvincible() { Cheats_MenuChanged(true); }
static void On_CheatOneHit() { Cheats_MenuChanged(false); }
static void Cmd_KillAll()
{
    Cheats_RequestKillAll();
    Menu_Status("kill all monsters...");
}

// task 12: a toggle without its own callback - "Monsters > Display > HP: on"
static void ToggleDone(const Command *c)
{
    if (c->action) c->action();
    else Menu_Status("%s > %s: %s", PathText(c->category), c->name, *c->flag ? "on" : "off");
}

static void Execute(const Command *c)
{
    switch (c->type) {
    case CT_ACTION:
        if (c->action) c->action();
        break;
    case CT_TOGGLE:
        *c->flag = !*c->flag;
        ToggleDone(c);
        break;
    case CT_FLOAT:
    case CT_FOLDER:  // opened by Menu_HandleKey
        break;
    case CT_ENUM:
        *c->choice = (*c->choice + 1) % c->numNames;
        Menu_Status("%s > %s: %s", PathText(c->category), c->name, c->names[*c->choice]);
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
        Menu_Status("%s > %s: %s", PathText(c->category), c->name, c->names[*c->choice]);
        if (c->action) c->action();
    } else if (c->type == CT_TOGGLE) {
        bool v = dir > 0;
        if (*c->flag != v) {
            *c->flag = v;
            ToggleDone(c);
        }
    } else if (c->type == CT_FLOAT) {
        float v = *c->value + dir * c->step;
        *c->value = v < c->vmin ? c->vmin : (v > c->vmax ? c->vmax : v);
        Log("menu: %s > %s = %.2f", PathText(c->category), c->name, *c->value);
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
        else if (a == KA_TARGET) Log("menu: %s -> info: target %s", Keys_KeyName(a), g_monTarget ? "on" : "off");
        else if (a == KA_TRIGTARGET)
            Log("menu: %s -> Triggers > info: target %s", Keys_KeyName(a), g_trigTarget ? "on" : "off");
        return;
    }
    if (!g_menuOpen || g_keyCapture) return;

    if (s_level == 0) {
        switch (vk) {
        case VK_UP: s_cat = (s_cat + s_numCats - 1) % s_numCats; break;
        case VK_DOWN: s_cat = (s_cat + 1) % s_numCats; break;
        case VK_RETURN:
        case VK_RIGHT:
            OpenPath(s_cats[s_cat]);
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
    int n = ItemsIn(s_path, items, 64);
    int &sel = SelOf(s_path);
    if (n == 0) {
        s_level = 0;
        return;
    }
    if (sel >= n) sel = n - 1;
    const Command *c = items[sel];
    char sub[96];
    switch (vk) {
    case VK_UP: sel = (sel + n - 1) % n; break;
    case VK_DOWN: sel = (sel + 1) % n; break;
    case VK_RETURN:
    case VK_RIGHT:
        if (c->type == CT_FOLDER) {  // task 12
            FolderPath(c, sub, sizeof(sub));
            OpenPath(sub);
        } else if (vk == VK_RETURN) {
            Log("menu: execute %s/%s", c->category, c->name);
            Execute(c);
        } else if (c->type != CT_ACTION && c->type != CT_KEY) {
            Change(c, +1);
        }
        break;
    case VK_LEFT:
        if (c->type == CT_ACTION || c->type == CT_KEY || c->type == CT_FOLDER) GoBack();
        else Change(c, -1);
        break;
    case VK_ESCAPE:
    case VK_BACK:
        GoBack();
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
    ImGui::TextDisabled(s_level == 0 ? "Main" : "Main > %s", PathText(s_path));
    ImGui::Separator();

    const char *desc = NULL;
    if (s_level == 0) {
        const Command *items[64];
        for (int c = 0; c < s_numCats; c++) {
            char cnt[16];
            sprintf_s(cnt, "%d >", ItemsIn(s_cats[c], items, 64));
            Row(c == s_cat, s_cats[c], cnt, kOff);
        }
    } else {
        const Command *items[64], *sub[64];
        int n = ItemsIn(s_path, items, 64);
        int sel = SelOf(s_path) < n ? SelOf(s_path) : n - 1;
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
            } else if (c->type == CT_FOLDER) {  // task 12: like a section row
                char path[96];
                FolderPath(c, path, sizeof(path));
                sprintf_s(value, "%d >", ItemsIn(path, sub, 64));
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
    else ImGui::TextDisabled("Up/Down select  Enter run/open  Left/Right change  Esc back");
    ImGui::PopTextWrapPos();
    s_menuBottom = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y;
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

float Menu_PanelBottom() { return g_menuOpen ? s_menuBottom : 0.0f; }

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
    World_Panel();  // task 13: the map, the active / loaded regions
    if (gi.exCode) ImGui::TextColored(kStatus, "exception %08X at %08X", gi.exCode, gi.exAddr);
    ImGui::End();
}

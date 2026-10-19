// Darksiders II PC 2012 debug menu (dinput8.dll proxy) — shared declarations.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d9.h>

#define DBGMENU_VERSION "dbgmenu v0.10 (task 11)"

// ---- dllmain.cpp: log, identity, image ranges ----
void Log(const char *fmt, ...);
bool LogBudget(volatile LONG *counter, LONG max);  // true while fewer than max messages were logged
extern HMODULE g_self;
extern bool g_gameMatch;  // Darksiders2.exe identity matched -> game addresses are valid
extern bool g_testMode;   // DBGMENU_TEST=1: offline test harness, hooks without the game
bool InImage(const void *p);  // inside the exe image (vtables live in .rdata)
bool InText(const void *p);   // inside the exe .text (functions)

// ---- hooks.cpp: MinHook + IAT + D3D9 + game functions ----
bool Hooks_Install();
void **FindImportSlot(HMODULE mod, const char *dll, const char *func);
bool PatchSlot(void **slot, void *fn, void **orig);
extern bool g_frameHooked;  // game frame 0x00B3D3E4 hooked -> flight runs there, otherwise from Present

// ---- input.cpp: WndProc subclass, Raw Input filter, key queue ----
extern volatile LONG g_menuOpen;     // menu visible (toggled by F1 in WndProc)
extern volatile LONG g_inputBlocked; // game must not get keyboard/mouse (menu open or flying)
extern volatile LONG g_overlayReady; // Present hook works -> F1 may open the menu
void Input_Init();
void Input_Attach(HWND hwnd);
void Input_PerFrame();
int Input_PopKey();  // 0 = queue empty
void Input_SetMenuOpen(bool open);
void Input_UpdateBlock();               // g_inputBlocked = menu open || flying
bool Input_KeyHeld(int vk);             // from our own view of key events (works while the game is blocked)
void Input_TakeMouse(long *dx, long *dy);  // relative mouse motion accumulated while flying
void Input_ResetFlight();               // drop accumulated mouse motion
HWND Input_Hwnd();                      // the game window we subclassed (NULL before CreateDevice)
bool Input_WindowActive();              // the game window has the focus (activation messages + foreground)
DWORD Input_ActiveForMs();              // time since the window got the focus (back)
void Input_HoverArm();                  // keys/buttons held now never release the hover (e.g. the teleport press)
int Input_HoverKey();                   // game control key/button pressed since Input_HoverArm (0 = none)
extern UINT(WINAPI *o_GetRawInputData)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
UINT WINAPI Hook_GetRawInputData(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT hdr);

// ---- cursor.cpp: system cursor hidden and locked in the window while the menu is open or flying ----
void Cursor_InstallHooks(HMODULE exe);  // IAT ShowCursor / ClipCursor / SetCursorPos of the exe
void Cursor_Update();                   // game thread: every frame and on activation messages
void Cursor_OnUnload();
void Cursor_TestLog(const char *when);  // test mode: log the simulated cursor state

// ---- overlay.cpp: ImGui on top of the game frame ----
void Overlay_SetDevice(IDirect3DDevice9 *dev, HWND hwnd);
void Overlay_OnFrame(IDirect3DDevice9 *dev);
void Overlay_BeforeReset(IDirect3DDevice9 *dev);
IDirect3DDevice9 *Overlay_BoundDevice();      // device the ImGui DX9 backend holds a reference to
void Overlay_FreeResources(IDirect3DDevice9 *dev); // ImGui D3DPOOL_DEFAULT objects (recreated lazily)
void Overlay_DeviceDying(IDirect3DDevice9 *dev); // game drops its last reference -> free ours first
bool Overlay_Internal();  // this thread is inside an ImGui backend call that releases device references
IDirect3DDevice9 *Overlay_MainDevice();          // last device created by the game (NULL after it died)

// ---- menu.cpp ----
extern bool g_showCameraPosition;
void Menu_HandleKey(int vk);
void Menu_Draw();
void Info_Draw();
bool Menu_WantsDraw();
void Menu_Status(const char *fmt, ...);
void Menu_OnDebugSwitch(bool on);  // task 10: F10 on / off (status cleared, key capture cancelled)

// ---- keys.cpp: task 10, the master switch (F10) and the hotkeys the player can change (dbgmenu_keys.ini) ----
enum KeyAction { KA_NONE = 0, KA_DEBUG, KA_MENU, KA_FLY, KA_TRIGGERS, KA_ATTACH, KA_TIME, KA_TARGET,  // 11: info: target
                 KA_COUNT };
enum { KQ_CAPTURE = 0x1000, KQ_ACTION = 0x2000 };  // key queue: a key for the menu Keys / a hotkey that was run
extern volatile LONG g_debugOn;     // the debug menu is on (off after the game starts)
extern volatile LONG g_keyCapture;  // menu Keys waits for a new key (the action), 0 = no
void Keys_Load();                   // read the file (create it with the defaults when missing)
int Keys_Get(int action);           // virtual key
const char *Keys_KeyName(int action);  // "F1"
const char *Keys_Label(int action);    // "menu", "attach (in flight)"
const char *Keys_Name(int vk);      // any key, also the forbidden ones ("Space")
bool Keys_Allowed(int vk);
int Keys_HotkeyNow(int vk);         // KA_DEBUG always; KA_MENU / KA_FLY / KA_TRIGGERS / KA_TARGET while it is on
bool Keys_Assign(int action, int vk, int *swapped);  // false = not allowed; writes the file
void Keys_ResetDefaults();          // writes the file
const char *Keys_FilePath();
void Debug_Toggle(const char *why); // F10: on / everything off

// ---- game.cpp: guarded reads of game objects ----
struct GameVec { float x, y, z, w; };
enum { ST_NONE = 0, ST_OK, ST_NULL, ST_EXCEPTION, ST_BADPTR };
const char *StName(int st);
struct GameInfo {
    int gameSt;   void *game;
    int actorSt;  void *actor;  void *actorVt;  void *actorGetPos;
    void *actorSetPos; void *actorSetYaw;                         // vt[0x34] / vt[0x138] (task 04 teleport)
    bool actorPosOk; GameVec actorPos; GameVec actorRaw;     // vt[0x38] getPosition / raw +0x90
    int camSt;    void *cam;    void *camVt;    void *camGetPos;
    bool camPosOk; GameVec camPos; GameVec camDir; GameVec camRaw;  // vt[0x44] / vt[0x40] / raw +0xB0
    DWORD exCode; DWORD exAddr;  // last exception, if any
};
void Game_Read(GameInfo *gi);

// flight API (every call guarded; false = not available / exception, already logged with a budget)
struct GameMat { float m[16]; };  // 4 rows: 0 right, 1 up, 2 back (dir = -row2), 3 position
bool Game_GetGame(void **game);                    // raw read of the singleton pointer
void *Game_CameraRaw(void *game);                  // StateCamera game+0x138 (no reference taken)
bool Game_GetActor(void *game, void **actor);      // getPlayerActor (reference released at once)
bool Game_ActorGetPos(void *actor, GameVec *out);  // vt[0x38]
bool Game_ActorTeleport(void *actor, const GameVec *pos, const GameVec *dir, float *yawOut);  // vt[0x34] + vt[0x138]
bool Game_ActorSetPos(void *actor, const GameVec *pos);  // vt[0x34] only (hover hold)
bool Game_ActorGetVz(void *actor, float *vz);      // player +0x3B8 vertical speed (only the known player class)
bool Game_ActorSetVz(void *actor, float vz);       // +0x3B8 and its previous value +0x3BC
// task 06: the player's state machine (notes/playerfly_pc.md §6)
struct GameMove {
    bool ok;                          // the known player class, current state readable
    void *state, *stateVt, *desc;     // actor+0x310, its vtable, state+8
    bool fall;                        // PlayerFall (vt 0x011E92A0): the fields below are valid
    int onJump, onFall, chanceJump;   // desc+0x60 OnJump, +0x58 OnFall, PlayerFall desc+0x130 OnSecondChanceJump
                                      // (-1 = none)
    float chanceDur, fallDistLimit;   // PlayerFall desc +0x12C SecondChanceJumpDuration, +0x134 FallDistance
    float fallDist, chance, stuck, fallHspeed;  // PlayerFall +0xA0 fallen height, +0xAC second chance left (s),
                                                // +0xB0 "stuck" timer, +0xA8 horizontal speed for that jump
    BYTE descFlags;                   // desc+0xA8 (bit 0 = InAir)
    bool ctrl, onGround;              // controller +0x30C: on ground = +0x109 && +0x45 && !+0x46 (0x005029AD)
    DWORD flags308;                   // actor+0x308 (bit 0x8000 = PlayerFall "stuck")
    float hspeed, vz;                 // actor +0x274, +0x3B8
    BYTE jumpIn;                      // actor+0x961 = input +0x81: jump pressed
};
bool Game_PlayerMove(void *actor, GameMove *m);
void Game_MoveStr(const GameMove &m, char *buf, int size);
// PlayerFall that is the current state: +0xAC = chance (if >= 0), +0xA0 = +0xB0 = 0 (if resetDist),
// +0xA8 = hspeed (if >= 0)
bool Game_FallArm(void *actor, void *state, float chance, bool resetDist, float hspeed);
bool Game_StateChange(void *actor, void *state, int id);  // state->vt[0x54] 0x006CDB14 changeState(id), state current
bool Game_ActorSetHspeed(void *actor, float v);            // +0x274
bool Game_CamGetMatrix(void *cam, GameMat *out);   // cam+0x80
bool Game_CamResetBehindPlayer(void *cam);         // StateCamera reset 0x005B7348 + default state 0x005D5B1A
bool Game_PauseState(void *game, int *count, DWORD *flags);  // game+0xF8 / game+0x10
bool Game_SetPaused(void *game, bool on);          // game->vt[0x68] 0x00B3C287
void Game_LogActorMethods(void *actor);
void Game_TestTick();                               // test mode: fake game objects (+ simple gravity)
void Game_TestRender();                             // test mode: fake renderer asks game->vt[0x7C]
void Game_TestControl(int cmd);

// ---- triggers.cpp: Show triggers (task 07, notes/triggers_pc.md) ----
enum { TD_WALLS = 0, TD_DEPTH = 1 };
enum { TE_SETRT = 1, TE_SETDS, TE_CLEAR, TE_VIEW, TE_HUD, TE_PRESENT };
extern bool g_showTriggers;        // menu Triggers -> Show triggers (F3)
extern int g_trigDepthMode;        // menu Triggers -> depth: through walls / depth
extern bool g_trigShowDisabled;    // menu Triggers -> show disabled (default on)
void Trig_Frame();                 // game thread, once per game frame (before the original frame): collect
void Trig_Toggle();                // F3 / menu
void Trig_OnRenderView(const float *camMat, const float *proj, void *caller);  // hook 0x00983AFC
void Trig_OnHud();                 // hook of the HUD pass 0x00B3B8D2 (entry): depth mode draws here
void Trig_OnPresent(IDirect3DDevice9 *dev);  // before the overlay: depth mode fallback
void Trig_Draw2D();                // overlay frame (ImGui): through walls
void Trig_DrawStatus();            // overlay frame (ImGui): "triggers: N drawn / M total"
void Trig_StartCapture();          // log one frame of render target / depth / Clear calls
void Trig_D3DEvent(int type, IDirect3DDevice9 *dev, DWORD a, DWORD b, void *p);  // device hooks -> timeline
void Trig_TestControl(int cmd);    // test mode: 8 projection check, 9 empty list, 10 broken list, 11 normal,
                                   // 13 exception in the next collection, 14 8000 objects (timing)

// task 11: for monsters.cpp - the view of this frame (false = no 3D view this frame) and the EventManagers of the
// loaded world (real game only)
bool Trig_View(float *vp, float *cam, float *proj);
int Trig_EventManagers(void **out, int max);

// task 09: hooks of the detector body overlap callbacks (before the original): log "trigger fired" for the player
void Trig_OnDetectorAdd(void *body, void *collidable);  // 0x008B82DD: a collidable starts overlapping
void Trig_OnDetectorRemove(void *body, void *obj);      // 0x008B815A: an object stops overlapping

// ---- monsters.cpp: section Monsters (task 11, notes/monsters_pc.md) ----
enum { MW_MONSTERS = 0, MW_NPC = 1, MW_ALL = 2 };
extern bool g_monNearby;           // menu Monsters -> info: nearby (labels over the monsters around)
extern bool g_monTarget;           // menu Monsters -> info: target (panel of the monster in the centre; key F5)
extern bool g_monDamage;           // menu Monsters -> damage info (the game's DamageIndicators while on)
extern bool g_monFreeze;           // menu Monsters -> freeze monsters (toggleMonsterUpdate, byte 0x014D4278)
extern int g_monLevel;             // menu Monsters -> info level: 0..3 = Xbox showCharacterInfo levels 1..4
extern int g_monRange;             // menu Monsters -> nearby range (index: 10 20 30 50 75 100 150 m, unlimited)
extern int g_monWho;               // menu Monsters -> show: MW_*
void Mon_Frame();                  // game thread, once per game frame (before the frame): freeze / damage flags
void Mon_Draw2D();                 // overlay frame (ImGui, game thread): collect, labels, target panel, summary
void Mon_ToggleTarget();           // the info: target key
void Mon_OnDebugOff();             // F10 off: everything off, flags back
void Mon_OnUnload();
extern bool g_monDamageNoHooks;    // the options hooks are not installed -> damage info is not offered
int Mon_OptionsHide(void *opt);    // hooks of the options commit / changed: the game sees the player's value
void Mon_OptionsRestore(void *opt, int prev);  // (any options object, also a copy; -1 = nothing was hidden)
void Mon_TestControl(int cmd);     // test mode: 17..26

// ---- flight.cpp: playerFly ----
extern bool g_hoverAfterTeleport;  // menu playerFly -> hover after teleport (default on)
enum { HJ_GAME = 0, HJ_DIRECT = 1 };
extern int g_hoverJumpMode;        // menu playerFly -> hover jump: game (second chance kept armed) / direct (we jump)
extern bool g_flyFreezeTime;       // task 09: menu playerFly -> freeze time in flight (default off: the world runs)
extern bool g_flyAttach;           // task 09: menu playerFly -> attach player (default off)
extern float g_attachDist;         // task 09: menu playerFly -> attach distance (units in front of the camera)
extern volatile LONG g_flying;
void Flight_Frame();                       // once per game frame, game thread
void Flight_RequestToggle();               // F2 / menu
void Flight_RequestStop();                 // task 10: F10 off - like F2, but never starts a flight
void Flight_RequestTeleport();             // Enter / left mouse button while flying
void Flight_RequestTime(int mode);         // task 09: T (-1 = switch) / menu (0 = run, 1 = freeze) while flying
void Flight_RequestAttach(int mode);       // task 09: R (-1 = switch) / menu (0 = off, 1 = on) while flying
bool Flight_TimeFrozen();                  // flying with our pause
bool Flight_Attached();                    // the player is carried in front of the camera
bool Flight_AttachedPos(GameVec *out);     // v0.8.1: where the carried player is (status line)
void Flight_RequestAbort(const char *why); // real failures only (not focus loss, not Reset)
void Flight_OnUnload(bool processExit);
void Flight_TestThrow();                   // test mode: exception in the next flight frame
bool Flight_RenderMatrix(float *out, void *caller);  // hook: game->vt[0x7C] (free camera / blend after teleport)
bool Flight_GetCamPos(GameVec *out);
float Flight_SpeedMul();
bool Flight_Hovering();
bool Flight_NoFocus();                     // flying, but the window lost the focus (frozen)

// Darksiders II PC 2012 debug menu (dinput8.dll proxy) — shared declarations.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d9.h>

#define DBGMENU_VERSION "dbgmenu v0.3 (task 04)"

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
extern UINT(WINAPI *o_GetRawInputData)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
UINT WINAPI Hook_GetRawInputData(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT hdr);

// ---- overlay.cpp: ImGui on top of the game frame ----
void Overlay_SetDevice(IDirect3DDevice9 *dev, HWND hwnd);
void Overlay_OnFrame(IDirect3DDevice9 *dev);
void Overlay_BeforeReset(IDirect3DDevice9 *dev);
IDirect3DDevice9 *Overlay_BoundDevice();      // device the ImGui DX9 backend holds a reference to
void Overlay_FreeResources(IDirect3DDevice9 *dev); // ImGui D3DPOOL_DEFAULT objects (recreated lazily)
void Overlay_DeviceDying(IDirect3DDevice9 *dev); // game drops its last reference -> free ours first
bool Overlay_Internal();  // this thread is inside an ImGui backend call that releases device references

// ---- menu.cpp ----
extern bool g_showCameraPosition;
void Menu_HandleKey(int vk);
void Menu_Draw();
void Info_Draw();
bool Menu_WantsDraw();
void Menu_Status(const char *fmt, ...);

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
bool Game_CamGetMatrix(void *cam, GameMat *out);   // cam+0x80
bool Game_CamSetMatrix(void *cam, const GameMat *m);  // cam+0x80 and cam+0xC0 (what the renderer reads)
bool Game_PauseState(void *game, int *count, DWORD *flags);  // game+0xF8 / game+0x10
bool Game_SetPaused(void *game, bool on);          // game->vt[0x68] 0x00B3C287
void Game_LogActorMethods(void *actor);
void Game_TestTick();                               // test mode: fake game objects
void Game_TestControl(int cmd);

// ---- flight.cpp: playerFly ----
enum { FLYCAM_A = 0, FLYCAM_B = 1 };
extern int g_flyCamMode;    // A: write StateCamera matrices; B: replace game->vt[0x7C] result (render only)
extern int g_teleportMode;  // 0: setPos once; 1: setPos + re-apply on the first frames after unpause
extern volatile LONG g_flying;
void Flight_Frame();                       // once per game frame, game thread
void Flight_RequestToggle();               // F2 / menu
void Flight_RequestTeleport();             // Enter / left mouse button while flying
void Flight_RequestAbort(const char *why); // Reset, focus loss
void Flight_OnUnload(bool processExit);
void Flight_TestThrow();                   // test mode: exception in the next flight frame
void Flight_AfterCamUpdate(void *cam, int which);  // hook: after StateCamera tick (0) / update (1)
bool Flight_RenderMatrix(float *out, void *caller);  // hook: game->vt[0x7C]
bool Flight_GetCamPos(GameVec *out);
float Flight_SpeedMul();
const char *Flight_CamModeName(int mode);

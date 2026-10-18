// playerFly, after the Xbox PlayerFlyCommand 0x825B42C8 + FlyCameraState 0x825BF7F8 (notes/playerfly_xbox.md):
// the game is paused (game->vt[0x68]), we fly our own camera, teleport = actor setPos vt[0x34] + setYaw vt[0x138]
// (the same pair the game's scripted teleportTo 0x00796B71 uses).
// Everything runs in the game thread, once per frame, from the hook of the game frame 0x00B3D3E4 (before the
// original frame: requests, pause and teleport happen between frames, and the camera matrix is ready before
// 0x00B3CD27 hands it to the renderer). Without that hook (offline test) it runs from Present.
// Free camera, two ways (menu Camera -> fly camera):
//   A: our matrix goes into StateCamera +0x80 and +0xC0 before the frame and after every StateCamera tick
//      (vt[0x34] 0x005B8599) / update (vt[0x38] 0x005B5D33) - the whole game sees the free camera;
//   B: game->vt[0x7C] 0x004CD9DA (the matrix 0x00B3CD27 gives the renderer, a copy of StateCamera+0xC0)
//      returns our matrix - only the picture changes, the game camera keeps following the player.
// Pause balance: exactly one setPaused(false) for our one setPaused(true), and only if that call really raised
// the counter game+0xF8; any failure (exception, Reset, focus loss, lost player/camera) exits with unpause.
#include "common.h"
#include <math.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>

int g_flyCamMode = FLYCAM_B;  // B reads exactly the render input (statically confirmed), so it is the default
int g_teleportMode = 0;
volatile LONG g_flying;

static const float kBaseSpeed = 375.0f * 2.0f;  // Xbox FlyCameraState: 375 * dt * 2.0 units per second
static const float kMaxSinPitch = 0.95f;         // Xbox: |forward . up| <= 0.95
static const float kMouseSens = 0.0025f;         // radians per raw mouse count
static const float kMaxDt = 0.1f;

static volatile LONG s_reqToggle, s_reqTeleport, s_reqAbort, s_testThrow;
static const char *volatile s_abortWhy;

static void *s_game, *s_cam, *s_actor;
static bool s_pausedByUs, s_matValid;
static GameMat s_orig, s_mat;
static float s_pos[3], s_fwd[3], s_yaw, s_pitch, s_sR = 1, s_sU = 1, s_speedMul = 1;
static LARGE_INTEGER s_freq, s_last;
static DWORD s_enterTick;
static int s_frames, s_pause0, s_pause1;
// one flight's statistics (log at exit)
static LONG s_tickCalls, s_updCalls, s_aWrites, s_bCalls, s_mouseTotal;
static void *s_callers[8];
static int s_nCallers;

// position probes after a teleport: did the body stay where we put it?
static struct {
    bool active;
    void *game, *actor;
    GameVec target, dir;
    DWORD t0;
    int stage, repeat;
} s_probe;

const char *Flight_CamModeName(int mode) { return mode == FLYCAM_A ? "A (StateCamera)" : "B (render)"; }
float Flight_SpeedMul() { return s_speedMul; }

bool Flight_GetCamPos(GameVec *out)
{
    if (!g_flying) return false;
    out->x = s_pos[0]; out->y = s_pos[1]; out->z = s_pos[2]; out->w = 0;
    return true;
}

void Flight_RequestToggle() { InterlockedExchange(&s_reqToggle, 1); }
void Flight_RequestTeleport()
{
    if (g_flying) InterlockedExchange(&s_reqTeleport, 1);
}
void Flight_RequestAbort(const char *why)
{
    if (!g_flying) return;
    s_abortWhy = why;
    InterlockedExchange(&s_reqAbort, 1);
}
void Flight_TestThrow()
{
    if (g_testMode) InterlockedExchange(&s_testThrow, 1);
}

// ---- math ----
static float Dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void Cross3(const float *a, const float *b, float *o)
{
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}

static void DirFromAngles()
{
    float cp = cosf(s_pitch);
    s_fwd[0] = cp * cosf(s_yaw);
    s_fwd[1] = cp * sinf(s_yaw);
    s_fwd[2] = sinf(s_pitch);
}

// rows: 0 = sR * (fwd x Z), 1 = sU * (row0 x fwd), 2 = -fwd, 3 = position; signs and w column copied from the
// game's own matrix at entry, so the handedness is exactly the game's
static void BuildMatrix()
{
    float r[3] = {sinf(s_yaw), -cosf(s_yaw), 0.0f}, u[3];
    DirFromAngles();
    Cross3(r, s_fwd, u);
    float *m = s_mat.m;
    for (int i = 0; i < 3; i++) {
        m[0 + i] = s_sR * r[i];
        m[4 + i] = s_sU * u[i];
        m[8 + i] = -s_fwd[i];
        m[12 + i] = s_pos[i];
    }
    m[3] = s_orig.m[3]; m[7] = s_orig.m[7]; m[11] = s_orig.m[11]; m[15] = s_orig.m[15];
    s_matValid = true;
}

// ---- enter / exit ----
static void Unavailable(const char *why)
{
    Menu_Status("playerFly: unavailable");
    Log("fly: unavailable - %s", why);
}

static void Enter()
{
    void *game = NULL, *actor = NULL, *cam;
    GameVec ap;
    GameMat m;
    if (!Game_GetGame(&game) || !game) return Unavailable("no game object");
    if (!(cam = Game_CameraRaw(game))) return Unavailable("no camera (loading?)");
    if (!Game_GetActor(game, &actor) || !actor) return Unavailable("no player actor (loading?)");
    if (!Game_ActorGetPos(actor, &ap)) return Unavailable("player getPos failed");
    if (ap.x == 0 && ap.y == 0 && ap.z == 0) return Unavailable("player position is 0,0,0 (loading?)");
    if (!Game_CamGetMatrix(cam, &m)) return Unavailable("camera matrix unreadable");
    for (int i = 0; i < 16; i++)
        if (!_finite(m.m[i])) return Unavailable("camera matrix is not finite");
    float f[3] = {-m.m[8], -m.m[9], -m.m[10]};
    float len = sqrtf(Dot3(f, f));
    if (!(len > 0.5f && len < 2.0f)) return Unavailable("camera matrix row 2 is not a unit vector");

    DWORD flags = 0;
    s_pause0 = s_pause1 = -1;
    Game_PauseState(game, &s_pause0, &flags);
    if (!Game_SetPaused(game, true)) return Unavailable("setPaused(true) failed");
    Game_PauseState(game, &s_pause1, NULL);
    s_pausedByUs = s_pause1 == s_pause0 + 1;
    if (!s_pausedByUs)
        Log("fly: WARNING setPaused(true) did not raise the pause counter (%d -> %d, game+0x10=%08X, bit 0x40=%d): "
            "flying WITHOUT pause, setPaused(false) will not be called", s_pause0, s_pause1, flags, (flags >> 6) & 1);

    s_game = game; s_cam = cam; s_actor = actor; s_orig = m;
    for (int i = 0; i < 3; i++) {
        f[i] /= len;
        s_pos[i] = m.m[12 + i];
    }
    float sp = f[2] > kMaxSinPitch ? kMaxSinPitch : (f[2] < -kMaxSinPitch ? -kMaxSinPitch : f[2]);
    s_pitch = asinf(sp);
    s_yaw = atan2f(f[1], f[0]);
    DirFromAngles();
    float r[3] = {sinf(s_yaw), -cosf(s_yaw), 0.0f}, u[3];
    Cross3(r, s_fwd, u);
    s_sR = Dot3(&m.m[0], r) < 0 ? -1.0f : 1.0f;
    s_sU = Dot3(&m.m[4], u) < 0 ? -1.0f : 1.0f;
    BuildMatrix();

    if (s_probe.active) Log("fly: previous teleport probe cancelled");
    s_probe.active = false;
    s_tickCalls = s_updCalls = s_aWrites = s_bCalls = s_mouseTotal = 0;
    s_nCallers = 0;
    s_frames = 0;
    s_speedMul = 1;
    Input_ResetFlight();
    InterlockedExchange(&s_reqTeleport, 0);
    InterlockedExchange(&s_reqAbort, 0);
    QueryPerformanceFrequency(&s_freq);
    QueryPerformanceCounter(&s_last);
    s_enterTick = GetTickCount();
    InterlockedExchange(&g_flying, 1);
    Input_UpdateBlock();

    Log("fly: ENTER camera=%s teleport=%s | game=%08X pause %d -> %d (game+0x10=%08X) | actor=%08X pos=(%.2f %.2f %.2f)",
        Flight_CamModeName(g_flyCamMode), g_teleportMode ? "setPos+repeat" : "setPos", (unsigned)(UINT_PTR)game,
        s_pause0, s_pause1, flags, (unsigned)(UINT_PTR)actor, ap.x, ap.y, ap.z);
    Log("fly:   cam=%08X vt=%08X pos=(%.2f %.2f %.2f) dir=(%.3f %.3f %.3f) row0=(%.3f %.3f %.3f) row1=(%.3f %.3f %.3f) "
        "w=(%g %g %g %g) signs right=%+.0f up=%+.0f",
        (unsigned)(UINT_PTR)cam, g_testMode ? 0u : (unsigned)(UINT_PTR) * (void **)cam, s_pos[0], s_pos[1], s_pos[2],
        f[0], f[1], f[2], m.m[0], m.m[1], m.m[2], m.m[4], m.m[5], m.m[6], m.m[3], m.m[7], m.m[11], m.m[15], s_sR, s_sU);
    Game_LogActorMethods(actor);
    Menu_Status("playerFly: on");
}

static void Exit(const char *why, bool emergency)
{
    int c0 = -1, c1 = -1;
    bool sameGame = true, unpaused = false;
    if (emergency) {
        void *g = NULL;
        sameGame = Game_GetGame(&g) && g == s_game;
    }
    if (sameGame) Game_PauseState(s_game, &c0, NULL);
    if (s_pausedByUs) {
        s_pausedByUs = false;  // never twice, whatever happens below
        if (sameGame) unpaused = Game_SetPaused(s_game, false);
        else Log("fly: game object changed -> setPaused(false) NOT called");
        if (sameGame && !unpaused) Log("fly: ERROR setPaused(false) failed");
    }
    if (sameGame) Game_PauseState(s_game, &c1, NULL);
    InterlockedExchange(&g_flying, 0);
    Input_UpdateBlock();
    Input_ResetFlight();
    s_matValid = false;
    InterlockedExchange(&s_reqTeleport, 0);

    Log("fly: %sEXIT (%s) after %.1f s, %d frames | pause %d -> %d%s | camera hooks during flight: tick=%ld update=%ld "
        "A-writes=%ld B-replaced=%ld | mouse counts %ld",
        emergency ? "EMERGENCY " : "", why, (GetTickCount() - s_enterTick) / 1000.0f, s_frames, c0, c1,
        unpaused ? " (setPaused(false))" : "", s_tickCalls, s_updCalls, s_aWrites, s_bCalls, s_mouseTotal);
    if (s_nCallers) {
        char buf[160] = "";
        for (int i = 0; i < s_nCallers; i++) {
            char one[16];
            sprintf_s(one, " %08X", (unsigned)(UINT_PTR)s_callers[i]);
            strcat_s(buf, one);
        }
        Log("fly:   game->vt[0x7C] callers during flight:%s (render 0x00B3CD7E)", buf);
    }
    if (emergency) Menu_Status("playerFly: aborted (%s)", why);
    else Menu_Status("playerFly: off");
}

static void SafeExit(const char *why)
{
    __try {
        Exit(why, true);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        s_pausedByUs = false;
        InterlockedExchange(&g_flying, 0);
        Input_UpdateBlock();
        Log("fly: ERROR exception %08X during emergency exit", GetExceptionCode());
    }
}

// the objects we hold must still be the game's current ones
static bool Validate()
{
    void *g = NULL, *a = NULL, *c;
    if (!Game_GetGame(&g) || g != s_game) {
        Exit("game object lost", true);
        return false;
    }
    c = Game_CameraRaw(g);
    if (c != s_cam) {
        Exit(c ? "camera changed" : "camera lost", true);
        return false;
    }
    if (!Game_GetActor(g, &a) || a != s_actor) {
        Exit(a ? "player changed" : "player lost", true);
        return false;
    }
    return true;
}

static void Update()
{
    LARGE_INTEGER now;
    long mx, my;
    QueryPerformanceCounter(&now);
    float dt = (float)((double)(now.QuadPart - s_last.QuadPart) / (double)s_freq.QuadPart);
    s_last = now;
    dt = dt < 0 ? 0 : (dt > kMaxDt ? kMaxDt : dt);
    Input_TakeMouse(&mx, &my);
    s_mouseTotal += labs(mx) + labs(my);
    if (g_menuOpen) return;  // the menu has the keys: the flight is not controlled

    s_yaw -= mx * kMouseSens * s_sR;  // screen right = row 0 = sR * (fwd x Z)
    s_pitch -= my * kMouseSens;       // mouse down = look down
    float lim = asinf(kMaxSinPitch);
    s_pitch = s_pitch > lim ? lim : (s_pitch < -lim ? -lim : s_pitch);
    if (s_yaw > 3.14159265f) s_yaw -= 6.28318531f;
    if (s_yaw < -3.14159265f) s_yaw += 6.28318531f;
    DirFromAngles();

    s_speedMul = Input_KeyHeld(VK_SHIFT) ? 3.0f : (Input_KeyHeld(VK_CONTROL) ? 0.5f : 1.0f);
    float v = kBaseSpeed * s_speedMul * dt;
    float fb = (float)Input_KeyHeld('W') - (float)Input_KeyHeld('S');
    float lr = (float)Input_KeyHeld('D') - (float)Input_KeyHeld('A');
    float ud = (float)Input_KeyHeld(VK_SPACE) - (float)Input_KeyHeld('C');
    float right[3] = {s_sR * sinf(s_yaw), -s_sR * cosf(s_yaw), 0.0f};
    for (int i = 0; i < 3; i++) s_pos[i] += (s_fwd[i] * fb + right[i] * lr) * v;  // flies where it looks
    s_pos[2] += ud * v;                                                          // world Z up/down
}

static void Teleport()
{
    GameVec before, after, target = {s_pos[0], s_pos[1], s_pos[2], 0}, dir = {s_fwd[0], s_fwd[1], s_fwd[2], 0};
    float yaw = 0;
    void *game = s_game, *actor = s_actor;
    Game_ActorGetPos(actor, &before);
    bool ok = Game_ActorTeleport(actor, &target, &dir, &yaw);
    Game_ActorGetPos(actor, &after);
    Log("fly: TELEPORT %s method=%s target=(%.2f %.2f %.2f) yaw=%.1f | before=(%.2f %.2f %.2f) after=(%.2f %.2f %.2f)",
        ok ? "ok" : "FAILED", g_teleportMode ? "setPos+repeat" : "setPos", target.x, target.y, target.z, yaw,
        before.x, before.y, before.z, after.x, after.y, after.z);
    Exit("teleport", false);
    Menu_Status(ok ? "playerFly: teleported" : "playerFly: teleport failed");
    if (!ok) return;
    s_probe.active = true;
    s_probe.game = game;
    s_probe.actor = actor;
    s_probe.target = target;
    s_probe.dir = dir;
    s_probe.t0 = GetTickCount();
    s_probe.stage = 0;
    s_probe.repeat = g_teleportMode == 1 ? 3 : 0;
}

static void Probe()
{
    void *g = NULL, *a = NULL;
    GameVec p;
    float yaw;
    if (!s_probe.active) return;
    if (!Game_GetGame(&g) || g != s_probe.game || !Game_GetActor(g, &a) || a != s_probe.actor) {
        Log("fly: probe stopped: player lost or changed");
        s_probe.active = false;
        return;
    }
    if (s_probe.repeat > 0) {
        GameVec q;
        Game_ActorGetPos(a, &p);
        bool ok = Game_ActorTeleport(a, &s_probe.target, &s_probe.dir, &yaw);
        Game_ActorGetPos(a, &q);
        s_probe.repeat--;
        Log("fly: probe re-apply setPos %s (%d left): before=(%.2f %.2f %.2f) after=(%.2f %.2f %.2f)", ok ? "ok" : "FAILED",
            s_probe.repeat, p.x, p.y, p.z, q.x, q.y, q.z);
    }
    DWORD el = GetTickCount() - s_probe.t0;
    const char *label = NULL;
    if (s_probe.stage == 0) label = "after 1st unpaused frame", s_probe.stage = 1;
    else if (s_probe.stage == 1 && el >= 500) label = "+0.5 s", s_probe.stage = 2;
    else if (s_probe.stage == 2 && el >= 2000) label = "+2 s", s_probe.stage = 3, s_probe.active = false;
    if (!label) return;
    Game_ActorGetPos(a, &p);
    float dx = p.x - s_probe.target.x, dy = p.y - s_probe.target.y, dz = p.z - s_probe.target.z;
    Log("fly: probe %s (%lu ms): pos=(%.2f %.2f %.2f) offset from target=(%.2f %.2f %.2f) horizontal=%.2f", label, el,
        p.x, p.y, p.z, dx, dy, dz, sqrtf(dx * dx + dy * dy));
}

// ---- per frame ----
static void FrameInner()
{
    Game_TestTick();
    Probe();
    if (InterlockedExchange(&s_reqAbort, 0) && g_flying) Exit(s_abortWhy ? s_abortWhy : "abort", true);
    if (InterlockedExchange(&s_reqToggle, 0)) {
        if (g_flying) Exit("F2 / menu", false);
        else Enter();
    }
    if (!g_flying) {
        InterlockedExchange(&s_reqTeleport, 0);
        return;
    }
    if (!Validate()) return;
    if (g_testMode && InterlockedExchange(&s_testThrow, 0)) {
        Log("test: forcing an exception in the flight frame");
        *(volatile int *)0 = 1;
    }
    s_frames++;
    if (InterlockedExchange(&s_reqTeleport, 0) && !g_menuOpen) {
        Teleport();
        return;
    }
    Update();
    BuildMatrix();
    if (g_flyCamMode == FLYCAM_A && Game_CamSetMatrix(s_cam, &s_mat)) s_aWrites++;
}

static int FrameFilter(EXCEPTION_POINTERS *ep)
{
    static volatile LONG budget;
    if (LogBudget(&budget, 20))
        Log("fly: ERROR exception %08X at %08X in the flight frame", ep->ExceptionRecord->ExceptionCode,
            (unsigned)(UINT_PTR)ep->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;
}

void Flight_Frame()
{
    __try {
        FrameInner();
    } __except (FrameFilter(GetExceptionInformation())) {
        if (g_flying) SafeExit("exception in the flight frame");
    }
}

void Flight_OnUnload(bool processExit)
{
    if (!g_flying) return;
    if (processExit) Log("fly: process exit during flight (the game is closing, pause left as is)");
    else SafeExit("DLL unload");
}

// ---- called from the game hooks (game thread) ----
void Flight_AfterCamUpdate(void *cam, int which)
{
    if (!g_flying || cam != s_cam) return;
    if (which == 0) s_tickCalls++;
    else s_updCalls++;
    if (g_flyCamMode == FLYCAM_A && s_matValid && Game_CamSetMatrix(cam, &s_mat)) s_aWrites++;
}

bool Flight_RenderMatrix(float *out, void *caller)
{
    if (!g_flying || !s_matValid) return false;
    int i = 0;
    while (i < s_nCallers && s_callers[i] != caller) i++;
    if (i == s_nCallers && s_nCallers < 8) s_callers[s_nCallers++] = caller;
    if (g_flyCamMode != FLYCAM_B) return false;
    memcpy(out, s_mat.m, sizeof(s_mat.m));
    s_bCalls++;
    return true;
}

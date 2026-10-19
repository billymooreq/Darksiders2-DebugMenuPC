// playerFly, after the Xbox PlayerFlyCommand 0x825B42C8 + FlyCameraState 0x825BF7F8 (notes/playerfly_xbox.md):
// the game is paused (game->vt[0x68]), we fly our own camera, teleport = actor setPos vt[0x34] + setYaw vt[0x138]
// (the same pair the game's scripted teleportTo 0x00796B71 uses).
// Everything runs in the game thread, once per frame, from the hook of the game frame 0x00B3D3E4 (before the
// original frame: requests, pause and teleport happen between frames, and the camera matrix is ready before
// 0x00B3CD27 hands it to the renderer). Without that hook (offline test) it runs from Present.
// Free camera: game->vt[0x7C] 0x004CD9DA (the matrix 0x00B3CD27 gives the renderer, a copy of StateCamera+0xC0)
// returns our matrix - only the picture changes, the game camera keeps following the player ("camera B" of v0.3;
// camera A, writing StateCamera itself, was removed in task 05).
// Pause balance: exactly one setPaused(false) for our one setPaused(true), and only if that call really raised
// the counter game+0xF8. Emergency exit (with unpause) only on real failures: exception, player / camera / game
// object lost or changed (level load), DLL unload. Losing the focus (Alt+Tab) freezes the flight; Reset is ignored.
// Task 05, after the teleport (notes/playerfly_pc.md §5):
//   - the game camera is put behind the player like the game does it itself (StateCamera reset 0x005B7348 +
//     default state 0x005D5B1A), the player faces where the free camera looked, so the camera looks there too;
//   - the picture blends from the free camera into the game camera in 0.3 s (same vt[0x7C] hook);
//   - hover: the game runs, but before every frame the player is put back at the teleport point and his vertical
//     speed (+0x3B8) is zeroed; the first game control key/button lets him go (the key reaches the game).
#include "common.h"
#include <math.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>

bool g_hoverAfterTeleport = true;
volatile LONG g_flying;

static const float kBaseSpeed = 375.0f * 2.0f;  // Xbox FlyCameraState: 375 * dt * 2.0 units per second
static const float kMaxSinPitch = 0.95f;         // Xbox: |forward . up| <= 0.95
static const float kMouseSens = 0.0025f;         // radians per raw mouse count
static const float kMaxDt = 0.1f;
static const float kBlendTime = 0.3f;            // free camera -> game camera after the teleport
static const float kHoverMovedByGame = 40.0f;    // the game moved the player that far by itself: stop holding
static const DWORD kClickAfterFocusMs = 300;     // a click that gives the focus back is not a teleport

static volatile LONG s_reqToggle, s_reqTeleport, s_reqAbort, s_testThrow;
static const char *volatile s_abortWhy;

static void *s_game, *s_cam, *s_actor;
static bool s_pausedByUs, s_matValid, s_frozen;
static GameMat s_orig, s_mat;
static float s_pos[3], s_fwd[3], s_yaw, s_pitch, s_sR = 1, s_sU = 1, s_speedMul = 1;
static LARGE_INTEGER s_freq, s_last;
static DWORD s_enterTick;
static int s_frames, s_frozenCount, s_pause0, s_pause1;
// one flight's statistics (log at exit)
static LONG s_bCalls, s_mouseTotal;
static void *s_callers[8];
static int s_nCallers;

// position probes after a teleport without hover: did the body stay where we put it?
static struct {
    bool active;
    void *game, *actor;
    GameVec target;
    DWORD t0;
    int stage;
} s_probe;

// blend of the rendered camera after a teleport
static struct {
    bool active;
    GameMat from;
    LARGE_INTEGER t0;
    LONG calls;
    bool midLogged;
} s_blend;

// hover after a teleport
static struct {
    bool active;
    void *game, *actor;
    GameVec target;
    DWORD t0, lastLog;
    int frames, setPosCalls;
    float maxDrop, maxVz;
    bool vzWarned;
} s_hover;

// the game camera 1 s after the teleport: did it keep looking where the free camera looked?
static struct {
    bool active;
    void *game, *cam;
    float fwd[3];
    DWORD t0;
} s_camProbe;

// what happens after the hover let go: position and vertical speed at +0.1 s / +0.5 s
static struct {
    bool active;
    void *game, *actor;
    GameVec p0;
    DWORD t0;
    int stage;
} s_rel;

float Flight_SpeedMul() { return s_speedMul; }
bool Flight_Hovering() { return s_hover.active; }
bool Flight_NoFocus() { return g_flying && s_frozen; }

bool Flight_GetCamPos(GameVec *out)
{
    if (!g_flying) return false;
    out->x = s_pos[0]; out->y = s_pos[1]; out->z = s_pos[2]; out->w = 0;
    return true;
}

void Flight_RequestToggle() { InterlockedExchange(&s_reqToggle, 1); }
void Flight_RequestTeleport()
{
    static volatile LONG budget;
    if (!g_flying) return;
    if (!Input_WindowActive() || Input_ActiveForMs() < kClickAfterFocusMs) {
        if (LogBudget(&budget, 20)) Log("fly: teleport request ignored (no focus / focus just came back)");
        return;
    }
    InterlockedExchange(&s_reqTeleport, 1);
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

// rotation part (rows 0..2 of a row-major 4x4) <-> unit quaternion (x, y, z, w); used as a consistent pair
static float Det3(const float *m)
{
    float c[3];
    Cross3(&m[4], &m[8], c);
    return Dot3(&m[0], c);
}

static void RowsToQuat(const float *m, float *q)
{
    float r[3][3];
    for (int i = 0; i < 3; i++) {
        float len = sqrtf(Dot3(&m[i * 4], &m[i * 4]));
        if (len < 1e-6f) len = 1;
        for (int j = 0; j < 3; j++) r[i][j] = m[i * 4 + j] / len;
    }
    float tr = r[0][0] + r[1][1] + r[2][2], s;
    if (tr > 0) {
        s = sqrtf(tr + 1.0f) * 2;
        q[3] = 0.25f * s; q[0] = (r[2][1] - r[1][2]) / s; q[1] = (r[0][2] - r[2][0]) / s; q[2] = (r[1][0] - r[0][1]) / s;
    } else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
        s = sqrtf(1.0f + r[0][0] - r[1][1] - r[2][2]) * 2;
        q[3] = (r[2][1] - r[1][2]) / s; q[0] = 0.25f * s; q[1] = (r[0][1] + r[1][0]) / s; q[2] = (r[0][2] + r[2][0]) / s;
    } else if (r[1][1] > r[2][2]) {
        s = sqrtf(1.0f + r[1][1] - r[0][0] - r[2][2]) * 2;
        q[3] = (r[0][2] - r[2][0]) / s; q[0] = (r[0][1] + r[1][0]) / s; q[1] = 0.25f * s; q[2] = (r[1][2] + r[2][1]) / s;
    } else {
        s = sqrtf(1.0f + r[2][2] - r[0][0] - r[1][1]) * 2;
        q[3] = (r[1][0] - r[0][1]) / s; q[0] = (r[0][2] + r[2][0]) / s; q[1] = (r[1][2] + r[2][1]) / s; q[2] = 0.25f * s;
    }
}

static void QuatToRows(const float *q, float *m)
{
    float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y - z * w);     m[2] = 2 * (x * z + y * w);
    m[4] = 2 * (x * y + z * w);     m[5] = 1 - 2 * (x * x + z * z); m[6] = 2 * (y * z - x * w);
    m[8] = 2 * (x * z - y * w);     m[9] = 2 * (y * z + x * w);     m[10] = 1 - 2 * (x * x + y * y);
}

// out = a..b at t (position lerp, rotation slerp; rows lerp if a matrix is not a proper rotation)
static void BlendMatrix(float *out, const float *a, const float *b, float t)
{
    float res[16];
    memcpy(res, b, sizeof(res));  // w column (and anything unknown) from the game's matrix
    for (int i = 0; i < 3; i++) res[12 + i] = a[12 + i] + (b[12 + i] - a[12 + i]) * t;
    if (Det3(a) > 0.5f && Det3(b) > 0.5f) {
        float qa[4], qb[4], q[4];
        RowsToQuat(a, qa);
        RowsToQuat(b, qb);
        float d = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3];
        if (d < 0) {
            for (int i = 0; i < 4; i++) qb[i] = -qb[i];
            d = -d;
        }
        float wa = 1 - t, wb = t;
        if (d < 0.9995f) {
            float th = acosf(d), sn = sinf(th);
            wa = sinf((1 - t) * th) / sn;
            wb = sinf(t * th) / sn;
        }
        float len = 0;
        for (int i = 0; i < 4; i++) {
            q[i] = wa * qa[i] + wb * qb[i];
            len += q[i] * q[i];
        }
        len = sqrtf(len);
        for (int i = 0; i < 4; i++) q[i] /= len;
        QuatToRows(q, res);
    } else {
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) res[r * 4 + c] = a[r * 4 + c] + (b[r * 4 + c] - a[r * 4 + c]) * t;
    }
    memcpy(out, res, sizeof(res));
}

static float Elapsed(const LARGE_INTEGER &t0)
{
    LARGE_INTEGER now, f;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&f);
    return (float)((double)(now.QuadPart - t0.QuadPart) / (double)f.QuadPart);
}

// ---- hover ----
static void HoverStop(const char *why)
{
    if (!s_hover.active) return;
    s_hover.active = false;
    Log("hover: stopped (%s) after %.1f s, %d frames", why, (GetTickCount() - s_hover.t0) / 1000.0f, s_hover.frames);
    Menu_Status("hover: off (%s)", why);
}

static void HoverStart(void *game, void *actor, const GameVec &target)
{
    float vz = 0;
    bool vzOk = Game_ActorGetVz(actor, &vz);
    memset(&s_hover, 0, sizeof(s_hover));
    s_hover.active = true;
    s_hover.game = game;
    s_hover.actor = actor;
    s_hover.target = target;
    s_hover.t0 = s_hover.lastLog = GetTickCount();
    Input_HoverArm();  // the teleport press itself (Enter / LMB still held) never releases the hover
    s_rel.active = false;
    Log("hover: START at (%.2f %.2f %.2f), vertical speed +0x3B8 = %s%.2f", target.x, target.y, target.z,
        vzOk ? "" : "(unknown class) ", vz);
}

static const char *KeyName(int vk)
{
    static char buf[32];
    switch (vk) {
    case VK_LBUTTON: return "left mouse button";
    case VK_RBUTTON: return "right mouse button";
    case VK_MBUTTON: return "middle mouse button";
    case VK_XBUTTON1: return "mouse X1";
    case VK_XBUTTON2: return "mouse X2";
    case VK_SPACE: return "Space";
    case VK_SHIFT: return "Shift";
    case VK_CONTROL: return "Ctrl";
    }
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) sprintf_s(buf, "%c", vk);
    else sprintf_s(buf, "vk %02X", vk);
    return buf;
}

// before every game frame while hovering: the player back at the point, vertical speed 0
static void HoverFrame()
{
    if (!s_hover.active) return;
    void *g = NULL, *a = NULL;
    if (!Game_GetGame(&g) || g != s_hover.game || !Game_GetActor(g, &a) || a != s_hover.actor)
        return HoverStop("player lost or changed (level load?)");
    GameVec p;
    float vz = 0;
    if (!Game_ActorGetPos(a, &p)) return HoverStop("player getPos failed");
    bool vzOk = Game_ActorGetVz(a, &vz);
    float dx = p.x - s_hover.target.x, dy = p.y - s_hover.target.y, dz = p.z - s_hover.target.z;
    float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    int key = Input_HoverKey();
    if (key) {  // let go: from the teleport point with vertical speed 0; the key reaches the game this frame
        if (dist > 0.001f && dist < kHoverMovedByGame) Game_ActorSetPos(a, &s_hover.target);
        Game_ActorSetVz(a, 0);
        GameVec q;
        float vz2 = 0;
        Game_ActorGetPos(a, &q);
        Game_ActorGetVz(a, &vz2);
        Log("hover: RELEASED by %s after %.1f s, %d frames: pos=(%.2f %.2f %.2f) vz %.2f -> %.2f | hold stats: setPos %d, "
            "max drop/frame %.3f, max |vz| before reset %.2f",
            KeyName(key), (GetTickCount() - s_hover.t0) / 1000.0f, s_hover.frames, q.x, q.y, q.z, vz, vz2,
            s_hover.setPosCalls, s_hover.maxDrop, s_hover.maxVz);
        s_hover.active = false;
        s_rel.active = true;
        s_rel.game = g;
        s_rel.actor = a;
        s_rel.p0 = q;
        s_rel.t0 = GetTickCount();
        s_rel.stage = 0;
        return;
    }
    if (dist > kHoverMovedByGame) {
        Log("hover: the game moved the player by %.1f to (%.2f %.2f %.2f) (respawn / hit / script?)", dist, p.x, p.y, p.z);
        return HoverStop("player moved by the game");
    }
    s_hover.frames++;
    if (dist > 0.001f) {
        Game_ActorSetPos(a, &s_hover.target);
        s_hover.setPosCalls++;
        if (-dz > s_hover.maxDrop) s_hover.maxDrop = -dz;
    }
    if (vzOk) {
        if (fabsf(vz) > s_hover.maxVz) s_hover.maxVz = fabsf(vz);
        Game_ActorSetVz(a, 0);
    } else if (!s_hover.vzWarned) {
        s_hover.vzWarned = true;
        Log("hover: WARNING vertical speed unknown for this actor class -> holding by position only");
    }
    DWORD now = GetTickCount();
    if (now - s_hover.lastLog >= 2000) {
        s_hover.lastLog = now;
        Log("hover: %.1f s, %d frames, setPos %d, max drop/frame %.3f, max |vz| before reset %.2f",
            (now - s_hover.t0) / 1000.0f, s_hover.frames, s_hover.setPosCalls, s_hover.maxDrop, s_hover.maxVz);
    }
}

static void CamProbe()
{
    if (!s_camProbe.active || GetTickCount() - s_camProbe.t0 < 1000) return;
    s_camProbe.active = false;
    void *g = NULL;
    GameMat m;
    if (!Game_GetGame(&g) || g != s_camProbe.game || Game_CameraRaw(g) != s_camProbe.cam ||
        !Game_CamGetMatrix(s_camProbe.cam, &m))
        return;
    float d[3] = {-m.m[8], -m.m[9], -m.m[10]};
    float h1 = sqrtf(d[0] * d[0] + d[1] * d[1]), h0 = sqrtf(s_camProbe.fwd[0] * s_camProbe.fwd[0] + s_camProbe.fwd[1] * s_camProbe.fwd[1]);
    float yawDiff = 0;
    if (h1 > 1e-4f && h0 > 1e-4f) {
        yawDiff = (atan2f(d[1], d[0]) - atan2f(s_camProbe.fwd[1], s_camProbe.fwd[0])) * 57.29578f;
        while (yawDiff > 180) yawDiff -= 360;
        while (yawDiff < -180) yawDiff += 360;
    }
    Log("fly: game camera 1 s after the teleport: pos (%.1f %.1f %.1f) dir (%.3f %.3f %.3f) | free camera dir (%.3f %.3f %.3f) "
        "| yaw difference %.1f deg", m.m[12], m.m[13], m.m[14], d[0], d[1], d[2], s_camProbe.fwd[0], s_camProbe.fwd[1],
        s_camProbe.fwd[2], yawDiff);
}

static void ReleaseProbe()
{
    if (!s_rel.active) return;
    void *g = NULL, *a = NULL;
    if (!Game_GetGame(&g) || g != s_rel.game || !Game_GetActor(g, &a) || a != s_rel.actor) {
        s_rel.active = false;
        return;
    }
    DWORD el = GetTickCount() - s_rel.t0;
    const char *label = NULL;
    if (s_rel.stage == 0 && el >= 100) label = "+0.1 s", s_rel.stage = 1;
    else if (s_rel.stage == 1 && el >= 500) label = "+0.5 s", s_rel.stage = 2, s_rel.active = false;
    if (!label) return;
    GameVec p;
    float vz = 0;
    Game_ActorGetPos(a, &p);
    Game_ActorGetVz(a, &vz);
    Log("hover: after release %s (%lu ms): pos=(%.2f %.2f %.2f) moved (%.2f %.2f %.2f) vz=%.2f", label, el, p.x, p.y,
        p.z, p.x - s_rel.p0.x, p.y - s_rel.p0.y, p.z - s_rel.p0.z, vz);
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

    HoverStop("F2 -> playerFly");
    if (s_blend.active) s_blend.active = false;
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
    s_probe.active = s_rel.active = s_camProbe.active = false;
    s_bCalls = s_mouseTotal = 0;
    s_nCallers = 0;
    s_frames = s_frozenCount = 0;
    s_frozen = false;
    s_speedMul = 1;
    Input_ResetFlight();
    InterlockedExchange(&s_reqTeleport, 0);
    InterlockedExchange(&s_reqAbort, 0);
    QueryPerformanceFrequency(&s_freq);
    QueryPerformanceCounter(&s_last);
    s_enterTick = GetTickCount();
    InterlockedExchange(&g_flying, 1);
    Input_UpdateBlock();

    Log("fly: ENTER | game=%08X pause %d -> %d (game+0x10=%08X) | actor=%08X pos=(%.2f %.2f %.2f)",
        (unsigned)(UINT_PTR)game, s_pause0, s_pause1, flags, (unsigned)(UINT_PTR)actor, ap.x, ap.y, ap.z);
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
    s_frozen = false;
    InterlockedExchange(&s_reqTeleport, 0);

    Log("fly: %sEXIT (%s) after %.1f s, %d frames (%d without focus) | pause %d -> %d%s | render matrix replaced %ld "
        "times | mouse counts %ld",
        emergency ? "EMERGENCY " : "", why, (GetTickCount() - s_enterTick) / 1000.0f, s_frames, s_frozenCount, c0, c1,
        unpaused ? " (setPaused(false))" : "", s_bCalls, s_mouseTotal);
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
        s_frozen = false;
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
    void *game = s_game, *actor = s_actor, *cam = s_cam;
    GameMat from = s_mat;
    float vz = 0;
    Game_ActorGetPos(actor, &before);
    bool ok = Game_ActorTeleport(actor, &target, &dir, &yaw);
    bool vzOk = ok && Game_ActorGetVz(actor, &vz) && Game_ActorSetVz(actor, 0);  // no fall speed kept from before
    Game_ActorGetPos(actor, &after);
    Log("fly: TELEPORT %s target=(%.2f %.2f %.2f) yaw=%.1f | before=(%.2f %.2f %.2f) after=(%.2f %.2f %.2f) | vz %.2f -> %s",
        ok ? "ok" : "FAILED", target.x, target.y, target.z, yaw, before.x, before.y, before.z, after.x, after.y, after.z,
        vz, vzOk ? "0" : "unchanged (unknown class)");
    Exit("teleport", false);
    Menu_Status(ok ? "playerFly: teleported" : "playerFly: teleport failed");
    if (!ok) return;

    // the game camera behind the player, looking where the free camera looked; the picture blends into it
    GameMat c0, c1;
    bool m0 = Game_CamGetMatrix(cam, &c0);
    bool camOk = Game_CamResetBehindPlayer(cam);
    bool m1 = Game_CamGetMatrix(cam, &c1);
    if (m0 && m1)
        Log("fly: camera reset %s | game camera pos (%.1f %.1f %.1f) dir (%.3f %.3f %.3f) -> pos (%.1f %.1f %.1f) dir "
            "(%.3f %.3f %.3f) | free camera dir (%.3f %.3f %.3f)",
            camOk ? "ok" : "NOT done", c0.m[12], c0.m[13], c0.m[14], -c0.m[8], -c0.m[9], -c0.m[10], c1.m[12], c1.m[13],
            c1.m[14], -c1.m[8], -c1.m[9], -c1.m[10], s_fwd[0], s_fwd[1], s_fwd[2]);
    else
        Log("fly: camera reset %s", camOk ? "ok" : "NOT done");
    s_camProbe.active = true;
    s_camProbe.game = game;
    s_camProbe.cam = cam;
    s_camProbe.t0 = GetTickCount();
    memcpy(s_camProbe.fwd, s_fwd, sizeof(s_fwd));
    s_blend.active = true;
    s_blend.from = from;
    s_blend.calls = 0;
    s_blend.midLogged = false;
    QueryPerformanceCounter(&s_blend.t0);

    if (g_hoverAfterTeleport) {
        HoverStart(game, actor, target);
        return;
    }
    s_probe.active = true;
    s_probe.game = game;
    s_probe.actor = actor;
    s_probe.target = target;
    s_probe.t0 = GetTickCount();
    s_probe.stage = 0;
}

static void Probe()
{
    void *g = NULL, *a = NULL;
    GameVec p;
    float vz = 0;
    if (!s_probe.active) return;
    if (!Game_GetGame(&g) || g != s_probe.game || !Game_GetActor(g, &a) || a != s_probe.actor) {
        Log("fly: probe stopped: player lost or changed");
        s_probe.active = false;
        return;
    }
    DWORD el = GetTickCount() - s_probe.t0;
    const char *label = NULL;
    if (s_probe.stage == 0) label = "after 1st unpaused frame", s_probe.stage = 1;
    else if (s_probe.stage == 1 && el >= 500) label = "+0.5 s", s_probe.stage = 2;
    else if (s_probe.stage == 2 && el >= 2000) label = "+2 s", s_probe.stage = 3, s_probe.active = false;
    if (!label) return;
    Game_ActorGetPos(a, &p);
    Game_ActorGetVz(a, &vz);
    float dx = p.x - s_probe.target.x, dy = p.y - s_probe.target.y, dz = p.z - s_probe.target.z;
    Log("fly: probe %s (%lu ms): pos=(%.2f %.2f %.2f) offset from target=(%.2f %.2f %.2f) horizontal=%.2f vz=%.2f", label,
        el, p.x, p.y, p.z, dx, dy, dz, sqrtf(dx * dx + dy * dy), vz);
}

// ---- per frame ----
static void FrameInner()
{
    Game_TestTick();
    Probe();
    ReleaseProbe();
    CamProbe();
    if (InterlockedExchange(&s_reqAbort, 0) && g_flying) Exit(s_abortWhy ? s_abortWhy : "abort", true);
    if (InterlockedExchange(&s_reqToggle, 0)) {
        if (g_flying) Exit("F2 / menu", false);
        else Enter();
    }
    if (!g_flying) {
        InterlockedExchange(&s_reqTeleport, 0);
        HoverFrame();
        return;
    }
    if (!Validate()) return;
    if (g_testMode && InterlockedExchange(&s_testThrow, 0)) {
        Log("test: forcing an exception in the flight frame");
        *(volatile int *)0 = 1;
    }
    s_frames++;
    if (!Input_WindowActive()) {  // Alt+Tab: frozen - pause stays, camera stays, nothing accumulates
        if (!s_frozen) Log("fly: no focus -> frozen (pause kept, keys reset)");
        s_frozen = true;
        s_frozenCount++;
        InterlockedExchange(&s_reqTeleport, 0);
        Input_ResetFlight();
        QueryPerformanceCounter(&s_last);
        return;
    }
    if (s_frozen) {
        s_frozen = false;
        Log("fly: focus back -> flight resumes at (%.2f %.2f %.2f) after %d frozen frames", s_pos[0], s_pos[1], s_pos[2],
            s_frozenCount);
        QueryPerformanceCounter(&s_last);
        Input_ResetFlight();
    }
    if (InterlockedExchange(&s_reqTeleport, 0) && !g_menuOpen) {
        Teleport();
        return;
    }
    Update();
    BuildMatrix();
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
        if (s_hover.active) {
            s_hover.active = false;
            Log("hover: stopped (exception)");
        }
    }
    Cursor_Update();
    __try {
        Game_TestRender();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void Flight_OnUnload(bool processExit)
{
    s_hover.active = false;
    s_blend.active = false;
    if (!g_flying) return;
    if (processExit) Log("fly: process exit during flight (the game is closing, pause left as is)");
    else SafeExit("DLL unload");
}

// ---- called from the game hook (game thread): game->vt[0x7C], the camera matrix for the renderer ----
bool Flight_RenderMatrix(float *out, void *caller)
{
    if (g_flying && s_matValid) {
        int i = 0;
        while (i < s_nCallers && s_callers[i] != caller) i++;
        if (i == s_nCallers && s_nCallers < 8) s_callers[s_nCallers++] = caller;
        memcpy(out, s_mat.m, sizeof(s_mat.m));
        s_bCalls++;
        return true;
    }
    if (!s_blend.active) return false;
    float t = Elapsed(s_blend.t0) / kBlendTime;
    if (t >= 1.0f || t < 0) {
        s_blend.active = false;
        Log("fly: camera blend done (%ld replaced matrices), game camera at (%.1f %.1f %.1f) dir (%.3f %.3f %.3f)",
            s_blend.calls, out[12], out[13], out[14], -out[8], -out[9], -out[10]);
        return false;
    }
    float k = t * t * (3 - 2 * t);
    if (!s_blend.midLogged && t >= 0.5f) {
        s_blend.midLogged = true;
        float tmp[16];
        BlendMatrix(tmp, s_blend.from.m, out, k);
        Log("fly: camera blend t=%.2f: pos (%.1f %.1f %.1f) between free (%.1f %.1f %.1f) and game (%.1f %.1f %.1f)", t,
            tmp[12], tmp[13], tmp[14], s_blend.from.m[12], s_blend.from.m[13], s_blend.from.m[14], out[12], out[13], out[14]);
    }
    BlendMatrix(out, s_blend.from.m, out, k);
    s_blend.calls++;
    return true;
}

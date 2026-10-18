// Guarded reads of game objects (addresses: notes/xbox_pc_map.md, notes/review_03_menu_overlay.md).
// Every step is in its own __try; a vtable must lie in the exe image and a method in .text before we call it.
#include "common.h"
#include <math.h>

#define PC_GAME_PTR       0x0184E95Cu  // Darksiders* singleton (instance() 0x004058CF = lazy init + read)
#define PC_GET_PLAYER      0x004CDA8Fu  // Handle* __thiscall getPlayerActor(Darksiders*, Handle* out): game+8 -> vt[0x68]
#define PC_GET_CAMERA      0x00745A0Fu  // Handle* __thiscall getCamera(Darksiders*, Handle* out): game+0x138, addref
#define PC_RELEASE         0x00401017u  // void __thiscall release(obj): --obj+4, delete at 0
#define VT_ACTOR_GETPOS    0x38         // getPosition(Vec4* out): base 0x008EF6B2 (+0x90); player 0x00885362 (+0x260)
#define OFS_ACTOR_POS      0x90
#define VT_ACTOR_SETPOS    0x34         // WorldObject::setPosition(Vec*), base 0x00A7F942; teleportTo 0x00796B71 uses it
#define VT_ACTOR_SETYAW    0x138        // setYaw(float) (Xbox vt[0x13C]); teleportTo calls it after setPosition
#define VT_CAM_GETDIR      0x40         // StateCamera 0x005B5DB6: -row2 of the matrix
#define VT_CAM_GETPOS      0x44         // StateCamera 0x00585FB5: row3 of the matrix (vt[0x3C] = copy of cam+0x80)
#define OFS_CAM_POS        0xB0         // cam+0x80 is the 4x4 camera matrix, translation row at +0x30

// __thiscall through __fastcall: ECX = this, EDX unused, stack args, callee pops
typedef void **(__fastcall *GetHandle_t)(void *self, void *edx, void **out);
typedef void(__fastcall *Release_t)(void *self, void *edx);
typedef GameVec *(__fastcall *GetVec_t)(void *self, void *edx, GameVec *out);

const char *StName(int st)
{
    switch (st) {
    case ST_OK: return "ok";
    case ST_NULL: return "null";
    case ST_EXCEPTION: return "exception";
    case ST_BADPTR: return "bad vtable";
    }
    return "-";
}

static int Catch(GameInfo *gi, EXCEPTION_POINTERS *ep)
{
    gi->exCode = ep->ExceptionRecord->ExceptionCode;
    gi->exAddr = (DWORD)(UINT_PTR)ep->ExceptionRecord->ExceptionAddress;
    return EXCEPTION_EXECUTE_HANDLER;
}

static void *VMethod(void *obj, int ofs, void **vtOut)
{
    void **vt = *(void ***)obj;
    *vtOut = vt;
    if (!InImage(vt)) return NULL;
    void *fn = vt[ofs / 4];
    return InText(fn) ? fn : NULL;
}

static void ReleaseHandle(GameInfo *gi, void *obj, int *st)
{
    __try {
        ((Release_t)PC_RELEASE)(obj, NULL);
    } __except (Catch(gi, GetExceptionInformation())) {
        *st = ST_EXCEPTION;
    }
}

static int ReadActor(GameInfo *gi)
{
    void *h = NULL;
    int st = ST_OK;
    __try {
        ((GetHandle_t)PC_GET_PLAYER)(gi->game, NULL, &h);
    } __except (Catch(gi, GetExceptionInformation())) {
        return ST_EXCEPTION;
    }
    if (!h) return ST_NULL;
    gi->actor = h;
    __try {
        void *fn = VMethod(h, VT_ACTOR_GETPOS, &gi->actorVt);
        gi->actorGetPos = fn;
        if (fn) {
            ((GetVec_t)fn)(h, NULL, &gi->actorPos);
            gi->actorPosOk = true;
        } else {
            st = ST_BADPTR;
        }
        gi->actorRaw = *(GameVec *)((BYTE *)h + OFS_ACTOR_POS);
        if (InImage(gi->actorVt)) {
            gi->actorSetPos = ((void **)gi->actorVt)[VT_ACTOR_SETPOS / 4];
            gi->actorSetYaw = ((void **)gi->actorVt)[VT_ACTOR_SETYAW / 4];
        }
    } __except (Catch(gi, GetExceptionInformation())) {
        st = ST_EXCEPTION;
    }
    ReleaseHandle(gi, h, &st);
    return st;
}

static int ReadCamera(GameInfo *gi)
{
    void *c = NULL;
    int st = ST_OK;
    __try {
        ((GetHandle_t)PC_GET_CAMERA)(gi->game, NULL, &c);
    } __except (Catch(gi, GetExceptionInformation())) {
        return ST_EXCEPTION;
    }
    if (!c) return ST_NULL;
    gi->cam = c;
    __try {
        void *vt2;
        void *fnPos = VMethod(c, VT_CAM_GETPOS, &gi->camVt);
        void *fnDir = VMethod(c, VT_CAM_GETDIR, &vt2);
        gi->camGetPos = fnPos;
        if (fnPos && fnDir) {
            ((GetVec_t)fnPos)(c, NULL, &gi->camPos);
            ((GetVec_t)fnDir)(c, NULL, &gi->camDir);
            gi->camPosOk = true;
        } else {
            st = ST_BADPTR;
        }
        gi->camRaw = *(GameVec *)((BYTE *)c + OFS_CAM_POS);
    } __except (Catch(gi, GetExceptionInformation())) {
        st = ST_EXCEPTION;
    }
    ReleaseHandle(gi, c, &st);
    return st;
}

void Game_Read(GameInfo *gi)
{
    memset(gi, 0, sizeof(*gi));
    if (!g_gameMatch && !g_testMode) return;  // unknown exe: the addresses mean nothing
    __try {
        gi->game = *(void **)PC_GAME_PTR;  // read the pointer; never call instance() (it would lazily construct)
        gi->gameSt = gi->game ? ST_OK : ST_NULL;
    } __except (Catch(gi, GetExceptionInformation())) {
        gi->gameSt = ST_EXCEPTION;
    }
    if (gi->gameSt != ST_OK || !g_gameMatch) return;  // test mode: never call into a foreign exe
    gi->actorSt = ReadActor(gi);
    gi->camSt = ReadCamera(gi);
}

// ======================= flight API (task 04) =======================
// Player actor (vt 0x01266568) overrides the WorldObject slots: getPos vt[0x38] 0x00885362 = copy of +0x260;
// setPos vt[0x34] 0x00885681: +0x260 = pos (+ vt[0x208]/vt[0x204] if changed), +0x230 = pos, then the Actor
// base 0x0076A2F7 (+0xF0 and the scene node +0xD4, unless flag bits 11/12 of +0x1F0); setYaw vt[0x138]
// 0x0077DE6A -> vt[0x2C4](yaw, 0). Yaw is in degrees, 0x00ACB5FC(Vec*) is the game's dir->yaw (teleportTo).
#define PC_YAW_FROM_DIR    0x00ACB5FCu  // float __cdecl (const Vec*): atan2 in degrees [0,360)
#define VT_GAME_SETPAUSED  0x68         // 0x00B3C287: counter +0xF8, works only with game+0x10 bit 6
#define OFS_GAME_FLAGS     0x10
#define OFS_GAME_PAUSE     0xF8
#define OFS_GAME_CAMERA    0x138
#define OFS_CAM_MATRIX     0x80         // camera world matrix (getMatrix vt[0x3C], pos vt[0x44])
#define OFS_CAM_RENDER     0xC0         // copy of +0x80 after the tick (+shake etc.), game->vt[0x7C] returns it

typedef void(__fastcall *SetVec_t)(void *self, void *edx, const GameVec *v);
typedef void(__fastcall *SetFloat_t)(void *self, void *edx, float f);
typedef void(__fastcall *SetBool_t)(void *self, void *edx, int b);
typedef float(__cdecl *YawFromDir_t)(const GameVec *dir);

static volatile LONG s_exBudget;

static int CatchFl(const char *where, EXCEPTION_POINTERS *ep)
{
    if (LogBudget(&s_exBudget, 30))
        Log("game: exception %08X at %08X in %s", ep->ExceptionRecord->ExceptionCode,
            (unsigned)(UINT_PTR)ep->ExceptionRecord->ExceptionAddress, where);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void *Method(void *obj, int ofs)
{
    void *vt;
    return VMethod(obj, ofs, &vt);
}

// ---- offline test: fake objects (DBGMENU_TEST=1 and not the game) ----
static struct {
    int frames;
    bool loaded, camLost;
    GameVec actorPos;
    float actorYaw;
    GameMat cam, camRender;
    int pause;
} s_fake;
static bool Fake() { return g_testMode && !g_gameMatch; }

void Game_TestTick()
{
    if (!Fake()) return;
    if (++s_fake.frames == 1) {  // looking along +X, right = -Y, up = +Z (right-handed, Z up)
        static const float m[16] = {0, -1, 0, 0, 0, 0, 1, 0, -1, 0, 0, 0, 100, 200, 300, 1};
        memcpy(s_fake.cam.m, m, sizeof(m));
        s_fake.camRender = s_fake.cam;
    }
    if (s_fake.loaded && s_fake.actorPos.x == 0 && s_fake.actorPos.y == 0 && s_fake.actorPos.z == 0) {
        s_fake.actorPos.x = 100; s_fake.actorPos.y = 200; s_fake.actorPos.z = 250;
    }
}

// 1 = level loaded, 2 = level unloaded (player gone), 3 = camera lost, 4 = camera back
void Game_TestControl(int cmd)
{
    if (!Fake()) return;
    switch (cmd) {
    case 1: s_fake.loaded = true; break;
    case 2: s_fake.loaded = false; memset(&s_fake.actorPos, 0, sizeof(s_fake.actorPos)); break;
    case 3: s_fake.camLost = true; break;
    case 4: s_fake.camLost = false; break;
    }
    Log("test: control %d (loaded=%d camLost=%d pause=%d)", cmd, s_fake.loaded, s_fake.camLost, s_fake.pause);
}

// ---- API ----
bool Game_GetGame(void **game)
{
    *game = NULL;
    if (Fake()) {
        *game = &s_fake;
        return true;
    }
    if (!g_gameMatch) return false;
    __try {
        *game = *(void **)PC_GAME_PTR;
    } __except (CatchFl("read game pointer", GetExceptionInformation())) {
        return false;
    }
    return *game != NULL;
}

void *Game_CameraRaw(void *game)
{
    if (Fake()) return s_fake.camLost ? NULL : &s_fake.cam;
    void *cam = NULL;
    __try {
        cam = *(void **)((BYTE *)game + OFS_GAME_CAMERA);
    } __except (CatchFl("read game+0x138", GetExceptionInformation())) {
        return NULL;
    }
    return cam;
}

bool Game_GetActor(void *game, void **actor)
{
    *actor = NULL;
    if (Fake()) {
        if (s_fake.loaded) *actor = &s_fake.actorPos;
        return true;
    }
    void *h = NULL;
    __try {
        ((GetHandle_t)PC_GET_PLAYER)(game, NULL, &h);
        if (h) ((Release_t)PC_RELEASE)(h, NULL);  // the game keeps its own reference to the player
    } __except (CatchFl("getPlayerActor", GetExceptionInformation())) {
        return false;
    }
    *actor = h;
    return true;
}

bool Game_ActorGetPos(void *actor, GameVec *out)
{
    memset(out, 0, sizeof(*out));
    if (Fake()) {
        *out = s_fake.actorPos;
        return true;
    }
    __try {
        void *fn = Method(actor, VT_ACTOR_GETPOS);
        if (!fn) return false;
        ((GetVec_t)fn)(actor, NULL, out);
    } __except (CatchFl("actor getPos", GetExceptionInformation())) {
        return false;
    }
    return true;
}

bool Game_ActorTeleport(void *actor, const GameVec *pos, const GameVec *dir, float *yawOut)
{
    *yawOut = 0;
    if (Fake()) {
        s_fake.actorPos.x = pos->x; s_fake.actorPos.y = pos->y; s_fake.actorPos.z = pos->z;
        float yaw = atan2f(dir->y, dir->x) * 57.29578f;
        s_fake.actorYaw = *yawOut = yaw < 0 ? yaw + 360.0f : yaw;
        return true;
    }
    __try {
        void *setPos = Method(actor, VT_ACTOR_SETPOS);
        void *setYaw = Method(actor, VT_ACTOR_SETYAW);
        if (!setPos || !setYaw) return false;
        GameVec p = *pos, d = *dir;
        p.w = d.w = 0;
        ((SetVec_t)setPos)(actor, NULL, &p);
        float yaw = ((YawFromDir_t)PC_YAW_FROM_DIR)(&d);
        *yawOut = yaw;
        ((SetFloat_t)setYaw)(actor, NULL, yaw);
    } __except (CatchFl("actor teleport (vt[0x34] / yaw / vt[0x138])", GetExceptionInformation())) {
        return false;
    }
    return true;
}

bool Game_CamGetMatrix(void *cam, GameMat *out)
{
    if (Fake()) {
        *out = s_fake.cam;
        return true;
    }
    __try {
        memcpy(out->m, (BYTE *)cam + OFS_CAM_MATRIX, sizeof(out->m));
    } __except (CatchFl("read camera matrix", GetExceptionInformation())) {
        return false;
    }
    return true;
}

bool Game_CamSetMatrix(void *cam, const GameMat *m)
{
    if (Fake()) {
        s_fake.cam = s_fake.camRender = *m;
        return true;
    }
    __try {
        memcpy((BYTE *)cam + OFS_CAM_MATRIX, m->m, sizeof(m->m));
        memcpy((BYTE *)cam + OFS_CAM_RENDER, m->m, sizeof(m->m));  // like StateCamera setMatrix 0x005B5BEC
    } __except (CatchFl("write camera matrix", GetExceptionInformation())) {
        return false;
    }
    return true;
}

bool Game_PauseState(void *game, int *count, DWORD *flags)
{
    *count = -1;
    if (flags) *flags = 0;
    if (Fake()) {
        *count = s_fake.pause;
        if (flags) *flags = 0x40;
        return true;
    }
    __try {
        *count = *(int *)((BYTE *)game + OFS_GAME_PAUSE);
        if (flags) *flags = *(DWORD *)((BYTE *)game + OFS_GAME_FLAGS);
    } __except (CatchFl("read pause counter", GetExceptionInformation())) {
        return false;
    }
    return true;
}

bool Game_SetPaused(void *game, bool on)
{
    if (Fake()) {
        s_fake.pause += on ? 1 : -1;
        if (s_fake.pause < 0) s_fake.pause = 0;
        return true;
    }
    __try {
        void *fn = Method(game, VT_GAME_SETPAUSED);
        if (!fn) return false;
        ((SetBool_t)fn)(game, NULL, on ? 1 : 0);
    } __except (CatchFl(on ? "setPaused(true)" : "setPaused(false)", GetExceptionInformation())) {
        return false;
    }
    return true;
}

void Game_LogActorMethods(void *actor)
{
    if (Fake()) return;
    __try {
        void **vt = *(void ***)actor;
        Log("fly: actor %08X vt=%08X getPos=%08X setPos=%08X setYaw=%08X (expected 01266568 / 00885362 / 00885681 / 0077DE6A)",
            (unsigned)(UINT_PTR)actor, (unsigned)(UINT_PTR)vt, (unsigned)(UINT_PTR)vt[VT_ACTOR_GETPOS / 4],
            (unsigned)(UINT_PTR)vt[VT_ACTOR_SETPOS / 4], (unsigned)(UINT_PTR)vt[VT_ACTOR_SETYAW / 4]);
    } __except (CatchFl("actor vtable", GetExceptionInformation())) {
    }
}

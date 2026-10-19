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
// task 05 (notes/playerfly_pc.md §5):
#define PC_VT_PLAYER       0x01266568u  // player actor class: +0x3B8 vertical speed, +0x3BC its previous value
#define OFS_PLAYER_VZ      0x3B8        // < 0 = falling (PlayerFall 0x006D7A08); setPos keeps it (v0.3 log)
#define OFS_PLAYER_VZ_PREV 0x3BC
#define PC_VT_STATECAM     0x01199EA0u
#define PC_CAM_RESET       0x005B7348u  // StateCamera::reset (script resetCamera 0x004E572F): matrix behind target yaw
#define PC_CAM_TO_DEFAULT  0x005D5B1Au  // cdecl (cam, float, float): transition to the default state cam+0x160

typedef void(__fastcall *SetVec_t)(void *self, void *edx, const GameVec *v);
typedef void(__fastcall *SetFloat_t)(void *self, void *edx, float f);
typedef void(__fastcall *SetBool_t)(void *self, void *edx, int b);
typedef float(__cdecl *YawFromDir_t)(const GameVec *dir);
typedef void(__fastcall *CamReset_t)(void *cam, void *edx);
typedef int(__cdecl *CamToDefault_t)(void *cam, float blendIn, float blendOut);

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
    float actorYaw, vz;
    GameMat cam;
    int pause, resets;
} s_fake;
static const float kFakeGround = 250.0f, kFakeGravity = 1000.0f, kFakeDt = 1.0f / 60.0f;
static bool Fake() { return g_testMode && !g_gameMatch; }

void Game_TestTick()
{
    if (!Fake()) return;
    if (++s_fake.frames == 1) {  // looking along +X, right = -Y, up = +Z (right-handed, Z up)
        static const float m[16] = {0, -1, 0, 0, 0, 0, 1, 0, -1, 0, 0, 0, 100, 200, 300, 1};
        memcpy(s_fake.cam.m, m, sizeof(m));
    }
    if (s_fake.loaded && s_fake.actorPos.x == 0 && s_fake.actorPos.y == 0 && s_fake.actorPos.z == 0) {
        s_fake.actorPos.x = 100; s_fake.actorPos.y = 200; s_fake.actorPos.z = kFakeGround;
    }
    // the fake "game frame": gravity while not paused (the hover hold runs before it, like before 0x00B3D3E4)
    if (s_fake.loaded && !s_fake.pause && (s_fake.actorPos.z > kFakeGround || s_fake.vz > 0)) {
        s_fake.vz -= kFakeGravity * kFakeDt;
        s_fake.actorPos.z += s_fake.vz * kFakeDt;
        if (s_fake.actorPos.z <= kFakeGround) {
            s_fake.actorPos.z = kFakeGround;
            s_fake.vz = 0;
        }
    }
}

// the fake renderer asks for the camera matrix like 0x00B3CD27 does (game->vt[0x7C])
void Game_TestRender()
{
    if (!Fake()) return;
    GameMat m = s_fake.cam;
    Flight_RenderMatrix(m.m, (void *)0x00B3CD7E);
}

// 1 = level loaded, 2 = level unloaded (player gone), 3 = camera lost, 4 = camera back,
// 7 = the game moves the player by itself (respawn / scripted teleport)
void Game_TestControl(int cmd)
{
    if (!Fake()) return;
    switch (cmd) {
    case 1: s_fake.loaded = true; break;
    case 2: s_fake.loaded = false; memset(&s_fake.actorPos, 0, sizeof(s_fake.actorPos)); s_fake.vz = 0; break;
    case 3: s_fake.camLost = true; break;
    case 4: s_fake.camLost = false; break;
    case 7: s_fake.actorPos.x += 500; s_fake.actorPos.z = kFakeGround; s_fake.vz = 0; break;
    }
    Log("test: control %d (loaded=%d camLost=%d pause=%d pos=(%.1f %.1f %.1f) vz=%.1f)", cmd, s_fake.loaded,
        s_fake.camLost, s_fake.pause, s_fake.actorPos.x, s_fake.actorPos.y, s_fake.actorPos.z, s_fake.vz);
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

bool Game_ActorSetPos(void *actor, const GameVec *pos)
{
    if (Fake()) {
        s_fake.actorPos.x = pos->x; s_fake.actorPos.y = pos->y; s_fake.actorPos.z = pos->z;
        return true;
    }
    __try {
        void *setPos = Method(actor, VT_ACTOR_SETPOS);
        if (!setPos) return false;
        GameVec p = *pos;
        p.w = 0;
        ((SetVec_t)setPos)(actor, NULL, &p);
    } __except (CatchFl("actor setPos vt[0x34]", GetExceptionInformation())) {
        return false;
    }
    return true;
}

// only for the player class we analysed: +0x3B8 is its vertical speed there, not in other classes
static bool IsKnownPlayer(void *actor) { return *(UINT_PTR *)actor == PC_VT_PLAYER; }

bool Game_ActorGetVz(void *actor, float *vz)
{
    *vz = 0;
    if (Fake()) {
        *vz = s_fake.vz;
        return true;
    }
    __try {
        if (!IsKnownPlayer(actor)) return false;
        *vz = *(float *)((BYTE *)actor + OFS_PLAYER_VZ);
    } __except (CatchFl("read player vz", GetExceptionInformation())) {
        return false;
    }
    return true;
}

bool Game_ActorSetVz(void *actor, float vz)
{
    if (Fake()) {
        s_fake.vz = vz;
        return true;
    }
    __try {
        if (!IsKnownPlayer(actor)) return false;
        *(float *)((BYTE *)actor + OFS_PLAYER_VZ) = vz;
        *(float *)((BYTE *)actor + OFS_PLAYER_VZ_PREV) = vz;
    } __except (CatchFl("write player vz", GetExceptionInformation())) {
        return false;
    }
    return true;
}

// The same pair the game uses when it puts the player at an object (0x0088E5A0: setPos, setYaw, then
// StateCamera reset 0x005B7348 and 0x005D5B1A(cam, -1, -1)): the camera matrix goes behind the target along
// its yaw (vt[0x134] = player +0x140, written by setYaw), then the camera returns to its default state.
bool Game_CamResetBehindPlayer(void *cam)
{
    if (Fake()) {
        float y = s_fake.actorYaw * 0.017453293f, c = cosf(y), sn = sinf(y);
        float m[16] = {sn, -c, 0, 0, 0, 0, 1, 0, -c, -sn, 0, 0, s_fake.actorPos.x - 195 * c,
                       s_fake.actorPos.y - 195 * sn, s_fake.actorPos.z + 60, 1};
        memcpy(s_fake.cam.m, m, sizeof(m));
        s_fake.resets++;
        Log("test: camera reset behind the player (yaw %.1f) #%d", s_fake.actorYaw, s_fake.resets);
        return true;
    }
    static const BYTE kReset[] = {0x53, 0x8B, 0xDC, 0x51, 0x51, 0x83, 0xE4, 0xF0};
    static const BYTE kToDefault[] = {0x6A, 0x04, 0xB8, 0x0C, 0xB8, 0x00, 0x01};
    __try {
        if (*(UINT_PTR *)cam != PC_VT_STATECAM) return false;
        if (memcmp((void *)PC_CAM_RESET, kReset, sizeof(kReset)) != 0 ||
            memcmp((void *)PC_CAM_TO_DEFAULT, kToDefault, sizeof(kToDefault)) != 0)
            return false;
        ((CamReset_t)PC_CAM_RESET)(cam, NULL);
        ((CamToDefault_t)PC_CAM_TO_DEFAULT)(cam, -1.0f, -1.0f);
    } __except (CatchFl("camera reset (0x005B7348 / 0x005D5B1A)", GetExceptionInformation())) {
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

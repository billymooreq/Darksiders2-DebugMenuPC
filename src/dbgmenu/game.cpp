// Guarded reads of game objects (addresses: notes/xbox_pc_map.md, notes/review_03_menu_overlay.md).
// Every step is in its own __try; a vtable must lie in the exe image and a method in .text before we call it.
#include "common.h"

#define PC_GAME_PTR        0x0184E95Cu  // Darksiders* singleton (instance() 0x004058CF = lazy init + read)
#define PC_GET_PLAYER      0x004CDA8Fu  // Handle* __thiscall getPlayerActor(Darksiders*, Handle* out): game+8 -> vt[0x68]
#define PC_GET_CAMERA      0x00745A0Fu  // Handle* __thiscall getCamera(Darksiders*, Handle* out): game+0x138, addref
#define PC_RELEASE         0x00401017u  // void __thiscall release(obj): --obj+4, delete at 0
#define VT_ACTOR_GETPOS    0x38         // WorldObject::getPosition(Vec4* out), base 0x008EF6B2 (copies +0x90)
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

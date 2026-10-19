// Monsters (task 11, notes/monsters_pc.md): the Xbox AI commands showCharacterInfo / showCharacterDamageInfo /
// toggleMonsterUpdate rebuilt for the PC exe (there the commands and the DebugRenderer are compiled out, the game
// data they read is still there).
//   Xbox showCharacterInfo (ctor 0x825BE048, execute 0x825BE618): level 0..4 in the debug flags [0x837D73E0]+0x20,
//     every character's drawDebug (vt[0x78], PC vt[0x74] = empty stub) prints lines at its projected position:
//     1 [Move states]: the move state; 2 [Move & Script states]: + desc class, object name, AI behaviour (+0xABC);
//     3 [Move, Script & Cooldowns]: + the behaviour's cooldowns (map +0x120); 4 [Everything]: + target (+0x930) and
//     distance, "%d/%d HP"; and a screen list "Monsters: %d" / "  %s: %d" per class (level 4: "Characters: %d").
//     We always show the name and HP (task 11 asks for them) and add the lines of the chosen level.
//   Xbox showCharacterDamageInfo (0x825BE870): switches the game option DamageIndicators (options +0x2F) - the HUD
//     damage numbers (_root.addDamageIndicator, PC 0x005C64A2) and the hit direction (_root.UpdateHudDamage, PC
//     0x005C3D35). The PC exe still has both. We force the option on only while the menu item is on; the hooks of
//     the options commit 0x004DA98B / "changed" 0x004DA84D show the game the player's own value, so the profile
//     never saves our value.
//   Xbox toggleMonsterUpdate (0x825BE168): byte 0x83723A08 (1 = update). PC: byte 0x014D4278 = 1, read by the
//     three Monster update functions 0x007AEE47 (vt[0x21C]), 0x007B2DB2 (vt[0x68]), 0x007BA0DD (vt[0x220]) exactly
//     like on the Xbox. Freeze = 0, back = 1. Off by itself on a level load, the player's death, F10 off, unload.
// Characters: the actor list of every EventManager (evm+0x8, like the detectors at +0x14 in triggers.cpp), class by
// the reflection: obj->vt[4] = class descriptor (vt 0x0116228C, +8 parent, +0x10 Name), kind = Monster [0x0156F524]
// / NPC [0x0157B1CC] / Player [0x01562FE8] by walking the parents. Everything of the game is read in the game
// thread (the overlay runs in Present = the game thread), every object in its own __try.
#include "common.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <float.h>
#include "imgui.h"

bool g_monNearby, g_monTarget, g_monDamage, g_monFreeze;
bool g_monDamageNoHooks;
int g_monLevel;          // 0..3 = Xbox levels 1..4
int g_monRange = 2;      // kRangeM index (30 m)
int g_monWho;            // MW_*

// ---- the game (notes/monsters_pc.md, section 1) ----
#define PC_CLS_MONSTER_PTR 0x0156F524u   // Monster class descriptor (registered 0x007B5CC7, parent NPC)
#define PC_CLS_NPC_PTR     0x0157B1CCu   // NPC (0x00837B48, parent Character)
#define PC_CLS_PLAYER_PTR  0x01562FE8u   // Player (0x00723C00, parent Character; War / Death below it)
#define PC_VT_CLASS        0x0116228Cu   // class descriptor vtable (ctor 0x0048A35F)
#define PC_NAME_CSTR       0x00497245u   // const char *__thiscall Name::c_str(Name *) - "" for an unknown name
#define PC_OBJ_GETNAME     0x005A327Bu   // vt[0x2C] getName: lea eax, [ecx+0x50]
#define PC_MAX_HEALTH      0x0078EBD0u   // int __thiscall getMaxHealth(Character *) = scale 0x0078E5D7 * stats+0x8C
#define PC_UPDATE_FLAG     0x014D4278u   // byte: 1 = monsters update (toggleMonsterUpdate)
#define OFS_GAME_OPTIONS   0x134         // game+0x134 -> +8: the options object (0x004CDC36 -> 0x00902F25)
#define OFS_OPT_DAMAGEIND  0x2F          // DamageIndicators (reflection property; profile block +0x40)
#define OFS_GAME_LOADER    0xBC          // world loader: +0x10 == 4 -> world +0xC (triggers.cpp)
#define VT_GETCLASS        0x04
#define VT_GETNAME         0x2C
#define VT_GETPOS          0x38          // Character 0x00885362: copy of +0x260
#define OFS_CLS_PARENT     0x08
#define OFS_CLS_NAME       0x10
#define OFS_OBJ_NAME       0x50          // Name (8 bytes)
#define OFS_CHR_DESC       0x144         // 0x0076D72C (lazily made config); +0x190 proxy height (vt[0x25C])
#define OFS_DESC_HEIGHT    0x190
#define OFS_CHR_STATE      0x310         // current state (its class = the move state)
#define OFS_CHR_STATDESC   0x410         // the stat table 0x00782EF3 caches (getMaxHealth's scale reads it)
#define OFS_CHR_STATS      0x330         // stats: +0 HitPoints (float), +0x18 level, +0x30 damage, +0x8C max HP
#define OFS_ST_HP          0x00          //   base (* HealthScalar), +0x110 defense, +0x114 resistance (0x00786F18)
#define OFS_ST_LEVEL       0x18
#define OFS_ST_DAMAGE      0x30
#define OFS_ST_MAXBASE     0x8C
#define OFS_ST_DEFENSE     0x110
#define OFS_ST_RESIST      0x114
#define OFS_MON_TARGET     0x930         // Monster: the current target (0x007BA0DD)
#define OFS_MON_BEHAVIOR   0xABC         // Monster: the current AIBehavior (+0xAC0 the next, 0x007AF56B)
#define OFS_BEH_SUB        0xF8          // IBehavior (ctor 0x006B8538): +0xF8 the running part, its Name +0xB0
#define OFS_SUB_NAME       0xB0
#define OFS_BEH_COOLDOWNS  0x120         // std::map<Name, float>: +4 head node, +8 size (Xbox 0x828CAF88)
#define OFS_EVM_ACTORS     0x08          // {head, tail, count}, node {next, obj}

enum { MK_NONE = 0, MK_MONSTER, MK_NPC, MK_PLAYER };
static const float kRangeM[] = {10, 20, 30, 50, 75, 100, 150, 0};  // 0 = unlimited
static const float kUnitsPerM = 32.81f;  // 1 unit = 0.03048 m (Xbox notes: Havok scale) [hypothesis]
static const int kMaxItems = 2048, kMaxNodes = 65536, kMaxLabels = 48;
static const DWORD kStatsMs = 5000;

typedef void *(__fastcall *GetPtr_t)(void *self, void *edx);
typedef GameVec *(__fastcall *GetVec_t)(void *self, void *edx, GameVec *out);
typedef const char *(__fastcall *CStr_t)(const void *name, void *edx);
typedef int(__fastcall *GetInt_t)(void *self, void *edx);

struct MItem {
    void *obj, *cls;
    int kind;
    GameVec pos;
    float height, dist, aim;  // aim = angle to the view direction (deg), 999 = behind
    float sx, sy;             // anchor (over the head) on screen
    bool onScreen;
};
static MItem s_items[kMaxItems];
static int s_numItems;

struct MStats {
    int chars, monsters, npcs, players, evms, nodes, badNodes, broken, longList, overflow, inRange, drawn, overlap;
};
static MStats s_st;

static LARGE_INTEGER s_qpf;
static double s_collectMs, s_drawMs, s_collectMax, s_drawMax, s_lastCollect, s_lastDraw;
static int s_frames;
static DWORD s_statsTick;
static volatile LONG s_exBudget, s_statsBudget, s_hitBudget, s_targetBudget;
static bool s_wasOn, s_logOnEnable, s_testThrow;
static void *s_target;          // the object the target panel shows
static char s_targetName[64];
static void *s_clsMonster, *s_clsNpc, *s_clsPlayer;

static bool Fake() { return g_testMode && !g_gameMatch; }

// text from the game can be of any length: these cut instead of the CRT invalid-parameter path (that ends the game)
#define SPRINTF(buf, ...) _snprintf_s(buf, sizeof(buf), _TRUNCATE, __VA_ARGS__)
#define STRCPY(buf, src) strncpy_s(buf, sizeof(buf), src, _TRUNCATE)

static double NowMs()
{
    LARGE_INTEGER t;
    if (!s_qpf.QuadPart) QueryPerformanceFrequency(&s_qpf);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)s_qpf.QuadPart;
}

static int CatchMon(const char *where, EXCEPTION_POINTERS *ep)
{
    if (LogBudget(&s_exBudget, 40))
        Log("monsters: exception %08X at %08X in %s", ep->ExceptionRecord->ExceptionCode,
            (unsigned)(UINT_PTR)ep->ExceptionRecord->ExceptionAddress, where);
    return EXCEPTION_EXECUTE_HANDLER;
}

// ======================= offline test: fake world (DBGMENU_TEST=1, not the game) =======================
// Classes Actor > Character > NPC > Monster > Skeleton_Archer / Ghoul_Basic, NPC > Vulgrim, Character > Player >
// Death, Actor > ItemActor; the fake camera (game.cpp) is at (100 200 300) looking along +X, Z up.
enum { FC_ACTOR, FC_CHARACTER, FC_NPC, FC_MONSTER, FC_SKEL, FC_GHOUL, FC_VULGRIM, FC_PLAYER, FC_DEATH, FC_ITEM,
       FC_IDLE, FC_ATTACK, FC_BEHAVIOR, FC_COUNT };
static const char *const kFakeNames[] = {"Actor", "Character", "NPC", "Monster", "Skeleton_Archer", "Ghoul_Basic",
                                         "Vulgrim", "Player", "Death", "ItemActor", "MonsterIdle", "MonsterAttack",
                                         "AIBehavior", "Skel_A01", "Ghoul_07", "Ghoul_08", "Skel_Far", "Behind_01",
                                         "Vulgrim", "Death", "Coin_03", "Attack_Slash", "Slash", "Leap"};
enum { FN_OBJ = FC_COUNT, FN_SUB = FN_OBJ + 8, FN_SLASH, FN_LEAP };
static void *s_fvCls[0x40 / 4], *s_fvObj[0x300 / 4], *s_fvSmall[0x40 / 4];
static __declspec(align(16)) BYTE s_fCls[FC_COUNT][0x80];
static const int kFakeObjs = 9, kFakeBig = 300;
static __declspec(align(16)) BYTE s_fObj[kFakeObjs][0xD00];
static __declspec(align(16)) BYTE s_fStats[kFakeObjs][0x200], s_fDesc[kFakeObjs][0x200];
static __declspec(align(16)) BYTE s_fState[2][0x40], s_fBeh[0x140], s_fSub[0xC0], s_fMapNodes[3][0x20];
static struct FakeNode { FakeNode *next; void *obj; } s_fNodes[kFakeObjs + 2];
static BYTE s_fEvm[0x20];
static BYTE *s_fBigObj, *s_fBigStats;
static FakeNode *s_fBigNodes;
static int s_fakeMode;         // 0 normal, 23 = 300 monsters, 25 = the first one removed
static BYTE s_fakeFlag = 1;    // stands for 0x014D4278
static __declspec(align(16)) BYTE s_fakeOpt[0x60];
static BYTE s_fakeOptBlock;    // the profile value of DamageIndicators after a "commit"
static float s_fakePlayerHp = 100.0f;
static int s_fakeWorld = 1;
static bool s_fakeBuilt;
static float s_fO[3] = {100, 200, 300}, s_fF[2] = {1, 0};  // camera at cmd 17 (horizontal forward)
// forward, right, up (relative to the camera) of the fake objects
static const float kFakeLocal[kFakeObjs][3] = {{600, 0, -60}, {700, -150, -60}, {700, -162, -58}, {1400, 150, -60},
                                               {-600, 0, -60}, {500, 120, -60}, {300, 0, -80}, {550, 60, -60},
                                               {0, 0, 0}};
static void FakePlace(BYTE *o, float a, float b, float c)
{
    GameVec *p = (GameVec *)(o + 0x260);  // right = (f.y, -f.x): looking along +X, -Y is to the right
    p->x = s_fO[0] + s_fF[0] * a + s_fF[1] * b;
    p->y = s_fO[1] + s_fF[1] * a - s_fF[0] * b;
    p->z = s_fO[2] + c;
    p->w = 1;
}

static void *__fastcall FakeGetClass(void *self, void *) { return *(void **)((BYTE *)self + 0xCC0); }
static void *__fastcall FakeGetClassSmall(void *self, void *) { return *(void **)((BYTE *)self + 0x1C); }
static void *__fastcall FakeGetName(void *self, void *) { return (BYTE *)self + OFS_OBJ_NAME; }
static GameVec *__fastcall FakeGetPos(void *self, void *, GameVec *out)
{
    *out = *(GameVec *)((BYTE *)self + 0x260);
    return out;
}
static const char *__fastcall FakeCStr(const void *name, void *)
{
    DWORD a = ((const DWORD *)name)[0], b = ((const DWORD *)name)[1];
    if (b != 0x7E57 || a == 0 || a > sizeof(kFakeNames) / sizeof(kFakeNames[0])) return "";
    return kFakeNames[a - 1];
}
static int __fastcall FakeMaxHp(void *self, void *)
{
    BYTE *st = *(BYTE **)((BYTE *)self + OFS_CHR_STATS);
    return *(int *)(st + OFS_ST_MAXBASE);
}
static void FakeName(BYTE *p, int idx)
{
    ((DWORD *)p)[0] = idx + 1;
    ((DWORD *)p)[1] = 0x7E57;
}

static void FakeObj(int i, BYTE *o, BYTE *st, BYTE *desc, int cls, int name, float x, float y, float z, float h,
                    float hp, int maxHp, int lvl)
{
    memset(o, 0, 0xD00);
    *(void ***)o = s_fvObj;
    *(void **)(o + 0xCC0) = s_fCls[cls];
    if (name >= 0) FakeName(o + OFS_OBJ_NAME, name);
    FakePlace(o, x, y, z);
    if (st) {
        memset(st, 0, 0x200);
        *(float *)(st + OFS_ST_HP) = hp;
        *(int *)(st + OFS_ST_LEVEL) = lvl;
        *(int *)(st + OFS_ST_DAMAGE) = 10 + lvl * 2;
        *(int *)(st + OFS_ST_MAXBASE) = maxHp;
        *(int *)(st + OFS_ST_DEFENSE) = 5 + lvl;
        *(int *)(st + OFS_ST_RESIST) = lvl / 2;
        *(BYTE **)(o + OFS_CHR_STATS) = st;
    }
    if (desc) {
        memset(desc, 0, 0x200);
        *(float *)(desc + OFS_DESC_HEIGHT) = h;
        *(BYTE **)(o + OFS_CHR_DESC) = desc;
    }
    (void)i;
}

static void FakeBuild()
{
    if (!s_fakeBuilt) {
        s_fakeBuilt = true;
        for (int i = 0; i < 0x40 / 4; i++) s_fvCls[i] = s_fvSmall[i] = (void *)(UINT_PTR)(0x7D000000 + i);
        for (int i = 0; i < 0x300 / 4; i++) s_fvObj[i] = (void *)(UINT_PTR)(0x7C000000 + i);
        s_fvObj[VT_GETCLASS / 4] = (void *)FakeGetClass;
        s_fvObj[VT_GETNAME / 4] = (void *)FakeGetName;
        s_fvObj[VT_GETPOS / 4] = (void *)FakeGetPos;
        s_fvSmall[VT_GETCLASS / 4] = (void *)FakeGetClassSmall;
        static const int kParent[FC_COUNT] = {-1, FC_ACTOR, FC_CHARACTER, FC_NPC, FC_MONSTER, FC_MONSTER, FC_NPC,
                                              FC_CHARACTER, FC_PLAYER, FC_ACTOR, -1, -1, -1};
        for (int c = 0; c < FC_COUNT; c++) {
            memset(s_fCls[c], 0, 0x80);
            *(void ***)s_fCls[c] = s_fvCls;
            *(void **)(s_fCls[c] + OFS_CLS_PARENT) = kParent[c] >= 0 ? s_fCls[kParent[c]] : NULL;
            FakeName(s_fCls[c] + OFS_CLS_NAME, c);
        }
        for (int s = 0; s < 2; s++) {
            memset(s_fState[s], 0, 0x40);
            *(void ***)s_fState[s] = s_fvSmall;
            *(void **)(s_fState[s] + 0x1C) = s_fCls[s ? FC_ATTACK : FC_IDLE];
        }
        // AIBehavior with the running part "Attack_Slash" and two cooldowns (Slash 1.5, Leap 0.0)
        memset(s_fBeh, 0, sizeof(s_fBeh));
        *(void ***)s_fBeh = s_fvSmall;
        *(void **)(s_fBeh + 0x1C) = s_fCls[FC_BEHAVIOR];
        memset(s_fSub, 0, sizeof(s_fSub));
        FakeName(s_fSub + OFS_SUB_NAME, FN_SUB);
        *(void **)(s_fBeh + OFS_BEH_SUB) = s_fSub;
        BYTE *head = s_fMapNodes[0], *slash = s_fMapNodes[1], *leap = s_fMapNodes[2];
        memset(s_fMapNodes, 0, sizeof(s_fMapNodes));
        *(BYTE **)(head + 0) = slash;  // leftmost
        *(BYTE **)(head + 4) = slash;  // root
        *(BYTE **)(head + 8) = leap;   // rightmost
        head[0x1D] = 1;                // isnil
        *(BYTE **)(slash + 0) = head;
        *(BYTE **)(slash + 4) = head;
        *(BYTE **)(slash + 8) = leap;
        FakeName(slash + 0x10, FN_SLASH);
        *(float *)(slash + 0x18) = 1.5f;
        *(BYTE **)(leap + 0) = head;
        *(BYTE **)(leap + 4) = slash;
        *(BYTE **)(leap + 8) = head;
        FakeName(leap + 0x10, FN_LEAP);
        *(float *)(leap + 0x18) = 0.0f;
        *(BYTE **)(s_fBeh + OFS_BEH_COOLDOWNS + 4) = head;
        *(int *)(s_fBeh + OFS_BEH_COOLDOWNS + 8) = 2;
        memset(s_fakeOpt, 0, sizeof(s_fakeOpt));
        *(void ***)s_fakeOpt = s_fvSmall;
        int n = 0;
        //       obj  class      name        x     y    z    height hp   max  lvl
        static const int kCls[kFakeObjs] = {FC_SKEL, FC_GHOUL, FC_GHOUL, FC_SKEL, FC_SKEL, FC_VULGRIM, FC_DEATH, FC_ITEM,
                                            FC_GHOUL};
        static const float kH[kFakeObjs] = {70, 60, 60, 70, 70, 0, 75, 0, 0}, kHp[kFakeObjs] = {180, 90, 40, 250, 250,
                                                                                               500, 100, 0, 0};
        static const int kMax[kFakeObjs] = {250, 90, 90, 250, 250, 500, 100, 0, 0}, kLvl[kFakeObjs] = {12, 10, 10, 12, 12,
                                                                                                    30, 8, 0, 0};
        for (; n < kFakeObjs; n++) {  // the last one is replaced by a bad pointer in the list; Coin_03 has no stats
            bool chr = n < 7;
            FakeObj(n, s_fObj[n], chr ? s_fStats[n] : NULL, chr ? s_fDesc[n] : NULL, kCls[n], n < 8 ? FN_OBJ + n : -1,
                    kFakeLocal[n][0], kFakeLocal[n][1], kFakeLocal[n][2], kH[n], kHp[n], kMax[n], kLvl[n]);
        }
        *(BYTE **)(s_fObj[0] + OFS_CHR_STATE) = s_fState[1];
        *(BYTE **)(s_fObj[1] + OFS_CHR_STATE) = s_fState[0];
        *(BYTE **)(s_fObj[0] + OFS_MON_BEHAVIOR) = s_fBeh;
        *(BYTE **)(s_fObj[0] + OFS_MON_TARGET) = s_fObj[6];
    }
    if (s_fakeMode == 23) {
        if (!s_fBigObj) {
            s_fBigObj = (BYTE *)VirtualAlloc(NULL, kFakeBig * 0xD00, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            s_fBigStats = (BYTE *)VirtualAlloc(NULL, kFakeBig * 0x200, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            s_fBigNodes = (FakeNode *)VirtualAlloc(NULL, kFakeBig * sizeof(FakeNode), MEM_COMMIT | MEM_RESERVE,
                                                   PAGE_READWRITE);
            if (s_fBigObj && s_fBigStats && s_fBigNodes)
                for (int i = 0; i < kFakeBig; i++)  // a 15 x 20 grid ahead of the camera
                    FakeObj(i, s_fBigObj + i * 0xD00, s_fBigStats + i * 0x200, s_fDesc[0], (i % 3) ? FC_GHOUL : FC_SKEL,
                            FN_OBJ + (i % 3 ? 1 : 0), 300.0f + (i % 15) * 120.0f, (i / 15 - 10) * 90.0f, -60, 70,
                            (float)(20 + i % 70), 90, 10);
        }
        if (s_fBigObj && s_fBigStats && s_fBigNodes) {
            for (int i = 0; i < kFakeBig; i++) {
                FakePlace(s_fBigObj + i * 0xD00, 300.0f + (i % 15) * 120.0f, (i / 15 - 10) * 90.0f, -60);
                s_fBigNodes[i].obj = s_fBigObj + i * 0xD00;
                s_fBigNodes[i].next = i + 1 < kFakeBig ? &s_fBigNodes[i + 1] : NULL;
            }
            memset(s_fEvm, 0, sizeof(s_fEvm));
            *(void **)(s_fEvm + OFS_EVM_ACTORS) = &s_fBigNodes[0];
            *(int *)(s_fEvm + OFS_EVM_ACTORS + 8) = kFakeBig;
            return;
        }
    }
    for (int i = 0; i < kFakeObjs - 1; i++) FakePlace(s_fObj[i], kFakeLocal[i][0], kFakeLocal[i][1], kFakeLocal[i][2]);
    int k = 0;
    for (int i = s_fakeMode == 25 ? 1 : 0; i < kFakeObjs; i++) {
        s_fNodes[k].obj = i == kFakeObjs - 1 ? (void *)(UINT_PTR)0x10 : (void *)s_fObj[i];  // the last: bad object
        s_fNodes[k].next = NULL;
        if (k) s_fNodes[k - 1].next = &s_fNodes[k];
        k++;
    }
    memset(s_fEvm, 0, sizeof(s_fEvm));
    *(void **)(s_fEvm + OFS_EVM_ACTORS) = &s_fNodes[0];
    *(void **)(s_fEvm + OFS_EVM_ACTORS + 4) = &s_fNodes[k - 1];
    *(int *)(s_fEvm + OFS_EVM_ACTORS + 8) = k;
}

// ======================= access helpers (real or fake) =======================
static bool VtOk(void *vt)
{
    if (Fake()) return vt == s_fvObj || vt == s_fvSmall || vt == s_fvCls;
    return InImage(vt);
}
static bool FnOk(void *fn)
{
    if (Fake()) return fn == (void *)FakeGetClass || fn == (void *)FakeGetClassSmall || fn == (void *)FakeGetName ||
                       fn == (void *)FakeGetPos;
    return InText(fn);
}
// getPos (vt[0x38]) we call: Character 0x00885362 (+0x260, also Monster / player) and WorldObject 0x008EF6B2 (+0x90)
static volatile LONG s_oddPosBudget;
static bool PosFnOk(void *fn)
{
    if (Fake()) return fn == (void *)FakeGetPos;
    if (fn == (void *)(UINT_PTR)0x00885362u || fn == (void *)(UINT_PTR)0x008EF6B2u) return true;
    if (LogBudget(&s_oddPosBudget, 20)) Log("monsters: getPos %08X is not a known one -> skipped", (unsigned)(UINT_PTR)fn);
    return false;
}
static void *ClassVt() { return Fake() ? (void *)s_fvCls : (void *)(UINT_PTR)PC_VT_CLASS; }
static CStr_t CStrFn() { return Fake() ? FakeCStr : (CStr_t)(UINT_PTR)PC_NAME_CSTR; }
static BYTE *UpdateFlag() { return Fake() ? &s_fakeFlag : (BYTE *)(UINT_PTR)PC_UPDATE_FLAG; }

// vt[4] getClass is only called when its code is a plain read: "mov eax, [abs]; ret" (native classes, e.g. the
// detectors 0x008B7C0B, PlayerFall 0x006D6A25) or "mov eax, [ecx + disp]; ret" (an instance class: Monster +0xB90
// 0x007B3F90, the player +0xCC0, AIBehavior +0x130). Anything else is skipped (logged once per function).
static volatile LONG s_oddGetClassBudget;
static bool PlainGetter(void *fn)
{
    if (Fake()) return true;
    static void *s_ok[256], *s_bad[64];
    static int s_nOk, s_nBad;
    for (int i = 0; i < s_nOk; i++)
        if (s_ok[i] == fn) return true;
    for (int i = 0; i < s_nBad; i++)
        if (s_bad[i] == fn) return false;
    const BYTE *b = (const BYTE *)fn;
    bool ok = (b[0] == 0xA1 && b[5] == 0xC3) || (b[0] == 0x8B && b[1] == 0x81 && b[6] == 0xC3) ||
              (b[0] == 0x8B && b[1] == 0x41 && b[3] == 0xC3);
    if (ok) {
        if (s_nOk < 256) s_ok[s_nOk++] = fn;
    } else {
        if (s_nBad < 64) s_bad[s_nBad++] = fn;
        if (LogBudget(&s_oddGetClassBudget, 20))
            Log("monsters: vt[4] %08X is not a plain getter (%02X %02X %02X %02X) -> such objects are skipped",
                (unsigned)(UINT_PTR)fn, b[0], b[1], b[2], b[3]);
    }
    return ok;
}

// object -> its class descriptor (NULL = not a reflected object)
static void *ClassOf(void *obj)
{
    void **vt = *(void ***)obj;
    if (!VtOk(vt)) return NULL;
    void *fn = vt[VT_GETCLASS / 4];
    if (!FnOk(fn) || !PlainGetter(fn)) return NULL;
    void *cls = ((GetPtr_t)fn)(obj, NULL);
    if (!cls || *(void **)cls != ClassVt()) return NULL;
    return cls;
}

// ---- Name -> text cache (Name = 8 bytes; the game's table lookup takes a lock, so we ask once per name) ----
struct NameEnt { DWORD a, b; bool used; char s[48]; };
static NameEnt s_names[2048];
static const char *NameText(const void *p)
{
    DWORD a, b;
    __try {
        a = ((const DWORD *)p)[0];
        b = ((const DWORD *)p)[1];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "";
    }
    if (!a && !b) return "";
    unsigned h = (a ^ (b * 0x9E3779B1u)) & 2047;
    for (int i = 0; i < 8; i++) {
        NameEnt &e = s_names[(h + i) & 2047];
        if (e.used && e.a == a && e.b == b) return e.s;
        if (!e.used) break;
    }
    const char *s = NULL;
    DWORD tmp[2] = {a, b};
    __try {
        s = CStrFn()(tmp, NULL);
    } __except (CatchMon("Name::c_str 0x00497245", GetExceptionInformation())) {
        s = NULL;
    }
    NameEnt *slot = &s_names[h];
    for (int i = 0; i < 8; i++)
        if (!s_names[(h + i) & 2047].used) {
            slot = &s_names[(h + i) & 2047];
            break;
        }
    slot->a = a, slot->b = b, slot->used = true;
    slot->s[0] = 0;
    __try {
        if (s) {
            int n = 0;
            while (s[n] && n < (int)sizeof(slot->s) - 1) {
                char c = s[n];
                slot->s[n++] = (c >= 32 && c < 127) ? c : '?';
            }
            slot->s[n] = 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        slot->s[0] = 0;
    }
    return slot->s;
}
static const char *ClassName(void *cls) { return cls ? NameText((BYTE *)cls + OFS_CLS_NAME) : ""; }
static const char *ObjName(void *obj)
{
    __try {
        void *fn = (*(void ***)obj)[VT_GETNAME / 4];
        if (fn != (Fake() ? (void *)FakeGetName : (void *)(UINT_PTR)PC_OBJ_GETNAME)) return "";
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "";
    }
    return NameText((BYTE *)obj + OFS_OBJ_NAME);
}

// ---- class -> kind cache ----
struct ClsEnt { void *cls; int kind; };
static ClsEnt s_cls[1024];
static int KindOf(void *cls)
{
    unsigned h = ((unsigned)(UINT_PTR)cls >> 4) & 1023;
    for (int i = 0; i < 16; i++) {
        ClsEnt &e = s_cls[(h + i) & 1023];
        if (e.cls == cls) return e.kind;
        if (!e.cls) break;
    }
    int kind = MK_NONE;
    __try {
        void *c = cls;
        for (int d = 0; c && d < 40; d++) {
            if (c == s_clsMonster) { kind = MK_MONSTER; break; }
            if (c == s_clsPlayer) { kind = MK_PLAYER; break; }
            if (c == s_clsNpc) { kind = MK_NPC; break; }  // Monster is below NPC: checked first on the way up
            c = *(void **)((BYTE *)c + OFS_CLS_PARENT);
        }
    } __except (CatchMon("class parents", GetExceptionInformation())) {
        kind = MK_NONE;
    }
    for (int i = 0; i < 16; i++) {
        ClsEnt &e = s_cls[(h + i) & 1023];
        if (!e.cls) {
            e.cls = cls;
            e.kind = kind;
            break;
        }
    }
    return kind;
}

static bool ReadBaseClasses()
{
    if (Fake()) {
        FakeBuild();
        s_clsMonster = s_fCls[FC_MONSTER];
        s_clsNpc = s_fCls[FC_NPC];
        s_clsPlayer = s_fCls[FC_PLAYER];
        return true;
    }
    __try {
        s_clsMonster = *(void **)PC_CLS_MONSTER_PTR;
        s_clsNpc = *(void **)PC_CLS_NPC_PTR;
        s_clsPlayer = *(void **)PC_CLS_PLAYER_PTR;
    } __except (CatchMon("class globals", GetExceptionInformation())) {
        return false;
    }
    return s_clsMonster && s_clsNpc && s_clsPlayer;
}

// ======================= view (the frame's matrices from triggers.cpp) =======================
static float s_vp[16], s_cam[16], s_proj[16], s_W, s_H;
static bool s_viewOk;

static bool Project(const float *p, float *sx, float *sy)
{
    const float *m = s_vp;
    float c[4];
    for (int j = 0; j < 4; j++) c[j] = p[0] * m[0 * 4 + j] + p[1] * m[1 * 4 + j] + p[2] * m[2 * 4 + j] + m[3 * 4 + j];
    if (c[2] < 0 || c[3] < 1.0f) return false;  // behind the camera / at the near plane
    float iw = 1.0f / c[3];
    *sx = (c[0] * iw * 0.5f + 0.5f) * s_W;
    *sy = (0.5f - c[1] * iw * 0.5f) * s_H;
    return _finite(*sx) && _finite(*sy) && fabsf(*sx) < 1e5f && fabsf(*sy) < 1e5f;
}

// ======================= collect =======================
static float DescHeight(void *obj)
{
    __try {
        BYTE *desc = *(BYTE **)((BYTE *)obj + OFS_CHR_DESC);  // raw: never create it ourselves
        if (!desc) return 0;
        float h = *(float *)(desc + OFS_DESC_HEIGHT);
        return (_finite(h) && h > 5.0f && h < 5000.0f) ? h : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// 1 = added, 0 = skipped, -1 = exception in the object, -2 = the node is unreadable
static int ReadNode(BYTE *node, BYTE **next)
{
    void *obj;
    __try {
        *next = *(BYTE **)node;
        obj = *(void **)(node + 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *next = NULL;
        return -2;
    }
    if ((UINT_PTR)obj < 0x10000) return obj ? -1 : 0;  // not an object (counted as a bad node, no exception)
    __try {
        void *cls = ClassOf(obj);
        if (!cls) return 0;
        int kind = KindOf(cls);
        if (kind == MK_NONE) return 0;
        s_st.chars++;
        if (kind == MK_MONSTER) s_st.monsters++;
        else if (kind == MK_NPC) s_st.npcs++;
        else s_st.players++;
        if (!(kind == MK_MONSTER || (kind == MK_NPC && g_monWho >= 1) || (kind == MK_PLAYER && g_monWho >= 2))) return 0;
        if (s_numItems >= kMaxItems) {
            s_st.overflow++;
            return 0;
        }
        void *fn = (*(void ***)obj)[VT_GETPOS / 4];
        if (!PosFnOk(fn)) return 0;
        MItem &it = s_items[s_numItems];
        memset(&it, 0, sizeof(it));
        ((GetVec_t)fn)(obj, NULL, &it.pos);
        if (!_finite(it.pos.x) || !_finite(it.pos.y) || !_finite(it.pos.z)) return 0;
        if (it.pos.x == 0 && it.pos.y == 0 && it.pos.z == 0) return 0;  // not placed yet (loading)
        it.obj = obj;
        it.cls = cls;
        it.kind = kind;
        it.height = DescHeight(obj);
        s_numItems++;
        return 1;
    } __except (CatchMon("character node", GetExceptionInformation())) {
        return -1;
    }
}

static void WalkActors(BYTE *evm)
{
    BYTE *head;
    int count;
    __try {
        head = *(BYTE **)(evm + OFS_EVM_ACTORS);
        count = *(int *)(evm + OFS_EVM_ACTORS + 8);
    } __except (CatchMon("EventManager actor list", GetExceptionInformation())) {
        s_st.broken++;
        return;
    }
    int limit = (count >= 0 && count < kMaxNodes - 64) ? count + 64 : kMaxNodes;
    int n = 0;
    BYTE *node = head;
    while (node && n < limit) {
        BYTE *next = NULL;
        int r = ReadNode(node, &next);
        n++;
        if (r == -1) s_st.badNodes++;
        else if (r == -2) {
            s_st.broken++;
            break;
        }
        node = next;
    }
    if (node && n >= limit) s_st.longList++;
    s_st.nodes += n;
}

static void CollectInner()
{
    memset(&s_st, 0, sizeof(s_st));
    s_numItems = 0;
    if (s_testThrow) {
        s_testThrow = false;
        *(volatile int *)0 = 1;
    }
    void *evms[4];
    int ne;
    if (Fake()) {
        FakeBuild();
        evms[0] = s_fEvm;
        ne = 1;
    } else {
        ne = Trig_EventManagers(evms, 4);
    }
    s_st.evms = ne;
    for (int i = 0; i < ne; i++) WalkActors((BYTE *)evms[i]);
}

// ======================= what one character shows =======================
struct MInfo {
    char name[64], cls[48], obj[48], state[48], beh[48], sub[48], cool[160], target[64];
    bool stats, maxOk;
    float hp, targetDist;
    int maxHp, level, damage, defense, resist, coolN;
};

static void ReadCooldowns(BYTE *beh, MInfo &mi)
{
    // std::map<Name, float> (Xbox 0x828CAF88 prints it as "name: %.1f"): head node +4, size +8; a node {left,
    // parent, right, key Name +0x10, value float +0x18}; the head is the nil node of every leaf -> walk from the
    // root (head->parent) and stop at the head
    BYTE *head = *(BYTE **)(beh + OFS_BEH_COOLDOWNS + 4);
    int size = *(int *)(beh + OFS_BEH_COOLDOWNS + 8);
    if (!head || size <= 0 || size > 64) return;
    BYTE *stack[24];
    int sp = 0, visited = 0;
    BYTE *root = *(BYTE **)(head + 4);
    if (root && root != head) stack[sp++] = root;
    int o = 0;
    while (sp && visited < 64) {
        BYTE *n = stack[--sp];
        visited++;
        const char *k = NameText(n + 0x10);
        float v = *(float *)(n + 0x18);
        if (k[0] && _finite(v) && o < (int)sizeof(mi.cool) - 4) {
            int w = _snprintf_s(mi.cool + o, sizeof(mi.cool) - o, _TRUNCATE, "%s%s %.1f", o ? "  " : "", k, v);
            mi.coolN++;
            if (w < 0) break;  // full
            o += w;
        }
        BYTE *l = *(BYTE **)(n + 0), *r = *(BYTE **)(n + 8);
        if (r && r != head && sp < 24) stack[sp++] = r;
        if (l && l != head && sp < 24) stack[sp++] = l;
    }
}

static void ReadInfo(const MItem &it, MInfo &mi)
{
    memset(&mi, 0, sizeof(mi));
    STRCPY(mi.cls, ClassName(it.cls));
    STRCPY(mi.obj, ObjName(it.obj));
    if (mi.obj[0] && _stricmp(mi.obj, mi.cls) != 0) SPRINTF(mi.name, "%s", mi.obj);
    else SPRINTF(mi.name, "%s", mi.cls[0] ? mi.cls : "?");
    BYTE *o = (BYTE *)it.obj;
    __try {
        BYTE *st = *(BYTE **)(o + OFS_CHR_STATS);
        if (st) {
            mi.hp = *(float *)(st + OFS_ST_HP);
            mi.level = *(int *)(st + OFS_ST_LEVEL);
            mi.damage = *(int *)(st + OFS_ST_DAMAGE);
            mi.defense = *(int *)(st + OFS_ST_DEFENSE);
            mi.resist = *(int *)(st + OFS_ST_RESIST);
            mi.maxHp = *(int *)(st + OFS_ST_MAXBASE);
            mi.stats = _finite(mi.hp) != 0;
        }
    } __except (CatchMon("character stats +0x330", GetExceptionInformation())) {
        mi.stats = false;
    }
    if (mi.stats) {
        __try {
            static const BYTE kPro[] = {0x56, 0x8B, 0xF1, 0xE8};
            if (Fake()) mi.maxHp = FakeMaxHp(it.obj, NULL), mi.maxOk = true;
            // only when the character already has its stat table (0x00782EF3 caches it at +0x410): then the call
            // just reads (difficulty scale * base), it never creates anything
            else if (*(void **)(o + OFS_CHR_STATDESC) && memcmp((void *)PC_MAX_HEALTH, kPro, sizeof(kPro)) == 0) {
                mi.maxHp = ((GetInt_t)(UINT_PTR)PC_MAX_HEALTH)(it.obj, NULL);  // base * difficulty scale
                mi.maxOk = true;
            }
        } __except (CatchMon("getMaxHealth 0x0078EBD0", GetExceptionInformation())) {
            mi.maxOk = false;
        }
    }
    __try {
        void *state = *(void **)(o + OFS_CHR_STATE);
        if (state) STRCPY(mi.state, ClassName(ClassOf(state)));
    } __except (CatchMon("move state +0x310", GetExceptionInformation())) {
    }
    if (it.kind != MK_MONSTER) return;
    __try {
        BYTE *beh = *(BYTE **)(o + OFS_MON_BEHAVIOR);
        if (beh && VtOk(*(void **)beh)) {
            STRCPY(mi.beh, ClassName(ClassOf(beh)));
            BYTE *sub = *(BYTE **)(beh + OFS_BEH_SUB);
            if (sub) STRCPY(mi.sub, NameText(sub + OFS_SUB_NAME));
            ReadCooldowns(beh, mi);
        }
    } __except (CatchMon("AI behaviour +0xABC", GetExceptionInformation())) {
    }
    __try {
        void *t = *(void **)(o + OFS_MON_TARGET);
        if (t && VtOk(*(void **)t)) {
            void *tc = ClassOf(t);
            const char *tn = ObjName(t);
            SPRINTF(mi.target, "%s", tn[0] ? tn : ClassName(tc));
            void *fn = (*(void ***)t)[VT_GETPOS / 4];
            if (PosFnOk(fn)) {
                GameVec tp;
                ((GetVec_t)fn)(t, NULL, &tp);
                float dx = tp.x - it.pos.x, dy = tp.y - it.pos.y;  // 2D like the Xbox line
                mi.targetDist = sqrtf(dx * dx + dy * dy);
            }
        }
    } __except (CatchMon("target +0x930", GetExceptionInformation())) {
    }
}

// ======================= damage tracking (damage info on) =======================
struct DmgEnt { void *obj; float lastHp, last, total; int hits; DWORD at, seen; };
static DmgEnt s_dmg[256];
static DmgEnt *DmgOf(void *obj, bool create)
{
    unsigned h = ((unsigned)(UINT_PTR)obj >> 4) & 255;
    DmgEnt *freeE = NULL;
    for (int i = 0; i < 16; i++) {
        DmgEnt &e = s_dmg[(h + i) & 255];
        if (e.obj == obj) return &e;
        if (!freeE && (!e.obj || GetTickCount() - e.seen > 10000)) freeE = &e;
    }
    if (!create || !freeE) return NULL;
    memset(freeE, 0, sizeof(*freeE));
    freeE->obj = obj;
    freeE->lastHp = -1;
    return freeE;
}
static void DmgUpdate(const MItem &it, const MInfo &mi)
{
    if (!g_monDamage || !mi.stats) return;
    DmgEnt *e = DmgOf(it.obj, true);
    if (!e) return;
    e->seen = GetTickCount();
    if (e->lastHp >= 0 && mi.hp < e->lastHp - 0.01f) {
        float d = e->lastHp - mi.hp;
        e->last = d;
        e->total += d;
        e->hits++;
        e->at = GetTickCount();
        if (LogBudget(&s_hitBudget, 300))
            Log("monsters: hit %s -%.1f hp -> %.1f/%d (hits %d, total -%.1f)", mi.name, d, mi.hp, mi.maxHp, e->hits,
                e->total);
    }
    e->lastHp = mi.hp;
}

// ======================= flags: freeze (0x014D4278) and DamageIndicators (options +0x2F) =======================
static bool s_freezeOn;         // the flag is 0 because of us
static void *s_freezeWorld;
static bool s_freezeChecked, s_freezeOk;
static bool s_dmgOn;            // options +0x2F forced by us
static BYTE *s_opt;             // the options object we wrote
static BYTE s_optOrig;          // the player's own value
static LONG s_optHidden;        // commit / changed calls that saw the player's value

static bool FreezeCodeOk()
{
    if (Fake()) return true;
    if (s_freezeChecked) return s_freezeOk;
    s_freezeChecked = true;
    static const UINT_PTR kSites[3] = {0x007AEE4Bu, 0x007B2DB5u, 0x007BA11Fu};
    static const BYTE kCmp[7] = {0x80, 0x3D, 0x78, 0x42, 0x4D, 0x01, 0x00};  // cmp byte ptr [0x014D4278], 0
    s_freezeOk = true;
    __try {
        for (int i = 0; i < 3; i++)
            if (memcmp((void *)kSites[i], kCmp, sizeof(kCmp)) != 0) {
                Log("monsters: freeze: unexpected bytes at %08X -> freeze not available", (unsigned)kSites[i]);
                s_freezeOk = false;
            }
        BYTE v = *(BYTE *)PC_UPDATE_FLAG;
        if (v != 0 && v != 1) {
            Log("monsters: freeze: flag 0x014D4278 = %u (expected 1) -> freeze not available", v);
            s_freezeOk = false;
        }
    } __except (CatchMon("freeze check", GetExceptionInformation())) {
        s_freezeOk = false;
    }
    return s_freezeOk;
}

static void *CurrentWorld()
{
    if (Fake()) return (void *)(UINT_PTR)s_fakeWorld;
    void *game = NULL;
    if (!Game_GetGame(&game) || !game) return NULL;
    __try {
        BYTE *ld = *(BYTE **)((BYTE *)game + OFS_GAME_LOADER);
        if (ld && *(int *)(ld + 0x10) == 4) return *(void **)(ld + 0xC);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return NULL;
}

// player HP (death = HP <= 0); false = no player now
static bool PlayerHp(float *hp)
{
    if (Fake()) {
        *hp = s_fakePlayerHp;
        return true;
    }
    void *game = NULL, *actor = NULL;
    if (!Game_GetGame(&game) || !game || !Game_GetActor(game, &actor) || !actor) return false;
    __try {
        if (!InImage(*(void **)actor)) return false;
        BYTE *st = *(BYTE **)((BYTE *)actor + OFS_CHR_STATS);
        if (!st) return false;
        *hp = *(float *)(st + OFS_ST_HP);
        return _finite(*hp) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void FreezeSet(bool on, const char *why)
{
    __try {
        BYTE *f = UpdateFlag();
        BYTE before = *f;
        if (on) {
            *f = 0;
            s_freezeOn = true;
            s_freezeWorld = CurrentWorld();
        } else {
            *f = 1;
            s_freezeOn = false;
        }
        Log("monsters: freeze %s (%s): monster update flag %08X %u -> %u", on ? "ON" : "OFF", why,
            (unsigned)(UINT_PTR)f, before, *f);
    } __except (CatchMon("monster update flag", GetExceptionInformation())) {
        s_freezeOn = false;
        g_monFreeze = false;
    }
}

static BYTE *OptionsObj()
{
    if (Fake()) return s_fakeOpt;
    void *game = NULL;
    if (!Game_GetGame(&game) || !game) return NULL;
    __try {
        BYTE *h = *(BYTE **)((BYTE *)game + OFS_GAME_OPTIONS);
        BYTE *opt = h ? *(BYTE **)(h + 8) : NULL;
        if (opt && InImage(*(void **)opt)) return opt;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return NULL;
}

static void DamageSet(bool on, const char *why)
{
    __try {
        if (on) {
            if (g_monDamageNoHooks && !Fake()) {  // without them the profile could keep our value
                g_monDamage = false;
                Log("monsters: damage info refused: the options hooks are not installed");
                Menu_Status("damage info: not available (see the log)");
                return;
            }
            BYTE *opt = OptionsObj();
            if (!opt) return;  // tried again next frame
            if (s_dmgOn && opt == s_opt) {
                if (opt[OFS_OPT_DAMAGEIND] != 1) opt[OFS_OPT_DAMAGEIND] = 1;  // the game loaded its value again
                return;
            }
            if (s_dmgOn && s_opt != opt) Log("monsters: damage info: the options object changed (%08X -> %08X)",
                                             (unsigned)(UINT_PTR)s_opt, (unsigned)(UINT_PTR)opt);
            s_opt = opt;
            s_optOrig = opt[OFS_OPT_DAMAGEIND];
            opt[OFS_OPT_DAMAGEIND] = 1;
            s_dmgOn = true;
            Log("monsters: damage info ON (%s): options %08X vt %08X DamageIndicators %u -> 1 (the profile keeps %u)",
                why, (unsigned)(UINT_PTR)opt, (unsigned)(UINT_PTR) * (void **)opt, s_optOrig, s_optOrig);
        } else if (s_dmgOn) {
            BYTE *opt = OptionsObj();
            if (opt == s_opt) opt[OFS_OPT_DAMAGEIND] = s_optOrig;
            Log("monsters: damage info OFF (%s): DamageIndicators back to %u%s; the game saved / compared the "
                "options %ld times meanwhile (it saw %u)", why, s_optOrig, opt == s_opt ? "" : " (object gone)",
                s_optHidden, s_optOrig);
            s_dmgOn = false;
            s_opt = NULL;
            s_optHidden = 0;
        }
    } __except (CatchMon("options DamageIndicators", GetExceptionInformation())) {
        s_dmgOn = false;
        g_monDamage = false;
    }
}

// hooks of the options commit 0x004DA98B / changed 0x004DA84D (hooks.cpp): while we force the option, every options
// object the game saves or compares (also a copy the options screen may make of ours) shows the player's own value
int Mon_OptionsHide(void *opt)
{
    if (!s_dmgOn || !opt) return -1;
    int prev;
    __try {
        prev = ((BYTE *)opt)[OFS_OPT_DAMAGEIND];
        ((BYTE *)opt)[OFS_OPT_DAMAGEIND] = s_optOrig;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    InterlockedIncrement(&s_optHidden);
    return prev;
}
void Mon_OptionsRestore(void *opt, int prev)
{
    __try {
        ((BYTE *)opt)[OFS_OPT_DAMAGEIND] = (BYTE)prev;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static void Status(const char *s) { Menu_Status("%s", s); }

// game thread, before every game frame (and on F10 off / unload): the flags follow the menu items
static void ApplyFlags(const char *why)
{
    if (g_monFreeze && !s_freezeOn) {
        if (!FreezeCodeOk()) {
            g_monFreeze = false;
            Status("freeze monsters: not available (see the log)");
        } else {
            FreezeSet(true, why ? why : "menu");
        }
    } else if (!g_monFreeze && s_freezeOn) {
        FreezeSet(false, why ? why : "menu");
    }
    if (g_monDamage) DamageSet(true, why ? why : "menu");
    else if (s_dmgOn) DamageSet(false, why ? why : "menu");
}

void Mon_Frame()
{
    if (!g_gameMatch && !g_testMode) return;
    ApplyFlags(NULL);
    if (s_freezeOn) {  // off by itself: level load, the player's death
        void *w = CurrentWorld();
        float hp;
        const char *why = NULL;
        if (w != s_freezeWorld) why = "level load";
        else if (PlayerHp(&hp) && hp <= 0) why = "the player died";
        if (why) {
            g_monFreeze = false;
            FreezeSet(false, why);
            char msg[96];
            SPRINTF(msg, "freeze monsters: off (%s)", why);
            Status(msg);
        }
    }
}

void Mon_OnDebugOff()
{
    bool any = g_monNearby || g_monTarget || g_monDamage || g_monFreeze;
    g_monNearby = g_monTarget = g_monDamage = g_monFreeze = false;
    if (any) ApplyFlags("debug off");
}

void Mon_OnUnload()
{
    g_monFreeze = g_monDamage = false;
    if (s_freezeOn || s_dmgOn) ApplyFlags("unload");
}

void Mon_ToggleTarget()
{
    g_monTarget = !g_monTarget;
    Menu_Status("info: target %s", g_monTarget ? "on" : "off");
}

// ======================= draw (overlay = game thread) =======================
static const ImU32 kWhite = IM_COL32(255, 255, 255, 255);   // Xbox prints everything white (DebugRenderer 1,1,1,1)
static const ImU32 kDim = IM_COL32(185, 190, 200, 235);
static const ImU32 kHit = IM_COL32(255, 160, 60, 255);
static const ImU32 kFrozen = IM_COL32(110, 220, 255, 255);
static const ImU32 kTargetCol = IM_COL32(255, 220, 70, 255);
static const char *const kLevelTitle[4] = {"[Move states]", "[Move & Script states]", "[Move, Script & Cooldowns]",
                                           "[Everything]"};

struct Line { char s[176]; ImU32 col; };
static int BuildLines(const MItem &it, const MInfo &mi, Line *l, int max, bool panel)
{
    int n = 0;
    float um = kUnitsPerM;
    SPRINTF(l[n].s, "%s%s", mi.name, (g_monFreeze && it.kind == MK_MONSTER) ? " [frozen]" : "");
    l[n++].col = (g_monFreeze && it.kind == MK_MONSTER) ? kFrozen : kWhite;
    if (mi.stats) SPRINTF(l[n].s, "%.0f/%d HP  lvl %d", mi.hp, mi.maxHp, mi.level);
    else SPRINTF(l[n].s, "HP n/a");
    l[n++].col = kWhite;
    if (g_monLevel >= 0 && mi.state[0] && n < max) SPRINTF(l[n].s, "move: %s", mi.state), l[n++].col = kWhite;
    if (g_monLevel >= 1 && it.kind == MK_MONSTER && n < max) {
        if (mi.beh[0]) SPRINTF(l[n].s, "AI: %s%s%s", mi.beh, mi.sub[0] ? ": " : "", mi.sub);
        else SPRINTF(l[n].s, "AI: -");
        l[n++].col = kWhite;
    }
    if (g_monLevel >= 2 && it.kind == MK_MONSTER && n < max)
        SPRINTF(l[n].s, "cooldowns: %s", mi.coolN ? mi.cool : "-"), l[n++].col = kWhite;
    if (g_monLevel >= 3 && n < max) {
        if (mi.target[0]) SPRINTF(l[n].s, "target: %s (%.0f units, ~%.1f m)", mi.target, mi.targetDist,
                                    mi.targetDist / um);
        else SPRINTF(l[n].s, "target: -");
        l[n++].col = kWhite;
        if (mi.stats && n < max)
            SPRINTF(l[n].s, "damage %d  defense %d  resistance %d", mi.damage, mi.defense, mi.resist), l[n++].col = kWhite;
        if (n < max) SPRINTF(l[n].s, "pos (%.0f %.0f %.0f)", it.pos.x, it.pos.y, it.pos.z), l[n++].col = kDim;
    }
    if (g_monDamage && n < max) {
        DmgEnt *e = DmgOf(it.obj, false);
        if (e && e->hits && (panel || GetTickCount() - e->at < 4000)) {
            SPRINTF(l[n].s, "hit -%.0f (%.1f s ago)  total -%.0f in %d", e->last, (GetTickCount() - e->at) / 1000.0f,
                      e->total, e->hits);
            l[n++].col = kHit;
        } else if (panel) {
            SPRINTF(l[n].s, "no hits yet");
            l[n++].col = kDim;
        }
    }
    return n;
}

struct Rect { float x0, y0, x1, y1; };
static Rect s_placed[kMaxLabels];
static int s_numPlaced;

static void DrawLabels(ImDrawList *dl, const int *order, int count)
{
    ImFont *font = ImGui::GetFont();
    float fsz = ImGui::GetFontSize() * 0.85f, lh = fsz + 1.0f;
    s_numPlaced = 0;
    for (int k = 0; k < count && s_numPlaced < kMaxLabels; k++) {
        const MItem &it = s_items[order[k]];
        MInfo mi;
        ReadInfo(it, mi);
        DmgUpdate(it, mi);
        Line lines[10];
        int n = BuildLines(it, mi, lines, 10, false);
        float w = 0;
        for (int i = 0; i < n; i++) w = fmaxf(w, font->CalcTextSizeA(fsz, FLT_MAX, 0, lines[i].s).x);
        Rect r = {it.sx - w * 0.5f - 3, it.sy - n * lh - 4, it.sx + w * 0.5f + 3, it.sy};
        // nearest first; a label that hits an already placed one moves up above it (3 tries), else it is skipped
        bool ok = false;
        for (int tries = 0; tries < 4 && !ok; tries++) {
            ok = true;
            for (int p = 0; p < s_numPlaced; p++) {
                const Rect &q = s_placed[p];
                if (r.x0 < q.x1 && r.x1 > q.x0 && r.y0 < q.y1 && r.y1 > q.y0) {
                    float hgt = r.y1 - r.y0;
                    r.y1 = q.y0 - 2;
                    r.y0 = r.y1 - hgt;
                    ok = false;
                    break;
                }
            }
        }
        if (!ok || r.y0 < 0 || r.x1 < 0 || r.x0 > s_W || r.y1 > s_H) {
            s_st.overlap++;
            continue;
        }
        s_placed[s_numPlaced++] = r;
        bool isT = it.obj == s_target && g_monTarget;
        dl->AddRectFilled(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1), IM_COL32(0, 0, 0, 115), 2.0f);
        if (isT) dl->AddRect(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1), kTargetCol, 2.0f, 0, 1.5f);
        for (int i = 0; i < n; i++) {
            float tw = font->CalcTextSizeA(fsz, FLT_MAX, 0, lines[i].s).x;
            dl->AddText(font, fsz, ImVec2(floorf(it.sx - tw * 0.5f), floorf(r.y0 + 2 + i * lh)), lines[i].col, lines[i].s);
        }
        s_st.drawn++;
    }
}

static int CmpDist(const void *a, const void *b)
{
    float da = s_items[*(const int *)a].dist, db = s_items[*(const int *)b].dist;
    return da < db ? -1 : (da > db ? 1 : 0);
}

// the monster the camera looks at: the smallest angle between the view direction and the body centre, <= 12 deg;
// the current one stays while it is within 16 deg and not 3 deg worse than the best (no flicker)
static int PickTarget()
{
    int best = -1, cur = -1;
    for (int i = 0; i < s_numItems; i++) {
        if (s_items[i].aim > 90) continue;
        if (s_items[i].obj == s_target) cur = i;
        if (s_items[i].aim <= 12.0f && (best < 0 || s_items[i].aim < s_items[best].aim - 0.01f ||
                                        (fabsf(s_items[i].aim - s_items[best].aim) < 0.01f &&
                                         s_items[i].dist < s_items[best].dist)))
            best = i;
    }
    if (cur >= 0 && s_items[cur].aim <= 16.0f && (best < 0 || s_items[cur].aim <= s_items[best].aim + 3.0f)) return cur;
    return best;
}

static void DrawTargetMark(ImDrawList *dl, const MItem &it)
{
    float c[3] = {it.pos.x, it.pos.y, it.pos.z + (it.height > 0 ? it.height : 150.0f) * 0.5f}, x, y;
    if (!Project(c, &x, &y)) return;
    float s = 14.0f, k = 6.0f;
    ImU32 col = kTargetCol;
    dl->AddLine(ImVec2(x - s, y - s), ImVec2(x - s + k, y - s), col, 2);
    dl->AddLine(ImVec2(x - s, y - s), ImVec2(x - s, y - s + k), col, 2);
    dl->AddLine(ImVec2(x + s, y - s), ImVec2(x + s - k, y - s), col, 2);
    dl->AddLine(ImVec2(x + s, y - s), ImVec2(x + s, y - s + k), col, 2);
    dl->AddLine(ImVec2(x - s, y + s), ImVec2(x - s + k, y + s), col, 2);
    dl->AddLine(ImVec2(x - s, y + s), ImVec2(x - s, y + s - k), col, 2);
    dl->AddLine(ImVec2(x + s, y + s), ImVec2(x + s - k, y + s), col, 2);
    dl->AddLine(ImVec2(x + s, y + s), ImVec2(x + s, y + s - k), col, 2);
}

static const ImGuiWindowFlags kPanel = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
                                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                       ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize;

// right side, from 40 % of the height down: below the showCameraPosition panel (top right), away from the flight
// line (bottom centre), the triggers / monsters lines (bottom left) and the F10 label (top centre)
static void DrawTargetPanel(int ti)
{
    float fs = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(ImVec2(s_W - fs * 1.5f, s_H * 0.40f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.70f);
    ImGui::Begin("##dbgmontarget", NULL, kPanel);
    ImGui::SetWindowFontScale(0.9f);
    float wrap = fmaxf(fs * 14.0f, s_W * 0.38f - ImGui::GetStyle().WindowPadding.x * 2);  // keep off the centre
    ImGui::PushTextWrapPos(wrap);
    ImGui::TextColored(ImVec4(1.0f, 0.80f, 0.30f, 1.0f), "info: target");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", kLevelTitle[g_monLevel & 3]);
    if (ti < 0) {
        ImGui::TextDisabled("no monster in the centre of the screen");
        ImGui::PopTextWrapPos();
        ImGui::End();
        return;
    }
    const MItem &it = s_items[ti];
    MInfo mi;
    ReadInfo(it, mi);
    DmgUpdate(it, mi);
    ImGui::Text("%s", mi.name);
    if (g_monFreeze && it.kind == MK_MONSTER) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.43f, 0.86f, 1.0f, 1.0f), "[frozen]");
    }
    ImGui::TextDisabled("class %s%s%s  (%s)", mi.cls, mi.obj[0] ? "  object " : "", mi.obj,
                        it.kind == MK_MONSTER ? "monster" : it.kind == MK_NPC ? "NPC" : "player");
    if (mi.stats) {
        float frac = mi.maxHp > 0 ? mi.hp / mi.maxHp : 0;
        ImGui::Text("HP %.0f / %d (%.0f%%)%s", mi.hp, mi.maxHp, frac * 100.0f, mi.maxOk ? "" : " (base max)");
        ImGui::Text("level %d   damage %d   defense %d   resistance %d", mi.level, mi.damage, mi.defense, mi.resist);
    } else {
        ImGui::TextDisabled("HP / stats: n/a");
    }
    ImGui::Text("move: %s", mi.state[0] ? mi.state : "-");
    if (it.kind == MK_MONSTER) {
        if (g_monLevel >= 1) ImGui::Text("AI: %s%s%s", mi.beh[0] ? mi.beh : "-", mi.sub[0] ? ": " : "", mi.sub);
        if (g_monLevel >= 2) ImGui::Text("cooldowns: %s", mi.coolN ? mi.cool : "-");
        if (g_monLevel >= 3) {
            if (mi.target[0]) ImGui::Text("target: %s (%.0f units, ~%.1f m)", mi.target, mi.targetDist,
                                          mi.targetDist / kUnitsPerM);
            else ImGui::Text("target: -");
        }
    }
    ImGui::TextDisabled("distance %.0f units (~%.1f m)  angle %.1f deg  height %.0f", it.dist, it.dist / kUnitsPerM,
                        it.aim, it.height);
    if (g_monLevel >= 3) ImGui::TextDisabled("pos (%.1f %.1f %.1f)", it.pos.x, it.pos.y, it.pos.z);
    if (g_monDamage) {
        DmgEnt *e = DmgOf(it.obj, false);
        if (e && e->hits) ImGui::TextColored(ImVec4(1.0f, 0.63f, 0.24f, 1.0f), "hit -%.0f (%.1f s ago)  total -%.0f in %d hits",
                                             e->last, (GetTickCount() - e->at) / 1000.0f, e->total, e->hits);
        else ImGui::TextDisabled("damage: no hits yet");
    }
    ImGui::PopTextWrapPos();
    ImGui::End();
}

// bottom left, above the triggers line: the Xbox screen list ("Monsters: %d", "  class: count")
static void DrawSummary(bool full)
{
    float fs = ImGui::GetFontSize();
    float bottom = s_H - fs * 3.6f - (g_showTriggers ? fs * 1.45f : 0.0f);
    ImGui::SetNextWindowPos(ImVec2(fs * 1.5f, bottom), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::Begin("##dbgmonsum", NULL, kPanel);
    ImGui::SetWindowFontScale(0.8f);
    if (full) ImGui::Text("monsters %s  Monsters: %d", kLevelTitle[g_monLevel & 3], s_st.monsters);
    else ImGui::Text("monsters");
    ImGui::SameLine();
    if (!full) {
        ImGui::TextDisabled("info off");
    } else if (g_monNearby) {
        char range[24];
        if (kRangeM[g_monRange]) SPRINTF(range, "%.0f m", kRangeM[g_monRange]);
        else STRCPY(range, "unlimited");
        ImGui::TextDisabled("labels %d / %d in %s", s_st.drawn, s_st.inRange, range);
    } else {
        ImGui::TextDisabled("labels off");
    }
    if (full && g_monLevel >= 3) {
        ImGui::SameLine();
        ImGui::TextDisabled("Characters: %d (NPC %d, player %d)", s_st.chars, s_st.npcs, s_st.players);
    }
    if (g_monFreeze) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.43f, 0.86f, 1.0f, 1.0f), "FROZEN");
    }
    if (g_monDamage) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.63f, 0.24f, 1.0f), "damage info");
    }
    ImGui::End();
}

static void LogFields(const char *why)
{
    struct CC { void *cls; int n, kind; } cc[32];
    int ncc = 0;
    for (int i = 0; i < s_numItems; i++) {
        int j = 0;
        while (j < ncc && cc[j].cls != s_items[i].cls) j++;
        if (j == ncc && ncc < 32) cc[ncc].cls = s_items[i].cls, cc[ncc].n = 0, cc[ncc].kind = s_items[i].kind, ncc++;
        if (j < ncc) cc[j].n++;
    }
    Log("monsters: %s: EventManagers %d, nodes %d, characters %d (monsters %d, NPC %d, players %d), listed %d | "
        "bad nodes %d, broken lists %d, too long %d, over limit %d | classes Monster %08X NPC %08X Player %08X",
        why, s_st.evms, s_st.nodes, s_st.chars, s_st.monsters, s_st.npcs, s_st.players, s_numItems, s_st.badNodes,
        s_st.broken, s_st.longList, s_st.overflow, (unsigned)(UINT_PTR)s_clsMonster, (unsigned)(UINT_PTR)s_clsNpc,
        (unsigned)(UINT_PTR)s_clsPlayer);
    for (int j = 0; j < ncc; j++) {
        char chain[160] = "";
        __try {
            void *c = *(void **)((BYTE *)cc[j].cls + OFS_CLS_PARENT);
            for (int d = 0; c && d < 8; d++) {
                strncat_s(chain, sizeof(chain), " > ", _TRUNCATE);
                strncat_s(chain, sizeof(chain), ClassName(c), _TRUNCATE);
                c = *(void **)((BYTE *)c + OFS_CLS_PARENT);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        Log("monsters:   class %08X %s%s: %d", (unsigned)(UINT_PTR)cc[j].cls, ClassName(cc[j].cls), chain, cc[j].n);
    }
    if (!Fake()) {  // the player's proxy height: Death is about 2 m tall -> units per metre (range, distances)
        void *game = NULL, *actor = NULL;
        if (Game_GetGame(&game) && game && Game_GetActor(game, &actor) && actor)
            Log("monsters:   player proxy height %.1f units (with 1 m = %.1f units: %.2f m)", DescHeight(actor),
                kUnitsPerM, DescHeight(actor) / kUnitsPerM);
    }
    for (int i = 0, shown = 0; i < s_numItems && shown < 4; i++) {
        const MItem &it = s_items[i];
        MInfo mi;
        ReadInfo(it, mi);
        shown++;
        void *vt = NULL;
        __try { vt = *(void **)it.obj; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        Log("monsters:   field check: \"%s\" class %s object \"%s\" vt %08X pos (%.1f %.1f %.1f) dist %.0f | stats %s: "
            "HP %.1f / max %d (%s) lvl %d dmg %d def %d res %d | height %.1f | move \"%s\" | AI \"%s\" \"%s\" "
            "cooldowns %d \"%s\" | target \"%s\" %.0f",
            mi.name, mi.cls, mi.obj, (unsigned)(UINT_PTR)vt, it.pos.x, it.pos.y, it.pos.z, it.dist,
            mi.stats ? "ok" : "n/a", mi.hp, mi.maxHp, mi.maxOk ? "getMaxHealth" : "base +0x8C", mi.level, mi.damage,
            mi.defense, mi.resist, it.height, mi.state, mi.beh, mi.sub, mi.coolN, mi.cool, mi.target, mi.targetDist);
    }
}

static void TurnOff(const char *why)
{
    g_monNearby = g_monTarget = false;
    Log("monsters: info turned OFF: %s", why);
    Menu_Status("monsters info: off (%s, see the log)", why);
}

static void DrawInner()
{
    ImGuiIO &io = ImGui::GetIO();
    s_W = io.DisplaySize.x;
    s_H = io.DisplaySize.y;
    s_viewOk = Trig_View(s_vp, s_cam, s_proj);
    double t0 = NowMs();
    if (!s_clsMonster && !ReadBaseClasses()) {
        TurnOff("no class descriptors");
        return;
    }
    __try {
        CollectInner();
    } __except (CatchMon("monster collection", GetExceptionInformation())) {
        s_numItems = 0;
        TurnOff("exception while collecting");
        return;
    }
    double t1 = NowMs();
    // distances, screen anchors, aim angles (the camera of this frame: free flight / blend included)
    float cp[3] = {s_cam[12], s_cam[13], s_cam[14]}, fw[3] = {-s_cam[8], -s_cam[9], -s_cam[10]};
    float maxD = kRangeM[g_monRange] * kUnitsPerM;
    int order[kMaxItems], no = 0;
    for (int i = 0; i < s_numItems; i++) {
        MItem &it = s_items[i];
        float h = it.height > 0 ? it.height : 150.0f;
        float c[3] = {it.pos.x, it.pos.y, it.pos.z + h * 0.5f};
        float d[3] = {c[0] - cp[0], c[1] - cp[1], c[2] - cp[2]};
        it.dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        float cosA = it.dist > 1 ? (d[0] * fw[0] + d[1] * fw[1] + d[2] * fw[2]) / it.dist : -1;
        it.aim = cosA > 0 ? acosf(fminf(cosA, 1.0f)) * 57.29578f : 999.0f;
        float top[3] = {it.pos.x, it.pos.y, it.pos.z + h + 8.0f};
        it.onScreen = s_viewOk && Project(top, &it.sx, &it.sy);
        if (g_monNearby && (maxD <= 0 || it.dist <= maxD)) {
            s_st.inRange++;
            if (it.onScreen && it.sx > -200 && it.sx < s_W + 200 && it.sy > -50 && it.sy < s_H + 400) order[no++] = i;
        }
    }
    int ti = -1;
    if (g_monTarget && s_viewOk) {
        ti = PickTarget();
        void *nt = ti >= 0 ? s_items[ti].obj : NULL;
        if (nt != s_target) {
            s_target = nt;
            if (ti >= 0) {
                MInfo mi;
                ReadInfo(s_items[ti], mi);
                STRCPY(s_targetName, mi.name);
            } else {
                s_targetName[0] = 0;
            }
            if (LogBudget(&s_targetBudget, 300))
                Log("monsters: target -> %s (%.0f units, %.1f deg)", ti >= 0 ? s_targetName : "none",
                    ti >= 0 ? s_items[ti].dist : 0, ti >= 0 ? s_items[ti].aim : 0);
        }
    }
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    if (no > 1) qsort(order, no, sizeof(int), CmpDist);
    if (g_monNearby && s_viewOk) DrawLabels(dl, order, no);
    if (ti >= 0) DrawTargetMark(dl, s_items[ti]);
    if (g_monTarget) DrawTargetPanel(s_viewOk ? ti : -1);
    DrawSummary(true);
    double t2 = NowMs();
    if (s_logOnEnable) {
        s_logOnEnable = false;
        LogFields("on");
    }
    s_lastCollect = t1 - t0;
    s_lastDraw = t2 - t1;
    s_collectMs += t1 - t0;
    s_drawMs += t2 - t1;
    s_collectMax = fmax(s_collectMax, t1 - t0);
    s_drawMax = fmax(s_drawMax, t2 - t1);
    s_frames++;
    if (GetTickCount() - s_statsTick >= kStatsMs) {
        if (LogBudget(&s_statsBudget, 300))
            Log("monsters: %d characters (monsters %d, NPC %d, players %d), listed %d, in range %d, labels %d (skipped "
                "%d) | target %s | collect %.3f ms avg (max %.3f) | draw %.3f ms avg (max %.3f) over %d frames | "
                "level %d, range %s, freeze %d, damage %d",
                s_st.chars, s_st.monsters, s_st.npcs, s_st.players, s_numItems, s_st.inRange, s_st.drawn,
                s_st.overlap, s_target ? s_targetName : "-", s_frames ? s_collectMs / s_frames : 0, s_collectMax,
                s_frames ? s_drawMs / s_frames : 0, s_drawMax, s_frames, g_monLevel + 1,
                kRangeM[g_monRange] ? "limited" : "unlimited", g_monFreeze, g_monDamage);
        s_statsTick = GetTickCount();
        s_collectMs = s_drawMs = s_collectMax = s_drawMax = 0;
        s_frames = 0;
    }
}

void Mon_Draw2D()
{
    bool on = g_monNearby || g_monTarget;
    if (!on) {
        s_wasOn = false;
        s_target = NULL;
        if (g_monFreeze || g_monDamage) {  // only the summary line (state of the flags)
            ImGuiIO &io = ImGui::GetIO();
            s_W = io.DisplaySize.x;
            s_H = io.DisplaySize.y;
            DrawSummary(false);
        }
        return;
    }
    if (!g_gameMatch && !g_testMode) return;
    if (!s_wasOn) {
        s_wasOn = true;
        s_logOnEnable = true;
        s_statsTick = GetTickCount();
        s_collectMs = s_drawMs = s_collectMax = s_drawMax = 0;
        s_frames = 0;
        Log("monsters: info ON: nearby %d target %d, level %d, range %.0f m (0 = unlimited), show %d", g_monNearby,
            g_monTarget, g_monLevel + 1, kRangeM[g_monRange], g_monWho);
    }
    __try {
        DrawInner();
    } __except (CatchMon("monster draw", GetExceptionInformation())) {
        TurnOff("exception while drawing");
    }
}

// ======================= offline test controls (17..26) =======================
// 17 normal list, 18 log check, 19 hit the first monster (-25 HP), 20 the player dies, 21 level load, 22 exception in
// the next collection, 23 300 monsters (timing), 24 the game commits the options (profile value check), 25 the
// first monster is gone, 26 the player alive again
static void FakeBasis()
{
    float vp[16], cam[16], proj[16];
    if (!Trig_View(vp, cam, proj)) return;
    float fx = -cam[8], fy = -cam[9], l = sqrtf(fx * fx + fy * fy);
    if (l < 0.1f) return;
    s_fO[0] = cam[12], s_fO[1] = cam[13], s_fO[2] = cam[14];
    s_fF[0] = fx / l, s_fF[1] = fy / l;
    Log("test: fake monsters in front of the camera (%.0f %.0f %.0f) forward (%.2f %.2f)", s_fO[0], s_fO[1], s_fO[2],
        s_fF[0], s_fF[1]);
}

void Mon_TestControl(int cmd)
{
    if (!Fake()) return;
    if (cmd == 17 || cmd == 23 || cmd == 25 || cmd == 26) FakeBasis();
    FakeBuild();
    switch (cmd) {
    case 17: s_fakeMode = 0; Log("test: monsters fake list normal"); break;
    case 18: {
        float x = 0, y = 0;
        GameVec *p0 = (GameVec *)(s_fObj[0] + 0x260);
        float top[3] = {p0->x, p0->y, p0->z + 70 + 8};
        bool ok = s_viewOk && Project(top, &x, &y);
        Log("test: monsters check: view %d, first monster anchor -> %s (%.1f %.1f) of %.0fx%.0f | listed %d, in range %d, "
            "labels %d, skipped %d | target %s | update flag %u | options DamageIndicators %u, profile %u | player HP %.0f"
            " | items: nearby %d target %d level %d range %d show %d damage %d freeze %d | last collect %.3f ms, "
            "draw %.3f ms",
            s_viewOk, ok ? "screen" : "off screen", x, y, s_W, s_H, s_numItems, s_st.inRange, s_st.drawn, s_st.overlap,
            s_target ? s_targetName : "-", s_fakeFlag, s_fakeOpt[OFS_OPT_DAMAGEIND], s_fakeOptBlock, s_fakePlayerHp,
            g_monNearby, g_monTarget, g_monLevel + 1, g_monRange, g_monWho, g_monDamage, g_monFreeze, s_lastCollect,
            s_lastDraw);
        LogFields("test check");
        break;
    }
    case 19: {
        float &hp = *(float *)(s_fStats[0] + OFS_ST_HP);
        hp -= 25.0f;
        Log("test: hit Skel_A01 -25 -> %.0f", hp);
        break;
    }
    case 20: s_fakePlayerHp = 0; Log("test: the player dies (HP 0)"); break;
    case 21: s_fakeWorld++; Log("test: level load (world %d)", s_fakeWorld); break;
    case 22: s_testThrow = true; Log("test: forcing an exception in the next monster collection"); break;
    case 23: s_fakeMode = 23; Log("test: 300 fake monsters"); break;
    case 24: {  // what the game's commit 0x004DA98B does, with our hook around it
        int prev = Mon_OptionsHide(s_fakeOpt);
        s_fakeOptBlock = s_fakeOpt[OFS_OPT_DAMAGEIND];
        if (prev >= 0) Mon_OptionsRestore(s_fakeOpt, prev);
        Log("test: options commit: hidden %d, profile value %u, live value %u", prev >= 0, s_fakeOptBlock,
            s_fakeOpt[OFS_OPT_DAMAGEIND]);
        break;
    }
    case 25: s_fakeMode = 25; Log("test: the first monster is gone from the list"); break;
    case 26: s_fakePlayerHp = 100; s_fakeMode = 0; Log("test: player alive, list normal"); break;
    }
}

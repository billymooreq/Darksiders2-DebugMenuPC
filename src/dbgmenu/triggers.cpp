// Show triggers (task 07, notes/triggers_pc.md): the live trigger volumes, after the Xbox EXPERIMENTAL patch
// ds2dbg_detectors.s (Xbox project, patches/src): the same walk, filter, shapes and colours, but drawn by us.
//   collect (game thread, once per game frame, before the frame 0x00B3D3E4): worlds = player actor+0x58 and the
//     current world of the loader game+0xBC (0x00AC153E: +0x10 == 4 -> +0xC; one world, regions stream into it,
//     both are walked and de-duplicated), EventManager = getEventManager 0x007735E8(world), its detector list
//     evm+0x14 {head, tail, count}, node {next, obj} (DetectorObject::onAdd 0x008B84FE -> add 0x00ABDCC8; the
//     actor list is evm+0x8). Filter: vt[0x40] == 0x00A24AFB && vt[0x48] == 0x00A24B3D (SimpleShapeObject
//     getRotation/getScale) like the Xbox patch; fields +0x80 Shape, +0x90 Pos, +0xA0 Rot (deg), +0xB0 Scale (full
//     size), +0xD8 enabled. A snapshot of plain numbers is kept; nothing of the game is read while drawing.
//   shapes (onAdd 0x008B84FE): 0 = AABB pos +- S/2 (rotation ignored), 1 = OBB S/2 rotated by Rx*Ry*Rz (0x00A79856),
//     2 = sphere r = S.z/2 (3 circles), 3 = capsule: like the Xbox patch an oriented box (S.x/2, S.x/2, S.z/2);
//     other values -> AABB (as the Xbox patch).
//   colours: magenta = TriggerRegion (vt 0x012702A0 / 0x012703F0), cyan = other detectors, gray paler = disabled.
//   view: the matrices the game gives its renderer for the frame - hook of 0x00983AFC called from 0x00B3CE31
//     (camera matrix from game->vt[0x7C] = also our free camera / blend, projection 0x009055F7 = RH perspective);
//     segments are clipped in clip space (near plane + screen edges), objects culled by the frustum (no far plane).
//   through walls: ImGui background lines at Present. depth: D3D lines with z-test against the bound depth buffer,
//     drawn at the entry of the HUD pass 0x00B3B8D2 (after the 3D scene), else at Present; no usable depth
//     buffer -> through walls for that frame.
// Task 13 (notes/triggers_pc.md §7): info about the triggers like Monsters - labels near the player (info: nearby),
//   the panel of the trigger in the centre of the screen (info: target, F6) with its highlight, Display switches.
//   Fields: name (+0x50), class chain, every reflection property (reflect.cpp), TriggerRegion OnEnterList /
//   OnExitList "Object:Event" (0x008C8E8C sends the event to the named objects), FilterList / NameFilter,
//   OneShot +0x11C, Enable +0x11D, Exclude +0x11E, RequireInWorld +0x11F, done once +0x120 / +0x121, listeners
//   (WorldObject +0x64 list), LoadOnEnter region ids -> names (world.cpp), the player inside (body +0xD4 list) and
//   our enter / exit counts (task 09 hooks). Read in the game frame right after the collection (objects alive),
//   drawn at Present from text only.
#include "common.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <float.h>
#include "imgui.h"

bool g_showTriggers;
int g_trigDepthMode = TD_WALLS;
bool g_trigShowDisabled = true;

#define OFS_ACTOR_WORLD    0x58
#define OFS_GAME_LOADER    0xBC   // world loader (0x00B3B15C): +0x10 state (4 = world ready), +0xC world handle
#define OFS_LOADER_STATE   0x10
#define OFS_LOADER_WORLD   0x0C
#define PC_GET_EVM         0x007735E8u  // EventManager* __thiscall getEventManager(World*): map World+0x118
#define OFS_EVM_DETECTORS  0x14   // {head, tail, count}; node {next, obj}
#define OFS_OBJ_SHAPE      0x80
#define OFS_OBJ_POS        0x90
#define OFS_OBJ_ROT        0xA0
#define OFS_OBJ_SCALE      0xB0
#define OFS_OBJ_ENABLED    0xD8
#define VT_OBJ_GETROT      0x40
#define VT_OBJ_GETSCALE    0x48
#define PC_SS_GETROT       0x00A24AFBu
#define PC_SS_GETSCALE     0x00A24B3Du
#define PC_VT_TRIGGER      0x012702A0u
#define PC_VT_TRIGGER2     0x012703F0u  // scripted variant
#define PC_OBJ_XFORM       0x00A79856u  // __thiscall (obj, Matrix* out): identity, rotX/Y/Z(deg*[0x015352AC]), +0x90
#define PC_RV_MAIN_RET     0x00B3CE31u  // return address of the main camera's 0x00983AFC call in 0x00B3CD27

static const int kMaxItems = 32768;
static const int kMaxNodes = 65536;       // a longer list is broken (cycle)
static const int kCircleSeg = 48;
static const int kLineChunk = 4096;       // D3D lines per DrawPrimitiveUP
static const DWORD kStatsMs = 5000;

enum { SH_BOX, SH_SPHERE };
struct TItem {
    float c[3];      // centre
    float h[3];      // box half extents
    float r[9];      // box rotation rows (local axis i = r[3i..3i+2]); identity for AABB
    float rad;       // sphere radius / bounding radius
    BYTE draw, shape, trig, enabled;
    UINT_PTR vt;
    void *obj;       // task 13: valid only inside Trig_Frame (the info is read there)
};
static TItem s_items[kMaxItems];
static int s_numItems;

// one collection's statistics
struct TStats {
    int worlds, evms, nodes, items, filtered, skipped, nodeEx, broken, longList, disabled, trig, overflow;
    int shape[5];            // 0..3, 4 = other
    void *world[4], *evm[4];
    int listCount[4], listLen[4];
};
static TStats s_st;
static struct { UINT_PTR vt; int n; } s_classes[64];
static int s_numClasses;

// view of this frame (0x00983AFC hook)
static struct {
    float cam[16], proj[16], vp[16];
    LONG frame;
    bool ok;
    LONG others;  // calls from other callers (not the main camera)
} s_view;
static LONG s_frame;            // game frame counter (Trig_Frame)
static LONG s_depthDrawnFrame = -1, s_hudFrame = -1;
static int s_drawn;             // items with at least one visible segment, last draw
static bool s_depthNA;          // depth mode, but no usable depth buffer last time
static bool s_logOnEnable, s_wasOn, s_viewLogged;
static volatile LONG s_capture; // RT timeline capture: frames left
static int s_capEvents;
static __declspec(thread) int t_ours;  // our own D3D calls (not logged in the timeline)
static LARGE_INTEGER s_qpf;
static double s_collectMs, s_drawMs, s_collectMax, s_drawMax;
static int s_collectN, s_drawN;
static DWORD s_statsTick;
static volatile LONG s_statsBudget, s_exBudget, s_badVtx;

static bool TrigFake() { return g_testMode && !g_gameMatch; }

static double NowMs()
{
    LARGE_INTEGER t;
    if (!s_qpf.QuadPart) QueryPerformanceFrequency(&s_qpf);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)s_qpf.QuadPart;
}

// ======================= offline test: fake EventManager (DBGMENU_TEST=1, not the game) =======================
static void *s_fakeVtDet[0x60 / 4], *s_fakeVtTrig[0x60 / 4], *s_fakeVtActor[0x60 / 4];
static const int kFakeSize = 0x180;   // task 13: up to the fields of the trigger info (+0x170 = the fake class)
static __declspec(align(16)) BYTE s_fakeObj[16][kFakeSize];
static struct FakeNode { FakeNode *next; void *obj; } s_fakeNodes[20];
static BYTE s_fakeEvm[0x20];
static int s_fakeMode;  // 0 = normal, 9 = empty (level unloaded), 10 = broken list, 14 = 8000 objects (timing)
static const int kFakeBig = 8000;
static BYTE *s_fakeBigObj;
static FakeNode *s_fakeBigNodes;
static double s_lastCollectMs, s_lastDrawMs;
static bool s_testThrow;

static void FakeObj(int i, void **vt, int shape, float x, float y, float z, float sx, float sy, float sz, float rz,
                    bool enabled)
{
    BYTE *o = s_fakeObj[i];
    memset(o, 0, kFakeSize);
    *(void ***)o = vt;
    *(int *)(o + OFS_OBJ_SHAPE) = shape;
    float *p = (float *)(o + OFS_OBJ_POS), *r = (float *)(o + OFS_OBJ_ROT), *s = (float *)(o + OFS_OBJ_SCALE);
    p[0] = x; p[1] = y; p[2] = z;
    r[2] = rz;
    s[0] = sx; s[1] = sy; s[2] = sz;
    o[OFS_OBJ_ENABLED] = enabled;
}

// task 13: classes, names and fields of the fake triggers for the trigger info (layout of the real TriggerRegion:
// +0xE0 FilterList, +0xF8 NameFilter, +0x104 OnEnterList, +0x11C OneShot ...; +0x130 Waypoint, +0x150 MaxDistance,
// +0x164 LoadOnEnter are the fake places of the other classes; +0x170 = the fake class)
enum { FK_DET, FK_TRIG, FK_KILL, FK_SCRIPTKILL, FK_LOAD, FK_CAM, FK_HANDLER, FK_COUNT };
static __declspec(align(8)) BYTE s_fCls[FK_COUNT][0x30];
static void *s_fClsProps[FK_COUNT][8];
static __declspec(align(8)) DWORD s_fNames[8][2];
static BYTE s_fakePlayer[16];
static __declspec(align(8)) BYTE s_fHandler[0x180], s_fNode[0x10], s_fBody[0x40];
static void *s_fBodyObjs[4];
static int s_fBodyCounts[4];
static BYTE s_fDoneEnter;
static DWORD s_fLoads[2][4] = {{2, (DWORD)-1, 3, 1}, {1, (DWORD)-1, 0, 0}};
static void *__fastcall FakeClassOf(void *self, void *) { return *(void **)((BYTE *)self + 0x170); }
__declspec(naked) static void FakeObjGetName() { __asm { lea eax, [ecx + 0x50] } __asm { ret } }

static void FakeArr(BYTE *o, int ofs, void *data, int n)
{
    *(void **)(o + ofs) = data;
    *(int *)(o + ofs + 4) = n;
    *(int *)(o + ofs + 8) = n;
}

static void FakeInfoFields()
{
    static const char *const kCls[FK_COUNT] = {"DetectorObject", "TriggerRegion", "KillRegion", "killregiontrigger",
                                               "LoadRegion", "CameraInfluenceTriggerRegion", "TriggerEventHandler"};
    // property indexes of reflect.cpp kFakeProps
    static const int kProps[FK_COUNT][8] = {{0, 5, 6, -1}, {1, 2, 3, 4, 7, 8, 9, 13}, {10, -1}, {-1}, {12, -1},
                                            {11, -1}, {0, -1}};
    static const int kParent[FK_COUNT] = {-1, FK_DET, FK_TRIG, FK_KILL, FK_DET, FK_DET, -1};
    for (int c = 0; c < FK_COUNT; c++) {
        int n = 0;
        for (int k = 0; k < 8 && kProps[c][k] >= 0; k++) s_fClsProps[c][n++] = Refl_FakeProp(kProps[c][k]);
        Refl_FakeClass(s_fCls[c], kCls[c], kParent[c] >= 0 ? s_fCls[kParent[c]] : NULL, s_fClsProps[c], n);
    }
    void **vts[3] = {s_fakeVtDet, s_fakeVtTrig, s_fakeVtActor};
    for (int v = 0; v < 3; v++) {
        vts[v][1] = (void *)FakeClassOf;
        vts[v][0x2C / 4] = (void *)FakeObjGetName;
        vts[v][0x38 / 4] = (void *)Refl_FakeGetPos;
    }
    static const char *const kNames[10] = {"Door_Z2_Trigger", "KillReg_Pit_01", "LoadRegion_02", "CamInfl_01",
                                           "Capsule_Trg", "Disabled_Trg", "Big_LoadRegion", "Behind_Det", "",
                                           "Odd_Shape"};
    static const int kKind[10] = {FK_TRIG, FK_SCRIPTKILL, FK_LOAD, FK_CAM, FK_TRIG, FK_TRIG, FK_LOAD, FK_DET, -1, FK_DET};
    static const char *const kStr[8] = {"Player", "Horse", "Door_01:Open", "Z2_Cine_VSM", "Hub_01_KillReg_Wpt",
                                        "Z2_Door_VSM", "Monster", ""};
    for (int i = 0; i < 8; i++) Refl_FakeName(s_fNames[i], kStr[i]);
    for (int i = 0; i < 10; i++) {
        BYTE *o = s_fakeObj[i];
        *(void **)(o + 0x170) = kKind[i] >= 0 ? s_fCls[kKind[i]] : NULL;
        Refl_FakeName(o + 0x50, kNames[i][0] ? kNames[i] : NULL);
        *(int *)(o + 0x14) = 100 + i;
        if (kKind[i] == FK_TRIG || kKind[i] == FK_SCRIPTKILL) {
            o[0x11C] = i == 0;           // OneShot
            o[0x11D] = i != 5;           // Enable
            o[0x11F] = 1;                // RequireInWorld
            FakeArr(o, 0xE0, s_fNames, i == 0 ? 2 : 1);  // FilterList Player (, Horse)
        }
        if (kKind[i] == FK_LOAD) FakeArr(o, 0x164, s_fLoads[i == 2 ? 0 : 1], i == 2 ? 2 : 1);
    }
    BYTE *door = s_fakeObj[0];
    *(DWORD *)(door + 0x140) = 2;               // IncludeGrabbables (bit 1)
    FakeArr(door, 0x104, s_fNames[2], 2);       // OnEnterList Door_01:Open, Z2_Cine_VSM
    door[0x120] = s_fDoneEnter;
    *(void **)(door + 0x64) = s_fNode;          // listeners: a script handler
    memset(s_fHandler, 0, sizeof(s_fHandler));
    *(void ***)s_fHandler = s_fakeVtDet;
    *(void **)(s_fHandler + 0x170) = s_fCls[FK_HANDLER];
    Refl_FakeName(s_fHandler + 0x50, kStr[5]);
    memset(s_fNode, 0, sizeof(s_fNode));
    *(void **)(s_fNode + 4) = s_fHandler;
    *(void **)(door + 0xD4) = s_fBody;          // the body: is the player inside
    *(void **)(s_fBody + 0x24) = door;
    *(void ***)(s_fBody + 0x28) = s_fBodyObjs;
    *(int **)(s_fBody + 0x34) = s_fBodyCounts;
    Refl_FakeName(s_fakeObj[1] + 0x130, kStr[4]);                    // KillRegion Waypoint
    *(float *)(s_fakeObj[3] + 0x150) = 450.0f;                       // MaxDistance
    *(void ***)s_fakePlayer = s_fakeVtActor;
}

// the fake camera starts at (100, 200, 300) looking along +X (game.cpp), Z up
static void FakeBuild()
{
    for (int i = 0; i < 0x60 / 4; i++) s_fakeVtDet[i] = s_fakeVtTrig[i] = s_fakeVtActor[i] = (void *)(UINT_PTR)(0x7E000000 + i);
    s_fakeVtDet[VT_OBJ_GETROT / 4] = s_fakeVtTrig[VT_OBJ_GETROT / 4] = (void *)(UINT_PTR)PC_SS_GETROT;
    s_fakeVtDet[VT_OBJ_GETSCALE / 4] = s_fakeVtTrig[VT_OBJ_GETSCALE / 4] = (void *)(UINT_PTR)PC_SS_GETSCALE;
    int n = 0;
    FakeObj(n++, s_fakeVtDet, 0, 700, 200, 300, 200, 200, 200, 0, true);      // AABB straight ahead (screen centre)
    FakeObj(n++, s_fakeVtTrig, 0, 700, 450, 300, 150, 150, 300, 30, true);    // TriggerRegion, rotation ignored
    FakeObj(n++, s_fakeVtDet, 1, 700, -50, 300, 200, 100, 150, 45, true);     // OBB yaw 45
    FakeObj(n++, s_fakeVtDet, 2, 900, 200, 550, 0, 0, 240, 0, true);          // sphere r 120
    FakeObj(n++, s_fakeVtTrig, 3, 1000, -250, 300, 160, 160, 300, 30, true);  // capsule: box (80, 80, 150) yaw 30
    FakeObj(n++, s_fakeVtDet, 0, 650, 650, 300, 120, 120, 120, 0, false);     // disabled
    FakeObj(n++, s_fakeVtDet, 0, 300, 200, 300, 1200, 1200, 600, 0, true);    // big box around the camera (near clip)
    FakeObj(n++, s_fakeVtDet, 0, -900, 200, 300, 200, 200, 200, 0, true);     // behind the camera (culled)
    FakeObj(n++, s_fakeVtActor, 0, 700, 200, 300, 50, 50, 50, 0, true);       // actor in the same list: filtered out
    FakeObj(n++, s_fakeVtDet, 7, 700, 200, 100, 100, 100, 50, 0, true);       // unknown shape -> AABB
    FakeInfoFields();  // task 13: classes, names and fields of the trigger info
    int used = n;
    if (s_fakeMode == 9) used = 0;
    if (s_fakeMode == 14) {  // a 20 x 20 x 20 grid ahead of the camera, every shape, every 5th disabled
        if (!s_fakeBigObj) {
            s_fakeBigObj = (BYTE *)VirtualAlloc(NULL, kFakeBig * kFakeSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            s_fakeBigNodes = (FakeNode *)VirtualAlloc(NULL, kFakeBig * sizeof(FakeNode), MEM_COMMIT | MEM_RESERVE,
                                                      PAGE_READWRITE);
        }
        if (s_fakeBigObj && s_fakeBigNodes) {
            for (int i = 0; i < kFakeBig; i++) {
                BYTE *o = s_fakeBigObj + i * kFakeSize;
                memset(o, 0, kFakeSize);
                *(void ***)o = (i % 7) ? s_fakeVtDet : s_fakeVtTrig;
                *(int *)(o + OFS_OBJ_SHAPE) = i % 4;
                float *p = (float *)(o + OFS_OBJ_POS), *r = (float *)(o + OFS_OBJ_ROT), *s = (float *)(o + OFS_OBJ_SCALE);
                p[0] = 600.0f + (i % 20) * 400.0f;
                p[1] = 200.0f + ((i / 20) % 20 - 10) * 400.0f;
                p[2] = 300.0f + (i / 400 - 10) * 400.0f;
                r[2] = (float)(i % 90);
                s[0] = s[1] = s[2] = 150.0f;
                o[OFS_OBJ_ENABLED] = (i % 5) != 0;
                s_fakeBigNodes[i].obj = o;
                s_fakeBigNodes[i].next = i + 1 < kFakeBig ? &s_fakeBigNodes[i + 1] : NULL;
            }
            memset(s_fakeEvm, 0, sizeof(s_fakeEvm));
            *(void **)(s_fakeEvm + OFS_EVM_DETECTORS) = &s_fakeBigNodes[0];
            *(void **)(s_fakeEvm + OFS_EVM_DETECTORS + 4) = &s_fakeBigNodes[kFakeBig - 1];
            *(int *)(s_fakeEvm + OFS_EVM_DETECTORS + 8) = kFakeBig;
            return;
        }
    }
    for (int i = 0; i < used; i++) {
        s_fakeNodes[i].obj = s_fakeObj[i];
        s_fakeNodes[i].next = i + 1 < used ? &s_fakeNodes[i + 1] : NULL;
    }
    if (s_fakeMode == 10) {  // a node with a bad object, then a cycle back to the head
        s_fakeNodes[used - 1].next = &s_fakeNodes[used];
        s_fakeNodes[used].obj = (void *)(UINT_PTR)0x10;
        s_fakeNodes[used].next = &s_fakeNodes[0];
    }
    memset(s_fakeEvm, 0, sizeof(s_fakeEvm));
    *(void **)(s_fakeEvm + OFS_EVM_DETECTORS) = used ? &s_fakeNodes[0] : NULL;
    *(void **)(s_fakeEvm + OFS_EVM_DETECTORS + 4) = used ? &s_fakeNodes[used - 1] : NULL;
    *(int *)(s_fakeEvm + OFS_EVM_DETECTORS + 8) = used;
}

static bool VtOk(void *vt)
{
    if (TrigFake()) return vt == s_fakeVtDet || vt == s_fakeVtTrig || vt == s_fakeVtActor;
    return InImage(vt);
}

static bool IsTriggerRegion(UINT_PTR vt)
{
    if (TrigFake()) return vt == (UINT_PTR)s_fakeVtTrig;
    return vt == PC_VT_TRIGGER || vt == PC_VT_TRIGGER2;
}

// ======================= math =======================
// row-vector convention (D3D): p' = p * M; a camera matrix has rows right, up, back, position
static void MatMul(const float *a, const float *b, float *o)
{
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            o[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] + a[r * 4 + 2] * b[2 * 4 + c] +
                           a[r * 4 + 3] * b[3 * 4 + c];
}

// 0x004EE2E3: view = inverse of a rigid camera matrix (transposed rotation, -pos . axes)
static void ViewFromCam(const float *m, float *v)
{
    v[0] = m[0]; v[1] = m[4]; v[2] = m[8];  v[3] = 0;
    v[4] = m[1]; v[5] = m[5]; v[6] = m[9];  v[7] = 0;
    v[8] = m[2]; v[9] = m[6]; v[10] = m[10]; v[11] = 0;
    for (int c = 0; c < 3; c++) v[12 + c] = -(m[12] * v[0 + c] + m[13] * v[4 + c] + m[14] * v[8 + c]);
    v[15] = 1;
}

// 0x00A79856: M = Rx * Ry * Rz (each post-multiplied, angles in degrees), rows = local axes in world space
static void RotRows(const float *deg, float *r)
{
    const float k = 0.017453292f;
    float cx = cosf(deg[0] * k), sx = sinf(deg[0] * k), cy = cosf(deg[1] * k), sy = sinf(deg[1] * k);
    float cz = cosf(deg[2] * k), sz = sinf(deg[2] * k);
    float m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    for (int i = 0; i < 3; i++) {  // rotX: col1' = c1 c - c2 s, col2' = c1 s + c2 c
        float a = m[i * 3 + 1], b = m[i * 3 + 2];
        m[i * 3 + 1] = a * cx - b * sx;
        m[i * 3 + 2] = a * sx + b * cx;
    }
    for (int i = 0; i < 3; i++) {  // rotY: col0' = c0 c + c2 s, col2' = c2 c - c0 s
        float a = m[i * 3 + 0], b = m[i * 3 + 2];
        m[i * 3 + 0] = a * cy + b * sy;
        m[i * 3 + 2] = b * cy - a * sy;
    }
    for (int i = 0; i < 3; i++) {  // rotZ: col0' = c0 c - c1 s, col1' = c0 s + c1 c
        float a = m[i * 3 + 0], b = m[i * 3 + 1];
        m[i * 3 + 0] = a * cz - b * sz;
        m[i * 3 + 1] = a * sz + b * cz;
    }
    memcpy(r, m, sizeof(m));
}

// ======================= collect (game thread) =======================
typedef void *(__fastcall *GetEvm_t)(void *world, void *edx);
typedef void(__fastcall *ObjXform_t)(void *obj, void *edx, float *out);

static int CatchTr(const char *where, EXCEPTION_POINTERS *ep)
{
    if (LogBudget(&s_exBudget, 30))
        Log("triggers: exception %08X at %08X in %s", ep->ExceptionRecord->ExceptionCode,
            (unsigned)(UINT_PTR)ep->ExceptionRecord->ExceptionAddress, where);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void AddClass(UINT_PTR vt)
{
    for (int i = 0; i < s_numClasses; i++)
        if (s_classes[i].vt == vt) {
            s_classes[i].n++;
            return;
        }
    if (s_numClasses < 64) {
        s_classes[s_numClasses].vt = vt;
        s_classes[s_numClasses++].n = 1;
    }
}

// one game-thread check of our OBB math against the game's own transform 0x00A79856 (log only)
static void CheckXform(void *obj, const TItem &it)
{
    static bool done;
    static const BYTE kPro[] = {0x55, 0x8B, 0xEC, 0x56, 0x57, 0x8B, 0x7D, 0x08, 0x8B, 0xF1};
    if (done || TrigFake()) return;
    done = true;
    float m[16];
    __try {
        if (memcmp((void *)PC_OBJ_XFORM, kPro, sizeof(kPro)) != 0) {
            Log("triggers: 0x00A79856 has unexpected bytes, transform check skipped");
            return;
        }
        ((ObjXform_t)PC_OBJ_XFORM)(obj, NULL, m);
    } __except (CatchTr("transform check 0x00A79856", GetExceptionInformation())) {
        return;
    }
    float d = 0;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) d = fmaxf(d, fabsf(m[r * 4 + c] - it.r[r * 3 + c]));
    for (int c = 0; c < 3; c++) d = fmaxf(d, fabsf(m[12 + c] - it.c[c]) * 0.001f);
    Log("triggers: rotated shape check: ours vs the game's transform 0x00A79856: max difference %.6f (%s); "
        "rot (%.1f %.1f %.1f) row0 (%.3f %.3f %.3f) game (%.3f %.3f %.3f)",
        d, d < 0.001f ? "same" : "DIFFERENT", ((float *)((BYTE *)obj + OFS_OBJ_ROT))[0],
        ((float *)((BYTE *)obj + OFS_OBJ_ROT))[1], ((float *)((BYTE *)obj + OFS_OBJ_ROT))[2], it.r[0], it.r[1],
        it.r[2], m[0], m[1], m[2]);
}

// 0 = skipped (not a shape object), 1 = item added, -1 = exception
static int ReadNode(BYTE *node, BYTE **next, bool wantClasses)
{
    void *obj;
    __try {
        *next = *(BYTE **)node;
        obj = *(void **)(node + 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *next = NULL;
        return -2;  // the node itself is unreadable: the list ends here
    }
    __try {
        if (!obj) return 0;
        void **vt = *(void ***)obj;
        if (!VtOk(vt)) return 0;
        if ((UINT_PTR)vt[VT_OBJ_GETROT / 4] != PC_SS_GETROT || (UINT_PTR)vt[VT_OBJ_GETSCALE / 4] != PC_SS_GETSCALE)
            return 0;
        if (s_numItems >= kMaxItems) {
            s_st.overflow++;
            return 0;
        }
        BYTE *o = (BYTE *)obj;
        TItem &it = s_items[s_numItems];
        int shape = *(int *)(o + OFS_OBJ_SHAPE);
        const float *p = (const float *)(o + OFS_OBJ_POS), *rot = (const float *)(o + OFS_OBJ_ROT);
        const float *sc = (const float *)(o + OFS_OBJ_SCALE);
        it.vt = (UINT_PTR)vt;
        it.obj = obj;
        it.trig = IsTriggerRegion(it.vt);
        it.enabled = o[OFS_OBJ_ENABLED] != 0;
        it.shape = (BYTE)(shape >= 0 && shape <= 3 ? shape : 4);
        for (int i = 0; i < 3; i++) it.c[i] = p[i];
        static const float kId[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        memcpy(it.r, kId, sizeof(kId));
        if (shape == 2) {
            it.draw = SH_SPHERE;
            it.rad = fabsf(sc[2]) * 0.5f;
        } else {
            it.draw = SH_BOX;
            if (shape == 3) {
                it.h[0] = it.h[1] = fabsf(sc[0]) * 0.5f;
                it.h[2] = fabsf(sc[2]) * 0.5f;
            } else {
                for (int i = 0; i < 3; i++) it.h[i] = fabsf(sc[i]) * 0.5f;
            }
            if (shape == 1 || shape == 3) RotRows(rot, it.r);
            it.rad = sqrtf(it.h[0] * it.h[0] + it.h[1] * it.h[1] + it.h[2] * it.h[2]);
        }
        for (int i = 0; i < 3; i++)
            if (!_finite(it.c[i]) || !_finite(it.h[i])) return 0;
        if (!_finite(it.rad)) return 0;
        s_numItems++;
        s_st.shape[it.shape]++;
        if (!it.enabled) s_st.disabled++;
        if (it.trig) s_st.trig++;
        if (wantClasses) AddClass(it.vt);
        if ((shape == 1 || shape == 3) && (rot[0] != 0 || rot[1] != 0 || rot[2] != 0)) CheckXform(obj, it);
        return 1;
    } __except (CatchTr("trigger node", GetExceptionInformation())) {
        return -1;
    }
}

static void WalkList(int li, BYTE *evm, bool wantClasses)
{
    BYTE *head;
    int count;
    __try {
        head = *(BYTE **)(evm + OFS_EVM_DETECTORS);
        count = *(int *)(evm + OFS_EVM_DETECTORS + 8);
    } __except (CatchTr("EventManager list header", GetExceptionInformation())) {
        s_st.broken++;
        return;
    }
    int limit = (count >= 0 && count < kMaxNodes - 64) ? count + 64 : kMaxNodes;
    int n = 0;
    BYTE *node = head;
    while (node && n < limit) {
        BYTE *next = NULL;
        int r = ReadNode(node, &next, wantClasses);
        n++;
        if (r == 1) s_st.items++;
        else if (r == 0) s_st.skipped++;
        else if (r == -1) s_st.nodeEx++;
        else {
            s_st.broken++;
            break;
        }
        node = next;
    }
    if (node && n >= limit) s_st.longList++;
    s_st.nodes += n;
    if (li < 4) {
        s_st.listCount[li] = count;
        s_st.listLen[li] = n;
    }
}

static void *GetEvm(void *world)
{
    if (TrigFake()) return s_fakeEvm;
    void *evm = NULL;
    __try {
        if (!InImage(*(void **)world)) return NULL;
        evm = ((GetEvm_t)PC_GET_EVM)(world, NULL);
        if (evm && !InImage(*(void **)evm)) return NULL;
    } __except (CatchTr("getEventManager 0x007735E8", GetExceptionInformation())) {
        return NULL;
    }
    return evm;
}

static int Worlds(void **out)
{
    if (TrigFake()) {
        out[0] = s_fakeEvm;  // only a key for de-duplication; GetEvm gives the fake list
        return 1;
    }
    int n = 0;
    void *game = NULL, *actor = NULL, *w;
    if (!Game_GetGame(&game)) return 0;
    if (Game_GetActor(game, &actor) && actor) {
        __try {
            w = *(void **)((BYTE *)actor + OFS_ACTOR_WORLD);
            if (w) out[n++] = w;
        } __except (CatchTr("player world actor+0x58", GetExceptionInformation())) {
        }
    }
    __try {
        BYTE *ld = *(BYTE **)((BYTE *)game + OFS_GAME_LOADER);
        if (ld && *(int *)(ld + OFS_LOADER_STATE) == 4) {
            w = *(void **)(ld + OFS_LOADER_WORLD);
            if (w && (n == 0 || out[0] != w)) out[n++] = w;
        }
    } __except (CatchTr("current world game+0xBC", GetExceptionInformation())) {
    }
    return n;
}

static void LogStats(const char *why)
{
    Log("triggers: %s: worlds=%d EventManagers=%d nodes=%d items=%d (TriggerRegion %d, disabled %d) other objects "
        "skipped=%d bad nodes=%d broken lists=%d too long=%d over limit=%d | shape 0 AABB=%d 1 OBB=%d 2 sphere=%d "
        "3 capsule=%d other=%d",
        why, s_st.worlds, s_st.evms, s_st.nodes, s_numItems, s_st.trig, s_st.disabled, s_st.skipped, s_st.nodeEx,
        s_st.broken, s_st.longList, s_st.overflow, s_st.shape[0], s_st.shape[1], s_st.shape[2], s_st.shape[3],
        s_st.shape[4]);
    for (int i = 0; i < s_st.evms && i < 4; i++)
        Log("triggers:   world %08X EventManager %08X: list count field %d, walked %d nodes",
            (unsigned)(UINT_PTR)s_st.world[i], (unsigned)(UINT_PTR)s_st.evm[i], s_st.listCount[i], s_st.listLen[i]);
    for (int i = 0; i < s_numClasses; i++) {
        UINT_PTR vt = s_classes[i].vt;
        const char *name = "";
        if (!TrigFake()) {
            if (vt == PC_VT_TRIGGER || vt == PC_VT_TRIGGER2) name = "TriggerRegion";
            else if (vt == 0x01269E40u || vt == 0x0126A040u) name = "DetectorObject";
        } else {
            name = vt == (UINT_PTR)s_fakeVtTrig ? "fake TriggerRegion" : "fake detector";
        }
        Log("triggers:   class vt %08X: %d %s", (unsigned)vt, s_classes[i].n, name);
    }
}

static void CollectInner()
{
    bool wantClasses = s_logOnEnable;
    memset(&s_st, 0, sizeof(s_st));
    if (wantClasses) s_numClasses = 0;
    s_numItems = 0;
    if (TrigFake()) FakeBuild();
    if (s_testThrow) {
        s_testThrow = false;
        *(volatile int *)0 = 1;
    }
    void *worlds[4];
    int nw = Worlds(worlds);
    s_st.worlds = nw;
    void *done[4];
    int nd = 0;
    for (int i = 0; i < nw; i++) {
        void *evm = GetEvm(worlds[i]);
        if (!evm) continue;
        bool dup = false;
        for (int j = 0; j < nd; j++) dup |= done[j] == evm;
        if (dup) continue;
        done[nd] = evm;
        s_st.world[nd] = worlds[i];
        s_st.evm[nd] = evm;
        WalkList(nd, (BYTE *)evm, wantClasses);
        nd++;
    }
    s_st.evms = nd;
}

static void InfoFrame();
static void InfoOff();
static void InfoTurnOff(const char *why);
static void FakeInfoFields();

static void TurnOff(const char *why)
{
    g_showTriggers = false;
    Log("triggers: turned OFF: %s", why);
    Menu_Status("Show triggers: off (%s, see the log)", why);
}

void Trig_Frame()
{
    InterlockedIncrement(&s_frame);
    if (s_capture > 0)
        Log("triggers: timeline: ---- frame %s ----", InterlockedDecrement(&s_capture) == 1 ? "start" : "end");
    bool info = g_trigNearby || g_trigTarget;  // task 13: the info needs the list too (also without contours)
    if (!g_showTriggers && !info) {
        s_wasOn = false;
        InfoOff();
        return;
    }
    if (!g_gameMatch && !g_testMode) return;
    if (!s_wasOn) {
        s_wasOn = true;
        s_logOnEnable = true;
        s_viewLogged = false;
        s_statsTick = GetTickCount();
        s_collectMs = s_drawMs = s_collectMax = s_drawMax = 0;
        s_collectN = s_drawN = 0;
    }
    double t0 = NowMs();
    __try {
        CollectInner();
    } __except (CatchTr("trigger collection", GetExceptionInformation())) {
        s_numItems = 0;
        TurnOff("exception while collecting");
        InfoTurnOff("exception while collecting");
        return;
    }
    if (info) InfoFrame();
    else InfoOff();
    double dt = NowMs() - t0;
    s_lastCollectMs = dt;
    s_collectMs += dt;
    s_collectMax = dt > s_collectMax ? dt : s_collectMax;
    s_collectN++;
    if (s_logOnEnable) {
        s_logOnEnable = false;
        LogStats("on");
    }
    if (GetTickCount() - s_statsTick >= kStatsMs) {
        if (LogBudget(&s_statsBudget, 300)) {
            Log("triggers: %d items, %d drawn | collect %.3f ms avg (max %.3f) | draw %.3f ms avg (max %.3f) over %d/%d "
                "frames | mode %s%s | bad vertices %ld | other 0x00983AFC calls %ld",
                s_numItems, s_drawn, s_collectN ? s_collectMs / s_collectN : 0, s_collectMax,
                s_drawN ? s_drawMs / s_drawN : 0, s_drawMax, s_collectN, s_drawN,
                g_trigDepthMode == TD_DEPTH ? "depth" : "through walls", s_depthNA ? " (no depth buffer)" : "",
                s_badVtx, s_view.others);
            if (s_st.nodeEx || s_st.broken || s_st.longList || s_st.overflow) LogStats("problems");
        }
        s_statsTick = GetTickCount();
        s_collectMs = s_drawMs = s_collectMax = s_drawMax = 0;
        s_collectN = s_drawN = 0;
    }
}

void Trig_Toggle()
{
    g_showTriggers = !g_showTriggers;
    Menu_Status("Show triggers %s", g_showTriggers ? "on" : "off");
}

// ======================= view of the frame (hook of 0x00983AFC) =======================
void Trig_OnRenderView(const float *camMat, const float *proj, void *caller)
{
    if (s_capture > 0) Trig_D3DEvent(TE_VIEW, NULL, (DWORD)(UINT_PTR)caller, 0, NULL);
    if (!TrigFake() && (UINT_PTR)caller != PC_RV_MAIN_RET) {
        s_view.others++;
        return;
    }
    __try {
        float v[16];
        memcpy(s_view.cam, camMat, sizeof(s_view.cam));
        memcpy(s_view.proj, proj, sizeof(s_view.proj));
        ViewFromCam(s_view.cam, v);
        MatMul(v, s_view.proj, s_view.vp);
        s_view.frame = s_frame;
        s_view.ok = true;
    } __except (CatchTr("render view 0x00983AFC", GetExceptionInformation())) {
        s_view.ok = false;
    }
}

static bool ViewFresh() { return s_view.ok && s_view.frame == s_frame; }

// task 11: the same view for the monster labels (monsters.cpp)
bool Trig_View(float *vp, float *cam, float *proj)
{
    if (!ViewFresh()) return false;
    memcpy(vp, s_view.vp, sizeof(s_view.vp));
    memcpy(cam, s_view.cam, sizeof(s_view.cam));
    memcpy(proj, s_view.proj, sizeof(s_view.proj));
    return true;
}

// task 11: the loaded worlds (their ActorManager has the characters - monsters.cpp); real game only
int Trig_Worlds(void **out, int max)
{
    if (TrigFake() || max < 4) return 0;
    return Worlds(out);
}

// task 11 (v0.10 / v0.10.1, not used since v0.10.2): the EventManagers of the loaded world; real game only
int Trig_EventManagers(void **out, int max)
{
    if (TrigFake() || max <= 0) return 0;
    void *worlds[4];
    int nw = Worlds(worlds), n = 0;
    for (int i = 0; i < nw && n < max; i++) {
        void *evm = GetEvm(worlds[i]);
        if (!evm) continue;
        bool dup = false;
        for (int j = 0; j < n; j++) dup |= out[j] == evm;
        if (!dup) out[n++] = evm;
    }
    return n;
}

static void LogView(const char *where, UINT w, UINT h)
{
    const float *p = s_view.proj, *m = s_view.cam;
    float fov = p[5] != 0 ? 2.0f * atanf(1.0f / p[5]) * 57.29578f : 0, aspect = p[0] != 0 ? p[5] / p[0] : 0;
    float q = -p[10], zn = p[10] != 0 ? p[14] / p[10] : 0, zf = (q - 1) != 0 ? q * zn / (q - 1) : 0;
    Log("triggers: view (%s, target %ux%u): camera (%.1f %.1f %.1f) dir (%.3f %.3f %.3f) | projection fov %.2f deg, "
        "aspect %.4f, near %.3f, far %.1f, m[11]=%.3f",
        where, w, h, m[12], m[13], m[14], -m[8], -m[9], -m[10], fov, aspect, zn, zf, p[11]);
}

// ======================= segments: clip + project =======================
struct SV { float x, y, z, rhw; };
enum { EM_IMGUI, EM_D3D };
static int s_emit;
static ImDrawList *s_dl;
static float s_W, s_H;
static float s_zA, s_zB;  // z_clip = zA * vz + zB of the projection (depth bias)
struct LV { float x, y, z, rhw; DWORD col; };
static LV s_lv[kLineChunk * 2 * 2];
static int s_nlv;
static IDirect3DDevice9 *s_ddev;
static bool s_itemVisible;

static void FlushLines()
{
    if (s_nlv && s_ddev) s_ddev->DrawPrimitiveUP(D3DPT_LINELIST, s_nlv / 2, s_lv, sizeof(LV));
    s_nlv = 0;
}

static void PushLine(float x0, float y0, float z0, float r0, float x1, float y1, float z1, float r1, DWORD col)
{
    if (s_nlv + 2 > (int)(sizeof(s_lv) / sizeof(s_lv[0]))) FlushLines();
    LV *v = &s_lv[s_nlv];
    v[0].x = x0; v[0].y = y0; v[0].z = z0; v[0].rhw = r0; v[0].col = col;
    v[1].x = x1; v[1].y = y1; v[1].z = z1; v[1].rhw = r1; v[1].col = col;
    s_nlv += 2;
}

// clip-space segment (x, y, z, w each) -> clipped against near (z >= 0) and the 4 screen edges -> emitted
static void Segment(const float *a, const float *b, DWORD argb, ImU32 abgr, bool thick)
{
    float t0 = 0, t1 = 1;
    for (int p = 0; p < 5; p++) {
        float da, db;
        switch (p) {
        case 0: da = a[2]; db = b[2]; break;
        case 1: da = a[3] + a[0]; db = b[3] + b[0]; break;
        case 2: da = a[3] - a[0]; db = b[3] - b[0]; break;
        case 3: da = a[3] + a[1]; db = b[3] + b[1]; break;
        default: da = a[3] - a[1]; db = b[3] - b[1]; break;
        }
        if (da < 0 && db < 0) return;
        if (da < 0) {
            float t = da / (da - db);
            if (t > t0) t0 = t;
        } else if (db < 0) {
            float t = da / (da - db);
            if (t < t1) t1 = t;
        }
        if (t0 > t1) return;
    }
    float c0[4], c1[4];
    for (int i = 0; i < 4; i++) {
        c0[i] = a[i] + (b[i] - a[i]) * t0;
        c1[i] = a[i] + (b[i] - a[i]) * t1;
    }
    if (!(c0[3] > 1e-4f) || !(c1[3] > 1e-4f)) {  // cannot happen after the near clip (w >= near); count, skip
        InterlockedIncrement(&s_badVtx);
        return;
    }
    SV s[2];
    const float *c[2] = {c0, c1};
    for (int k = 0; k < 2; k++) {
        float iw = 1.0f / c[k][3];
        s[k].x = (c[k][0] * iw * 0.5f + 0.5f) * s_W;
        s[k].y = (0.5f - c[k][1] * iw * 0.5f) * s_H;
        s[k].rhw = iw;
        // depth with a small pull towards the camera (lines lying on floors/walls do not flicker)
        float d = c[k][3], d2 = d - (d * 0.003f > 2.0f ? d * 0.003f : 2.0f);
        if (d2 < d * 0.5f) d2 = d * 0.5f;
        float z = (-s_zA * d2 + s_zB) / d2;
        s[k].z = z < 0 ? 0 : (z > 0.99999f ? 0.99999f : z);
        if (!_finite(s[k].x) || !_finite(s[k].y) || fabsf(s[k].x) > 1e6f || fabsf(s[k].y) > 1e6f) {
            InterlockedIncrement(&s_badVtx);
            return;
        }
    }
    s_itemVisible = true;
    if (s_emit == EM_IMGUI) {
        s_dl->AddLine(ImVec2(s[0].x, s[0].y), ImVec2(s[1].x, s[1].y), abgr, thick ? 2.0f : 1.0f);
        return;
    }
    PushLine(s[0].x, s[0].y, s[0].z, s[0].rhw, s[1].x, s[1].y, s[1].z, s[1].rhw, argb);
    if (thick) {  // D3D lines are 1 px: a second one, 1 px aside
        bool horiz = fabsf(s[1].x - s[0].x) >= fabsf(s[1].y - s[0].y);
        float ox = horiz ? 0 : 1.0f, oy = horiz ? 1.0f : 0;
        PushLine(s[0].x + ox, s[0].y + oy, s[0].z, s[0].rhw, s[1].x + ox, s[1].y + oy, s[1].z, s[1].rhw, argb);
    }
}

static void ToClip(const float *p, float *c)
{
    const float *m = s_view.vp;
    for (int j = 0; j < 4; j++) c[j] = p[0] * m[0 * 4 + j] + p[1] * m[1 * 4 + j] + p[2] * m[2 * 4 + j] + m[3 * 4 + j];
}

// frustum planes (no far plane: no distance limit), from the columns of the view*projection matrix
static float s_planes[5][4];
static void BuildPlanes()
{
    const float *m = s_view.vp;
    for (int p = 0; p < 5; p++) {
        for (int i = 0; i < 4; i++) {
            float c0 = m[i * 4 + 0], c1 = m[i * 4 + 1], c2 = m[i * 4 + 2], c3 = m[i * 4 + 3];
            float v;
            switch (p) {
            case 0: v = c2; break;       // near (D3D: z >= 0)
            case 1: v = c3 + c0; break;  // left
            case 2: v = c3 - c0; break;  // right
            case 3: v = c3 + c1; break;  // bottom
            default: v = c3 - c1; break; // top
            }
            s_planes[p][i] = v;
        }
        float len = sqrtf(s_planes[p][0] * s_planes[p][0] + s_planes[p][1] * s_planes[p][1] + s_planes[p][2] * s_planes[p][2]);
        if (len > 0)
            for (int i = 0; i < 4; i++) s_planes[p][i] /= len;
    }
}

static bool SphereVisible(const float *c, float r)
{
    for (int p = 0; p < 5; p++)
        if (s_planes[p][0] * c[0] + s_planes[p][1] * c[1] + s_planes[p][2] * c[2] + s_planes[p][3] < -r) return false;
    return true;
}

// level of detail: vertical screen radius of the bounding sphere in px, -1 when the camera is near or inside it
static float ScreenRadius(const TItem &it, float *clip)
{
    ToClip(it.c, clip);
    if (clip[3] <= it.rad * 1.05f) return -1;
    return it.rad * fabsf(s_view.proj[5]) * 0.5f * s_H / clip[3];
}

static float s_circle[kCircleSeg][2];

static void DrawItem(const TItem &it, DWORD argb, ImU32 abgr, bool thick)
{
    float cc[4];
    float rpx = ScreenRadius(it, cc);
    if (rpx >= 0 && rpx < 1.5f) {  // a few pixels at most: a dot, not 12 lines on top of each other
        float b[4] = {cc[0] + 4.0f / s_W * cc[3], cc[1], cc[2], cc[3]};
        Segment(cc, b, argb, abgr, thick);
        return;
    }
    if (it.draw == SH_BOX) {
        float cl[8][4];
        for (int k = 0; k < 8; k++) {
            float sx = (k & 1) ? it.h[0] : -it.h[0], sy = (k & 2) ? it.h[1] : -it.h[1], sz = (k & 4) ? it.h[2] : -it.h[2];
            float p[3];
            for (int i = 0; i < 3; i++) p[i] = it.c[i] + sx * it.r[0 + i] + sy * it.r[3 + i] + sz * it.r[6 + i];
            ToClip(p, cl[k]);
        }
        static const BYTE e[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7},
                                      {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        for (int i = 0; i < 12; i++) Segment(cl[e[i][0]], cl[e[i][1]], argb, abgr, thick);
        return;
    }
    // sphere: three great circles (XY, XZ, YZ planes); 8..48 segments by the size on screen (~10 px each)
    if (!s_circle[1][1]) {
        for (int k = 0; k < kCircleSeg; k++) {
            s_circle[k][0] = cosf(k * (6.2831853f / kCircleSeg));
            s_circle[k][1] = sinf(k * (6.2831853f / kCircleSeg));
        }
    }
    int step = 1;
    if (rpx >= 0) {
        float n = rpx * 0.6f;
        step = n >= 48 ? 1 : (n >= 24 ? 2 : (n >= 16 ? 3 : (n >= 12 ? 4 : 6)));
    }
    for (int plane = 0; plane < 3; plane++) {
        float prev[4], first[4];
        for (int k = 0; k <= kCircleSeg; k += step) {
            float cur[4];
            if (k == kCircleSeg) {
                memcpy(cur, first, sizeof(cur));
            } else {
                float u = s_circle[k][0] * it.rad, v = s_circle[k][1] * it.rad;
                float p[3] = {it.c[0], it.c[1], it.c[2]};
                if (plane == 0) p[0] += u, p[1] += v;
                else if (plane == 1) p[0] += u, p[2] += v;
                else p[1] += u, p[2] += v;
                ToClip(p, cur);
                if (k == 0) memcpy(first, cur, sizeof(cur));
            }
            if (k > 0) Segment(prev, cur, argb, abgr, thick);
            memcpy(prev, cur, sizeof(prev));
        }
    }
}

static void DrawAll(float W, float H)
{
    s_W = W;
    s_H = H;
    s_zA = s_view.proj[10];
    s_zB = s_view.proj[14];
    BuildPlanes();
    int drawn = 0;
    for (int i = 0; i < s_numItems; i++) {
        const TItem &it = s_items[i];
        if (!it.enabled && !g_trigShowDisabled) continue;
        if (!SphereVisible(it.c, it.rad)) continue;
        DWORD argb;
        ImU32 abgr;
        if (!it.enabled) {
            argb = D3DCOLOR_ARGB(150, 128, 128, 128);
            abgr = IM_COL32(128, 128, 128, 150);
        } else if (it.trig) {
            argb = D3DCOLOR_ARGB(255, 255, 0, 255);
            abgr = IM_COL32(255, 0, 255, 255);
        } else {
            argb = D3DCOLOR_ARGB(255, 0, 255, 255);
            abgr = IM_COL32(0, 255, 255, 255);
        }
        s_itemVisible = false;
        DrawItem(it, argb, abgr, it.enabled != 0);
        if (s_itemVisible) drawn++;
    }
    s_drawn = drawn;
}

// ======================= through walls: ImGui (overlay frame, inside ImGui::NewFrame/Render) =======================
static int DrawFilter(const char *where, EXCEPTION_POINTERS *ep)
{
    CatchTr(where, ep);
    return EXCEPTION_EXECUTE_HANDLER;
}

void Trig_Draw2D()
{
    if (!g_showTriggers) return;
    if (!ViewFresh()) {
        s_drawn = 0;
        return;
    }
    if (g_trigDepthMode == TD_DEPTH && s_depthDrawnFrame == s_frame) return;  // already in the 3D scene
    ImGuiIO &io = ImGui::GetIO();
    if (!s_viewLogged) {
        s_viewLogged = true;
        LogView("through walls, ImGui", (UINT)io.DisplaySize.x, (UINT)io.DisplaySize.y);
    }
    double t0 = NowMs();
    __try {
        s_emit = EM_IMGUI;
        s_dl = ImGui::GetBackgroundDrawList();
        DrawAll(io.DisplaySize.x, io.DisplaySize.y);
    } __except (DrawFilter("trigger drawing (ImGui)", GetExceptionInformation())) {
        TurnOff("exception while drawing");
    }
    double dt = NowMs() - t0;
    s_lastDrawMs = dt;
    s_drawMs += dt;
    s_drawMax = dt > s_drawMax ? dt : s_drawMax;
    s_drawN++;
}

// ======================= depth: D3D lines with the z-test of the game's depth buffer =======================
static void SurfDesc(IDirect3DSurface9 *s, char *buf, int size)
{
    D3DSURFACE_DESC d;
    if (!s || FAILED(s->GetDesc(&d))) {
        _snprintf_s(buf, size, _TRUNCATE, "none");
        return;
    }
    _snprintf_s(buf, size, _TRUNCATE, "%08X %ux%u fmt=%d ms=%d usage=%lX pool=%d", (unsigned)(UINT_PTR)s, d.Width,
                d.Height, d.Format, d.MultiSampleType, d.Usage, d.Pool);
}

// returns true if the lines were drawn with the depth test
static bool DepthInner(IDirect3DDevice9 *dev, bool atPresent)
{
    IDirect3DSurface9 *rt = NULL, *ds = NULL, *bb = NULL, *oldRT = NULL;
    bool ok = false;
    D3DSURFACE_DESC rd = {}, dd = {};
    if (atPresent) {
        if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return false;
        dev->GetRenderTarget(0, &oldRT);
        dev->SetRenderTarget(0, bb);
    }
    dev->GetRenderTarget(0, &rt);
    dev->GetDepthStencilSurface(&ds);
    bool zOk = rt && ds && SUCCEEDED(rt->GetDesc(&rd)) && SUCCEEDED(ds->GetDesc(&dd)) && dd.Width >= rd.Width &&
               dd.Height >= rd.Height && dd.MultiSampleType == rd.MultiSampleType;
    static IDirect3DSurface9 *lastRT, *lastDS;
    static bool lastAt, lastOk, any;
    if (!any || rt != lastRT || ds != lastDS || atPresent != lastAt || zOk != lastOk) {
        static volatile LONG budget;
        if (LogBudget(&budget, 40)) {
            char a[96], b[96];
            SurfDesc(rt, a, sizeof(a));
            SurfDesc(ds, b, sizeof(b));
            D3DVIEWPORT9 vp = {};
            dev->GetViewport(&vp);
            Log("triggers: depth point %s: render target %s | depth %s | viewport %lu,%lu %lux%lu z %.2f..%.2f -> %s",
                atPresent ? "Present (fallback)" : "HUD pass 0x00B3B8D2", a, b, vp.X, vp.Y, vp.Width, vp.Height,
                vp.MinZ, vp.MaxZ, zOk ? "depth test" : "NO usable depth buffer -> through walls");
            if (zOk) LogView(atPresent ? "depth, Present" : "depth, HUD pass", rd.Width, rd.Height);
        }
        any = true;
        lastRT = rt; lastDS = ds; lastAt = atPresent; lastOk = zOk;
    }
    if (zOk) {
        IDirect3DStateBlock9 *sb = NULL;
        D3DVIEWPORT9 full = {0, 0, rd.Width, rd.Height, 0.0f, 1.0f};
        dev->CreateStateBlock(D3DSBT_ALL, &sb);
        dev->SetViewport(&full);
        dev->SetVertexShader(NULL);
        dev->SetPixelShader(NULL);
        dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
        dev->SetTexture(0, NULL);
        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_LIGHTING, FALSE);
        dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
        dev->SetRenderState(D3DRS_ANTIALIASEDLINEENABLE, FALSE);
        dev->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, FALSE);
        dev->SetRenderState(D3DRS_CLIPPING, TRUE);
        bool began = SUCCEEDED(dev->BeginScene());  // fails if the game is inside its scene: then draw into that
        s_emit = EM_D3D;
        s_ddev = dev;
        s_nlv = 0;
        DrawAll((float)rd.Width, (float)rd.Height);
        FlushLines();
        s_ddev = NULL;
        if (began) dev->EndScene();
        if (sb) {
            sb->Apply();
            sb->Release();
        }
        ok = true;
    }
    if (atPresent) {
        dev->SetRenderTarget(0, oldRT);
        if (oldRT) oldRT->Release();
        bb->Release();
    }
    if (rt) rt->Release();
    if (ds) ds->Release();
    return ok;
}

static void DrawDepth(IDirect3DDevice9 *dev, bool atPresent)
{
    if (!dev || !g_showTriggers || g_trigDepthMode != TD_DEPTH || !ViewFresh() || s_depthDrawnFrame == s_frame) return;
    if (dev->TestCooperativeLevel() != D3D_OK) return;
    double t0 = NowMs();
    bool ok = false;
    t_ours++;
    __try {
        ok = DepthInner(dev, atPresent);
    } __except (DrawFilter("trigger drawing (depth)", GetExceptionInformation())) {
        TurnOff("exception while drawing with depth");
    }
    t_ours--;
    s_depthNA = !ok;
    if (ok) {
        s_depthDrawnFrame = s_frame;
        double dt = NowMs() - t0;
        s_lastDrawMs = dt;
        s_drawMs += dt;
        s_drawMax = dt > s_drawMax ? dt : s_drawMax;
        s_drawN++;
    }
}

void Trig_OnHud()
{
    s_hudFrame = s_frame;
    if (s_capture > 0) Trig_D3DEvent(TE_HUD, NULL, 0, 0, NULL);
    DrawDepth(Overlay_MainDevice(), false);
}

void Trig_OnPresent(IDirect3DDevice9 *dev)
{
    if (s_capture > 0) Trig_D3DEvent(TE_PRESENT, dev, 0, 0, NULL);
    if (dev != Overlay_MainDevice()) return;
    DrawDepth(dev, true);  // only if the HUD point did not draw this frame
}

// ======================= RT timeline (one frame, when depth mode is switched on) =======================
void Trig_StartCapture()
{
    static volatile LONG budget;
    if (!LogBudget(&budget, 3)) return;
    s_capEvents = 0;
    Log("triggers: capturing one frame of render target / depth / Clear calls (game thread, from the next frame)");
    InterlockedExchange(&s_capture, 2);  // Trig_Frame: 2 -> 1 starts the frame, 1 -> 0 ends it
}

void Trig_D3DEvent(int type, IDirect3DDevice9 *dev, DWORD a, DWORD b, void *p)
{
    if (t_ours || s_capture != 1 || ++s_capEvents > 300) return;
    char d[96] = "";
    switch (type) {
    case TE_SETRT:
        SurfDesc((IDirect3DSurface9 *)p, d, sizeof(d));
        Log("triggers: timeline: SetRenderTarget(%lu, %s) from %08X", a, d, b);
        break;
    case TE_SETDS:
        SurfDesc((IDirect3DSurface9 *)p, d, sizeof(d));
        Log("triggers: timeline: SetDepthStencilSurface(%s) from %08X", d, b);
        break;
    case TE_CLEAR:
        Log("triggers: timeline: Clear(flags %lX) from %08X", a, b);
        break;
    case TE_VIEW: Log("triggers: timeline: render view 0x00983AFC from %08X", a); break;
    case TE_HUD: Log("triggers: timeline: HUD pass 0x00B3B8D2 (depth draw point)"); break;
    case TE_PRESENT: Log("triggers: timeline: Present"); break;
    }
    (void)dev;
}

// ======================= status line =======================
bool Trig_StatusShown() { return g_showTriggers || g_trigNearby || g_trigTarget; }
static void InfoStatus();

void Trig_DrawStatus()
{
    if (!Trig_StatusShown()) return;
    float fs = ImGui::GetFontSize();
    ImGuiIO &io = ImGui::GetIO();
    // bottom left, above the playerFly / hover line (bottom centre)
    ImGui::SetNextWindowPos(ImVec2(fs * 1.5f, io.DisplaySize.y - fs * 3.6f), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::Begin("##dbgtrig", NULL,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::SetWindowFontScale(0.8f);
    if (g_showTriggers) {
        ImGui::Text("triggers: %d drawn / %d total", ViewFresh() ? s_drawn : 0, s_numItems);
        ImGui::SameLine();
        if (g_trigDepthMode == TD_DEPTH) ImGui::TextDisabled(s_depthNA ? "[depth: n/a, through walls]" : "[depth]");
        else ImGui::TextDisabled("[through walls]");
    } else {
        ImGui::Text("triggers: %d total", s_numItems);
        ImGui::SameLine();
        ImGui::TextDisabled("[contours off]");
    }
    InfoStatus();
    ImGui::End();
}

// ======================= task 09: "trigger fired" for the player (log only) =======================
// Every detector (DetectorObject::onAdd 0x008B84FE, TriggerRegion too: its onAdd 0x008C8E47 jumps there) gets a
// Havok phantom body +0xD4 (vt 0x0126A020; base 0x008D69C1, AABB phantom / shape phantom). The body keeps the
// overlapping objects: +0x24 the detector, +0x28 objects {data, count +0x2C}, +0x34 per object collidable count.
//   vt[0x14] 0x008B82DD add(collidable) (ret 4): obj = collidable+0x3C -> +0xC (0x00A112FF), WorldObject check
//     0x00584325; a new object -> list, count 1, and if the detector is enabled (+0xD8) -> detector->vt[0x130](obj)
//     ("onEnter" for DetectorObject 0x008B81E7, own handlers in TriggerRegion 0x008C77F9 and others); else count++.
//   vt[0x18] 0x008B815A remove(?, obj) (ret 8): count-- ; at 0, if enabled -> detector->vt[0x134](obj) ("onExit").
// Our hooks run before the original and only read: does this call make the player (vt 0x01266568) enter / leave?
#define PC_VT_PLAYER_DET   0x01266568u
#define OFS_BODY_DETECTOR  0x24
#define OFS_BODY_OBJS      0x28
#define OFS_BODY_NOBJS     0x2C
#define OFS_BODY_COUNTS    0x34
static volatile LONG s_detLogBudget;
static LONG s_detEnter, s_detExit;

// task 13: the player's enter / exit count and times per detector (state line of the trigger info)
struct DetStat { void *det; int enter, exit; DWORD lastEnter, lastExit; };
static DetStat s_detStat[1024];
static DetStat *DetStatOf(void *d, bool add)
{
    unsigned h = ((unsigned)(UINT_PTR)d >> 4) & 1023;
    for (int i = 0; i < 32; i++) {
        DetStat &e = s_detStat[(h + i) & 1023];
        if (e.det == d) return &e;
        if (!e.det) {
            if (!add) return NULL;
            memset(&e, 0, sizeof(e));
            e.det = d;
            return &e;
        }
    }
    if (!add) return NULL;
    DetStat &e = s_detStat[h];  // full around h: reuse
    memset(&e, 0, sizeof(e));
    e.det = d;
    return &e;
}
static void DetCount(void *d, bool enter)
{
    DetStat *e = d ? DetStatOf(d, true) : NULL;
    if (!e) return;
    if (enter) e->enter++, e->lastEnter = GetTickCount() | 1;
    else e->exit++, e->lastExit = GetTickCount() | 1;
}

static UINT_PTR PlayerVt()
{
    return TrigFake() ? (UINT_PTR)s_fakeVtActor : PC_VT_PLAYER_DET;
}

// index of the player in the body's object list, -1 = not there
static int PlayerIndex(BYTE *body, void *player)
{
    int n = *(int *)(body + OFS_BODY_NOBJS);
    void **objs = *(void ***)(body + OFS_BODY_OBJS);
    if (n <= 0 || n > 4096 || !objs) return -1;
    for (int i = 0; i < n; i++)
        if (objs[i] == player) return i;
    return -1;
}

static void DetectorLog(BYTE *body, const char *what)
{
    BYTE *d = *(BYTE **)(body + OFS_BODY_DETECTOR);
    if (!d) return;
    UINT_PTR vt = *(UINT_PTR *)d;
    bool enabled = d[OFS_OBJ_ENABLED] != 0;
    const char *cls = IsTriggerRegion(vt) ? "TriggerRegion" : "detector";
    float *p = (float *)(d + OFS_OBJ_POS), *s = (float *)(d + OFS_OBJ_SCALE);
    if (!LogBudget(&s_detLogBudget, 600)) return;
    Log("trigger fired: %s %s vt=%08X det=%08X shape=%d at (%.0f %.0f %.0f) size (%.0f %.0f %.0f)%s | player %s",
        what, cls, (unsigned)vt, (unsigned)(UINT_PTR)d, *(int *)(d + OFS_OBJ_SHAPE), p[0], p[1], p[2], s[0], s[1], s[2],
        enabled ? "" : " - DISABLED, no event", !g_flying ? "walking" : (Flight_Attached() ? "carried (flight)" : "in flight"));
}

void Trig_OnDetectorAdd(void *body, void *collidable)
{
    __try {
        BYTE *wrap = *(BYTE **)((BYTE *)collidable + 0x3C);  // 0x00A112FF
        void *obj = wrap ? *(void **)(wrap + 0xC) : NULL;
        if (!obj || *(UINT_PTR *)obj != PlayerVt()) return;
        if (PlayerIndex((BYTE *)body, obj) >= 0) return;  // another collidable of the player: count++ only
        s_detEnter++;
        DetCount(*(void **)((BYTE *)body + OFS_BODY_DETECTOR), true);
        DetectorLog((BYTE *)body, "ENTER");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void Trig_OnDetectorRemove(void *body, void *obj)
{
    __try {
        if (!obj || *(UINT_PTR *)obj != PlayerVt()) return;
        int i = PlayerIndex((BYTE *)body, obj);
        int *counts = *(int **)((BYTE *)body + OFS_BODY_COUNTS);
        if (i < 0 || !counts || counts[i] != 1) return;  // still overlapping with another collidable
        s_detExit++;
        DetCount(*(void **)((BYTE *)body + OFS_BODY_DETECTOR), false);
        DetectorLog((BYTE *)body, "EXIT ");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// test 15: a fake detector body; the player enters by two collidables, another object enters, the player leaves
static void TestDetectorEvents()
{
    static BYTE det[0xE0], player[16], other[16], wrapP[16], wrapO[16], colP1[0x40], colP2[0x40], colO[0x40];
    static BYTE body[0x40];
    static void *objs[4];
    static int counts[4];
    *(void **)det = s_fakeVtTrig;
    det[OFS_OBJ_ENABLED] = 1;
    float pos[3] = {700, 200, 300}, sc[3] = {100, 50, 200};
    memcpy(det + OFS_OBJ_POS, pos, sizeof(pos));
    memcpy(det + OFS_OBJ_SCALE, sc, sizeof(sc));
    *(void **)player = s_fakeVtActor;
    *(void **)other = s_fakeVtDet;
    *(void **)(wrapP + 0xC) = player;
    *(void **)(wrapO + 0xC) = other;
    *(void **)(colP1 + 0x3C) = wrapP;
    *(void **)(colP2 + 0x3C) = wrapP;
    *(void **)(colO + 0x3C) = wrapO;
    *(void **)(body + OFS_BODY_DETECTOR) = det;
    *(void ***)(body + OFS_BODY_OBJS) = objs;
    *(int **)(body + OFS_BODY_COUNTS) = counts;
    int &n = *(int *)(body + OFS_BODY_NOBJS);
    n = 0;
    LONG e0 = s_detEnter, x0 = s_detExit;
    // what the game's add / remove do, after our hook looked
    Trig_OnDetectorAdd(body, colP1); objs[n] = player; counts[n++] = 1;  // ENTER expected
    Trig_OnDetectorAdd(body, colP2); counts[0]++;                       // second collidable: nothing
    Trig_OnDetectorAdd(body, colO); objs[n] = other; counts[n++] = 1;   // not the player: nothing
    Trig_OnDetectorRemove(body, player); counts[0]--;                   // one collidable left: nothing
    Trig_OnDetectorRemove(body, other); n--;                            // not the player: nothing (list: player)
    Trig_OnDetectorRemove(body, player);                                // EXIT expected
    Log("test: detector events: enter %ld (expected 1), exit %ld (expected 1)", s_detEnter - e0, s_detExit - x0);
}

// ======================= task 13: trigger info (like Monsters, notes/triggers_pc.md §7) =======================
bool g_trigNearby, g_trigTarget;
int g_trigRange = 2;                               // 30 m
bool g_trigShow[TF_COUNT] = {true, true, true};    // name, class, leads to

#define TSPRINTF(buf, ...) _snprintf_s(buf, sizeof(buf), _TRUNCATE, __VA_ARGS__)
#define TSTRCPY(buf, src) strncpy_s(buf, sizeof(buf), src, _TRUNCATE)
#define TSTRCAT(buf, src) strncat_s(buf, sizeof(buf), src, _TRUNCATE)
#define OFS_OBJ_NAME_T     0x50
#define OFS_OBJ_LISTENERS  0x64   // WorldObject: {+0 ?, +4 listener, +8 next, +0xC off} (0x00A7DE70, sent by 0x00676F02)
#define OFS_OBJ_BODY       0xD4
#define OFS_TR_DONE_ENTER  0x120  // TriggerRegion: onEnter / onExit done once (0x008C77F9 / 0x008C7861, OneShot)
#define OFS_TR_DONE_EXIT   0x121
#define PC_VT_CLASS_SCRIPT 0x01166A08u  // descriptor of a scripted class

static const float kTRangeM[] = {10, 20, 30, 50, 75, 100, 150, 0};  // 0 = unlimited
static const float kTUnitsPerM = 100.0f;                              // 1 unit = 1 cm (monsters_pc.md)
static const int kTMaxLabels = 48, kInfoSlots = 64, kRefreshFrames = 15, kAllLines = 96;
static const ImU32 kTgtCol = IM_COL32(255, 220, 0, 255);

struct TInfo {
    void *obj;
    LONG read, used;            // frame of the last full reading / of the last use
    bool trigRegion, inside, enabled, scripted;
    char name[64], cls[48], chain[160];
    char dest[400], destShort[96];
    char events[480], eventsShort[96];
    char filter[240], flags[200], state[200];
    int nProps, nUnread;
};
static TInfo s_info[kInfoSlots];
static struct { int item; TInfo *info; float dist; } s_lab[kTMaxLabels];
static int s_numLab, s_inRange, s_labDrawn, s_labSkipped;
static void *s_tObj;                // the target (its object; compared only, never read outside Trig_Frame)
static int s_tItem = -1;
static TInfo *s_tInfo;
static char s_tHow[48];
static float s_tDist;               // camera -> target (units, the ray hit or the centre)
static char s_tAll[kAllLines][176];
static int s_tAllN, s_tAllTotal;
static LONG s_tAllRead = -1000;
static float s_ref[3], s_cam[3];
static bool s_refOk, s_refCam, s_camOk;
static void *s_player;
static bool s_infoWasOn;
static volatile LONG s_tLogBudget, s_infoExBudget, s_infoStatsBudget;
static bool s_infoThrow;
static double s_infoMs, s_infoMax;
static int s_infoN;

void Trig_ShowAll(bool on)
{
    for (int i = 0; i < TF_COUNT; i++) g_trigShow[i] = on;
}

void Trig_ToggleTarget()
{
    g_trigTarget = !g_trigTarget;
    Menu_Status("Triggers > info: target %s", g_trigTarget ? "on" : "off");
}

void Trig_OnDebugOff()
{
    g_trigNearby = g_trigTarget = false;
    InfoOff();
}

static void InfoOff()
{
    if (!s_infoWasOn) return;
    s_infoWasOn = false;
    s_numLab = s_inRange = 0;
    s_tObj = NULL;
    s_tItem = -1;
    s_tInfo = NULL;
    for (int i = 0; i < kInfoSlots; i++) s_info[i].obj = NULL;
    Log("triggers: info OFF");
}

static void InfoTurnOff(const char *why)
{
    if (!g_trigNearby && !g_trigTarget) return;
    g_trigNearby = g_trigTarget = false;
    InfoOff();
    Log("triggers: info turned OFF: %s", why);
    Menu_Status("Triggers info: off (%s, see the log)", why);
}

static const char *TShowText()
{
    static const char *const kNames[TF_COUNT] = {"name", "class", "leads-to", "events", "filter", "flags", "state",
                                                 "shape", "pos", "distance", "all"};
    static char buf[128];
    buf[0] = 0;
    for (int i = 0; i < TF_COUNT; i++)
        if (g_trigShow[i]) {
            if (buf[0]) TSTRCAT(buf, " ");
            TSTRCAT(buf, kNames[i]);
        }
    if (!buf[0]) TSTRCPY(buf, "none");
    return buf;
}

// ---- geometry ----
static float Dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// distance from p to the volume (0 inside)
static float DistTo(const TItem &it, const float *p)
{
    float d[3] = {p[0] - it.c[0], p[1] - it.c[1], p[2] - it.c[2]};
    if (it.draw == SH_SPHERE) {
        float l = sqrtf(Dot3(d, d)) - it.rad;
        return l > 0 ? l : 0;
    }
    float s = 0;
    for (int i = 0; i < 3; i++) {
        float q = fabsf(Dot3(d, &it.r[3 * i])) - it.h[i];
        if (q > 0) s += q * q;
    }
    return sqrtf(s);
}

// the ray o + t*dir enters the volume at t (>= 0; 0 = o is inside); false = missed
static bool RayHit(const TItem &it, const float *o, const float *dir, float *t, bool *inside)
{
    float d[3] = {o[0] - it.c[0], o[1] - it.c[1], o[2] - it.c[2]};
    if (it.draw == SH_SPHERE) {
        float b = Dot3(d, dir), c = Dot3(d, d) - it.rad * it.rad, disc = b * b - c;
        if (c <= 0) { *t = 0; *inside = true; return true; }
        if (disc < 0 || b > 0) return false;
        *t = -b - sqrtf(disc);
        *inside = false;
        return true;
    }
    float t0 = -1e30f, t1 = 1e30f;
    for (int i = 0; i < 3; i++) {
        float oo = Dot3(d, &it.r[3 * i]), dd = Dot3(dir, &it.r[3 * i]), h = it.h[i];
        if (fabsf(dd) < 1e-6f) {
            if (oo < -h || oo > h) return false;
            continue;
        }
        float a = (-h - oo) / dd, b = (h - oo) / dd;
        if (a > b) { float x = a; a = b; b = x; }
        if (a > t0) t0 = a;
        if (b < t1) t1 = b;
        if (t0 > t1) return false;
    }
    if (t1 < 0) return false;
    *inside = t0 < 0;
    *t = t0 < 0 ? 0 : t0;
    return true;
}

static float Volume(const TItem &it)
{
    return it.draw == SH_SPHERE ? 4.19f * it.rad * it.rad * it.rad : 8.0f * it.h[0] * it.h[1] * it.h[2];
}

// the point the label stands on: the centre of the top face (the highest point of the volume over its centre)
static void LabelPoint(const TItem &it, float *p)
{
    p[0] = it.c[0];
    p[1] = it.c[1];
    if (it.draw == SH_SPHERE) p[2] = it.c[2] + it.rad;
    else p[2] = it.c[2] + fabsf(it.r[2]) * it.h[0] + fabsf(it.r[5]) * it.h[1] + fabsf(it.r[8]) * it.h[2];
}

// ---- the fields of one trigger (game thread, inside Trig_Frame: the object is alive) ----
struct PV { char cls[40], prop[40]; int type; char val[200]; bool ok; };
static PV s_pv[192];
static int s_npv;
static void PVCollect(void *, const char *cls, const char *prop, int type, const char *val)
{
    if (s_npv >= 192) return;
    PV &v = s_pv[s_npv++];
    TSTRCPY(v.cls, cls);
    TSTRCPY(v.prop, prop);
    v.type = type;
    v.ok = val != NULL;
    TSTRCPY(v.val, val ? val : "");
}
static const PV *PVGet(const char *prop)
{
    for (int i = 0; i < s_npv; i++)
        if (!strcmp(s_pv[i].prop, prop) && s_pv[i].ok) return &s_pv[i];
    return NULL;
}
// a value worth showing: not empty, not "" / [0] / none
static const char *PVText(const char *prop)
{
    const PV *v = PVGet(prop);
    if (!v || !v->val[0] || !strcmp(v->val, "\"\"") || !strcmp(v->val, "[0]") || !strcmp(v->val, "none")) return NULL;
    return v->val;
}
// "[2] a, b" -> "a, b"
static const char *ArrItems(const char *v)
{
    if (v && v[0] == '[') {
        const char *p = strchr(v, ']');
        if (p) return p[1] == ' ' ? p + 2 : p + 1;
    }
    return v;
}

static void Short(char *dst, int size, const char *src, int max)
{
    int n = (int)strlen(src);
    if (n <= max) {
        strncpy_s(dst, size, src, _TRUNCATE);
        return;
    }
    _snprintf_s(dst, size, _TRUNCATE, "%.*s...", max - 3, src);
}

// LoadOnEnter / UnloadOnEnter / UnloadOnExit: {region id, layer} -> "Z2_Hub, Z2_Forge (layer 1)"
static void RegionList(void *obj, const char *prop, char *out, int size)
{
    out[0] = 0;
    DWORD d[64];
    int per = 0, n = Refl_GetArrayDwords(obj, prop, d, 64, &per);
    if (n <= 0 || per < 1) return;
    for (int i = 0; i < n && (i + 1) * per <= 64; i++) {
        int id = (int)d[i * per], layer = per >= 2 ? (int)d[i * per + 1] : -1;
        char one[80];
        const char *nm = id >= 0 ? World_RegionName(id) : NULL;
        if (id < 0) _snprintf_s(one, sizeof(one), _TRUNCATE, "all regions");
        else if (nm) _snprintf_s(one, sizeof(one), _TRUNCATE, "%s", nm);
        else _snprintf_s(one, sizeof(one), _TRUNCATE, "region %d", id);
        if (layer >= 0) {
            char l[24];
            _snprintf_s(l, sizeof(l), _TRUNCATE, " (layer %d)", layer);
            strncat_s(one, sizeof(one), l, _TRUNCATE);
        }
        if (i) strncat_s(out, size, ", ", _TRUNCATE);
        strncat_s(out, size, one, _TRUNCATE);
    }
    if (n * per > 64) strncat_s(out, size, ", ...", _TRUNCATE);
}

// OnEnterList item "Object:Event" (no ':' -> the event Execute, 0x008C8E8C) -> "Object.Event"
static void EventItems(const char *v, char *out, int size)
{
    out[0] = 0;
    v = ArrItems(v);
    if (!v) return;
    char buf[200];
    strncpy_s(buf, sizeof(buf), v, _TRUNCATE);
    char *ctx = NULL;
    int n = 0;
    for (char *tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx)) {
        while (*tok == ' ') tok++;
        if (!strcmp(tok, "...")) {
            strncat_s(out, size, ", ...", _TRUNCATE);
            break;
        }
        char one[96];
        char *c = strchr(tok, ':');
        if (c) *c = 0, _snprintf_s(one, sizeof(one), _TRUNCATE, "%s.%s", tok, c + 1);
        else _snprintf_s(one, sizeof(one), _TRUNCATE, "%s.Execute", tok);
        if (n++) strncat_s(out, size, ", ", _TRUNCATE);
        strncat_s(out, size, one, _TRUNCATE);
    }
}

// the listeners of a world object (+0x64): script event handlers, components
static int Listeners(void *obj, char *out, int size)
{
    out[0] = 0;
    int n = 0;
    BYTE *node = *(BYTE **)((BYTE *)obj + OFS_OBJ_LISTENERS);
    for (int k = 0; node && k < 16; k++) {
        void *l = *(void **)(node + 4);
        if (!l) l = *(void **)node;
        bool off = node[0xC] != 0;
        char one[120] = "?";
        if (l) {
            void *c = Refl_ClassOf(l);
            const char *nm = c ? Refl_ObjName(l) : "";
            if (c) _snprintf_s(one, sizeof(one), _TRUNCATE, "%s%s%s%s%s", Refl_ClassName(c), nm[0] ? " '" : "", nm,
                               nm[0] ? "'" : "", off ? " (off)" : "");
            else _snprintf_s(one, sizeof(one), _TRUNCATE, "object %08X", (unsigned)(UINT_PTR)l);
        }
        if (n++) strncat_s(out, size, ", ", _TRUNCATE);
        strncat_s(out, size, one, _TRUNCATE);
        node = *(BYTE **)(node + 8);
    }
    return n;
}

static const char *const kDestProps[] = {"WorldFile", "Region", "StartPoint", "StartLoadRegion", "Waypoint",
    "SpawnRegion", "SpawnPoint", "SpawnLoadRegion", "Level", "Room", "PortalCameraWaypoint", "OrientationWaypoint",
    "Target", "TargetName", "Destination", "TeleportDestination", "TeleportLocation", "HUDText", "MapTileClass"};
static const char *const kFlagProps[] = {"OneShot", "RequireInWorld", "Exclude", "IncludeGrabbables", "AutoPersist",
    "Required", "DoAutoSave", "DoStoreData", "DoCommitData", "ActivePortalsOnly", "DeterRuin", "AutoCameraRotate"};

static void ReadInfo(TInfo &ti, void *obj)
{
    ti.read = s_frame;
    TSTRCPY(ti.name, Refl_ObjName(obj));
    void *cls = Refl_ClassOf(obj);
    TSTRCPY(ti.cls, cls ? Refl_ClassName(cls) : "");
    Refl_ClassChain(obj, ti.chain, sizeof(ti.chain), 6);
    ti.scripted = cls && !TrigFake() && *(UINT_PTR *)cls == PC_VT_CLASS_SCRIPT;
    ti.trigRegion = Refl_IsKindOf(obj, "TriggerRegion");
    s_npv = 0;
    Refl_Visit(obj, PVCollect, NULL);
    ti.nProps = s_npv;
    ti.nUnread = 0;
    for (int i = 0; i < s_npv; i++) ti.nUnread += !s_pv[i].ok;
    // leads to: destination-like properties, then the regions it loads / unloads
    ti.dest[0] = 0;
    for (int i = 0; i < (int)(sizeof(kDestProps) / sizeof(kDestProps[0])); i++) {
        const char *v = PVText(kDestProps[i]);
        if (!v) continue;
        char one[160];
        TSPRINTF(one, "%s%s %s", ti.dest[0] ? "; " : "", kDestProps[i], v);
        TSTRCAT(ti.dest, one);
    }
    static const char *const kLoads[3][2] = {{"LoadOnEnter", "loads"}, {"UnloadOnEnter", "unloads"},
                                             {"UnloadOnExit", "unloads on exit"}};
    for (int i = 0; i < 3; i++) {
        char list[320];
        RegionList(obj, kLoads[i][0], list, sizeof(list));
        if (!list[0]) continue;
        char one[360];
        TSPRINTF(one, "%s%s %s", ti.dest[0] ? "; " : "", kLoads[i][1], list);
        TSTRCAT(ti.dest, one);
    }
    if (const char *v = PVText("ConditionalLoads")) {
        char one[80];
        TSPRINTF(one, "%sconditional loads %s", ti.dest[0] ? "; " : "", v);
        TSTRCAT(ti.dest, one);
    }
    Short(ti.destShort, sizeof(ti.destShort), ti.dest[0] ? ti.dest : "-", 70);
    // events: OnEnterList / OnExitList, the listeners, the script class
    char en[200] = "", ex[200] = "", ls[320] = "";
    EventItems(PVText("OnEnterList"), en, sizeof(en));
    EventItems(PVText("OnExitList"), ex, sizeof(ex));
    int nl = 0;
    __try {
        nl = Listeners(obj, ls, sizeof(ls));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        TSTRCPY(ls, "(unreadable)");
    }
    ti.events[0] = 0;
    if (en[0]) TSPRINTF(ti.events, "onEnter -> %s", en);
    if (ex[0]) {
        char one[240];
        TSPRINTF(one, "%sonExit -> %s", ti.events[0] ? "; " : "", ex);
        TSTRCAT(ti.events, one);
    }
    if (nl) {
        char one[360];
        TSPRINTF(one, "%slisteners: %s", ti.events[0] ? "; " : "", ls);
        TSTRCAT(ti.events, one);
    }
    if (ti.scripted) {
        char one[96];
        TSPRINTF(one, "%sscript class %s (its onEnter / onExit)", ti.events[0] ? "; " : "", ti.cls);
        TSTRCAT(ti.events, one);
    }
    int ne = 0;
    for (const char *p = ArrItems(PVText("OnEnterList")); p && *p; p++) ne += *p == ',';
    if (PVText("OnEnterList")) ne++;
    int nx = 0;
    for (const char *p = ArrItems(PVText("OnExitList")); p && *p; p++) nx += *p == ',';
    if (PVText("OnExitList")) nx++;
    if (ne || nx || nl) TSPRINTF(ti.eventsShort, "onEnter %d, onExit %d, listeners %d", ne, nx, nl);
    else TSTRCPY(ti.eventsShort, ti.scripted ? "script class" : "-");
    // who triggers it
    const char *fl = PVText("FilterList"), *df = PVText("DefaultFilterList"), *nf = PVText("NameFilter");
    ti.filter[0] = 0;
    if (fl) TSPRINTF(ti.filter, "%s", ArrItems(fl));
    else if (df) TSPRINTF(ti.filter, "%s (default)", ArrItems(df));
    if (nf) {
        char one[160];
        TSPRINTF(one, "%snames: %s", ti.filter[0] ? "; " : "", ArrItems(nf));
        TSTRCAT(ti.filter, one);
    }
    const PV *exc = PVGet("Exclude");
    if (exc && !strcmp(exc->val, "true")) TSTRCAT(ti.filter, " (exclude: everyone else)");
    // flags (true ones; OneShot also when false - it says "every time")
    ti.flags[0] = 0;
    for (int i = 0; i < (int)(sizeof(kFlagProps) / sizeof(kFlagProps[0])); i++) {
        const PV *v = PVGet(kFlagProps[i]);
        if (!v || (strcmp(v->val, "true") && strcmp(kFlagProps[i], "OneShot"))) continue;
        char one[64];
        if (!strcmp(kFlagProps[i], "OneShot")) TSPRINTF(one, "%s%s", ti.flags[0] ? ", " : "", strcmp(v->val, "true") ? "every time" : "OneShot");
        else TSPRINTF(one, "%s%s", ti.flags[0] ? ", " : "", kFlagProps[i]);
        TSTRCAT(ti.flags, one);
    }
    const char *delay = PVText("Delay");
    if (delay) {
        char one[48];
        TSPRINTF(one, "%sdelay %s", ti.flags[0] ? ", " : "", delay);
        TSTRCAT(ti.flags, one);
    }
}

// the live state (every frame): enabled, the player inside, done once, our counts
static void ReadState(TInfo &ti, const TItem &it, void *obj)
{
    ti.enabled = it.enabled != 0;
    ti.inside = false;
    if (s_player) {
        __try {
            BYTE *body = *(BYTE **)((BYTE *)obj + OFS_OBJ_BODY);
            ti.inside = body && PlayerIndex(body, s_player) >= 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ti.inside = false;
        }
    }
    char done[64] = "";
    if (ti.trigRegion) {
        BYTE de = ((BYTE *)obj)[OFS_TR_DONE_ENTER], dx = ((BYTE *)obj)[OFS_TR_DONE_EXIT];
        if (de || dx) TSPRINTF(done, "; fired: %s%s%s", de ? "onEnter" : "", de && dx ? " + " : "", dx ? "onExit" : "");
    }
    DetStat *d = DetStatOf(obj, false);
    char cnt[96] = "";
    if (d && (d->enter || d->exit)) {
        DWORD now = GetTickCount();
        if (d->lastEnter) TSPRINTF(cnt, "; in %d / out %d, entered %.1f s ago", d->enter, d->exit,
                                   (now - d->lastEnter) / 1000.0f);
        else TSPRINTF(cnt, "; in %d / out %d", d->enter, d->exit);
    }
    TSPRINTF(ti.state, "%s%s%s%s", ti.enabled ? "enabled" : "DISABLED", ti.inside ? ", player INSIDE" : ", player outside",
             cnt, done);
}

static TInfo *InfoFor(void *obj)
{
    TInfo *best = NULL;
    for (int i = 0; i < kInfoSlots; i++) {
        if (s_info[i].obj == obj) return &s_info[i];
        if (s_info[i].used == s_frame) continue;  // in use this frame
        if (!best || s_info[i].used < best->used) best = &s_info[i];
    }
    if (!best) return NULL;
    memset(best, 0, sizeof(*best));
    best->obj = obj;
    best->read = -1000;
    return best;
}

static TInfo *Use(const TItem &it)
{
    TInfo *ti = InfoFor(it.obj);
    if (!ti) return NULL;
    ti->used = s_frame;
    if (s_frame - ti->read >= kRefreshFrames) ReadInfo(*ti, it.obj);
    ReadState(*ti, it, it.obj);
    return ti;
}

// every property of the target as lines (panel + log)
static void ReadAll(void *obj)
{
    s_tAllRead = s_frame;
    s_npv = 0;
    Refl_Visit(obj, PVCollect, NULL);
    s_tAllTotal = s_npv;
    s_tAllN = 0;
    const char *last = "";
    for (int i = 0; i < s_npv && s_tAllN < kAllLines; i++) {
        if (strcmp(last, s_pv[i].cls)) {
            TSPRINTF(s_tAll[s_tAllN], "-- %s --", s_pv[i].cls);
            s_tAllN++;
            last = s_pv[i].cls;
            if (s_tAllN >= kAllLines) break;
        }
        if (s_pv[i].ok) TSPRINTF(s_tAll[s_tAllN], "%s = %s", s_pv[i].prop, s_pv[i].val);
        else TSPRINTF(s_tAll[s_tAllN], "%s = (not read, type %d)", s_pv[i].prop, s_pv[i].type);
        s_tAllN++;
    }
}

// the trigger the camera looks at: the nearest volume the view ray enters; else (the camera inside volumes) the
// smallest one around the camera; else the one nearest to the screen centre within 8 deg. The current one stays
// while the ray still enters it and the best is not 15 % nearer (no flicker).
static int PickTarget(const float *o, const float *dir)
{
    int bestHit = -1, bestIn = -1, bestAim = -1, cur = -1;
    float tHit = 1e30f, vIn = 1e30f, aAim = 8.0f, tCur = 0;
    bool curHit = false;
    for (int i = 0; i < s_numItems; i++) {
        const TItem &it = s_items[i];
        if (!it.enabled && !g_trigShowDisabled) continue;
        float t;
        bool inside;
        if (RayHit(it, o, dir, &t, &inside)) {
            if (!inside && t < tHit) tHit = t, bestHit = i;
            if (inside && Volume(it) < vIn) vIn = Volume(it), bestIn = i;
            if (it.obj == s_tObj) cur = i, curHit = !inside, tCur = t;
        } else if (it.obj == s_tObj) {
            cur = i;
        }
        float d[3] = {it.c[0] - o[0], it.c[1] - o[1], it.c[2] - o[2]}, l = sqrtf(Dot3(d, d));
        if (l > 1) {
            float a = acosf(fmaxf(-1.0f, fminf(1.0f, Dot3(d, dir) / l))) * 57.29578f;
            if (a < aAim) aAim = a, bestAim = i;
        }
    }
    if (bestHit >= 0) {
        if (cur >= 0 && curHit && tHit >= tCur * 0.85f) {
            TSPRINTF(s_tHow, "ray enters it at %.1f m", tCur / kTUnitsPerM);
            s_tDist = tCur;
            return cur;
        }
        TSPRINTF(s_tHow, "ray enters it at %.1f m", tHit / kTUnitsPerM);
        s_tDist = tHit;
        return bestHit;
    }
    if (bestIn >= 0) {
        TSTRCPY(s_tHow, "the camera is inside");
        s_tDist = 0;
        return bestIn;
    }
    if (bestAim >= 0) {
        TSPRINTF(s_tHow, "nearest to the centre (%.1f deg)", aAim);
        float d[3] = {s_items[bestAim].c[0] - o[0], s_items[bestAim].c[1] - o[1], s_items[bestAim].c[2] - o[2]};
        s_tDist = sqrtf(Dot3(d, d));
        return bestAim;
    }
    s_tHow[0] = 0;
    return -1;
}

static void LogTarget(const TItem &it, const TInfo &ti)
{
    if (!LogBudget(&s_tLogBudget, 40)) return;
    Log("triggers: target -> '%s' %s (%s) at (%.0f %.0f %.0f) shape %d | %s", ti.name, ti.chain, s_tHow, it.c[0],
        it.c[1], it.c[2], it.shape, ti.state);
    Log("triggers:   leads to: %s | events: %s | who: %s | flags: %s", ti.dest[0] ? ti.dest : "-",
        ti.events[0] ? ti.events : "-", ti.filter[0] ? ti.filter : "-", ti.flags[0] ? ti.flags : "-");
    for (int i = 0; i < s_tAllN; i++) Log("triggers:   %s", s_tAll[i]);
    if (s_tAllTotal > s_tAllN) Log("triggers:   ... %d more properties", s_tAllTotal - s_tAllN);
}

// "on": which classes the triggers have and how many properties were read (for the in-game check)
static void LogClasses()
{
    struct { char cls[48]; int n, props, unread; char sample[64]; } c[32];
    int nc = 0, nobj = 0;
    for (int i = 0; i < s_numItems && nobj < 3000; i++, nobj++) {
        void *cl = Refl_ClassOf(s_items[i].obj);
        const char *nm = cl ? Refl_ClassName(cl) : "(no class)";
        int k = 0;
        while (k < nc && strcmp(c[k].cls, nm)) k++;
        if (k == nc) {
            if (nc == 32) continue;
            TSTRCPY(c[k].cls, nm);
            c[k].n = 0;
            s_npv = 0;
            Refl_Visit(s_items[i].obj, PVCollect, NULL);
            c[k].props = s_npv;
            c[k].unread = 0;
            for (int q = 0; q < s_npv; q++) c[k].unread += !s_pv[q].ok;
            TSTRCPY(c[k].sample, Refl_ObjName(s_items[i].obj));
            nc++;
        }
        c[k].n++;
    }
    Log("triggers: info ON (nearby %s, target %s, range %s, display %s): %d triggers, %d classes:",
        g_trigNearby ? "on" : "off", g_trigTarget ? "on" : "off",
        kTRangeM[g_trigRange] ? "limited" : "unlimited", TShowText(), s_numItems, nc);
    for (int k = 0; k < nc; k++)
        Log("triggers:   class %s: %d objects, %d properties (%d not read), e.g. '%s'", c[k].cls, c[k].n, c[k].props,
            c[k].unread, c[k].sample);
}

static void InfoInner()
{
    if (s_infoThrow) {
        s_infoThrow = false;
        *(volatile int *)0 = 2;
    }
    // the player: the "inside" check; the reference point of nearby: the player, or the camera in a free flight
    s_player = NULL;
    s_refOk = s_camOk = false;
    if (TrigFake()) {
        s_player = s_fakePlayer;
    } else {
        void *game = NULL, *actor = NULL;
        if (Game_GetGame(&game) && Game_GetActor(game, &actor)) s_player = actor;
    }
    bool viewOk = s_view.ok && s_frame - s_view.frame <= 3;
    if (viewOk) {
        for (int i = 0; i < 3; i++) s_cam[i] = s_view.cam[12 + i];
        s_camOk = true;
    }
    s_refCam = g_flying && !Flight_Attached();
    if (s_refCam && s_camOk) {
        memcpy(s_ref, s_cam, sizeof(s_ref));
        s_refOk = true;
    } else if (s_player && !TrigFake()) {
        GameVec p;
        if (Game_ActorGetPos(s_player, &p)) s_ref[0] = p.x, s_ref[1] = p.y, s_ref[2] = p.z, s_refOk = true;
    }
    if (!s_refOk && s_camOk) {  // no player (test, loading): the camera
        memcpy(s_ref, s_cam, sizeof(s_ref));
        s_refOk = true;
        s_refCam = true;
    }
    World_Refresh();  // region names for LoadOnEnter (twice a second; the map log too)
    if (!s_infoWasOn) {
        s_infoWasOn = true;
        s_infoMs = s_infoMax = 0;
        s_infoN = 0;
        LogClasses();
    }
    // nearby: the nearest kTMaxLabels within the range (distance to the volume)
    s_numLab = s_inRange = 0;
    if (g_trigNearby && s_refOk) {
        float range = kTRangeM[g_trigRange] * kTUnitsPerM;
        for (int i = 0; i < s_numItems; i++) {
            const TItem &it = s_items[i];
            if (!it.enabled && !g_trigShowDisabled) continue;
            float d = DistTo(it, s_ref);
            if (range > 0 && d > range) continue;
            s_inRange++;
            int k = s_numLab < kTMaxLabels ? s_numLab++ : kTMaxLabels;
            if (k == kTMaxLabels) {
                if (d >= s_lab[kTMaxLabels - 1].dist) continue;
                k = kTMaxLabels - 1;
            }
            while (k > 0 && s_lab[k - 1].dist > d) s_lab[k] = s_lab[k - 1], k--;
            s_lab[k].item = i;
            s_lab[k].dist = d;
            s_lab[k].info = NULL;
        }
    }
    // target
    int ti = -1;
    if (g_trigTarget && viewOk) {
        float dir[3] = {-s_view.cam[8], -s_view.cam[9], -s_view.cam[10]}, l = sqrtf(Dot3(dir, dir));
        if (l > 0) {
            for (int i = 0; i < 3; i++) dir[i] /= l;
            ti = PickTarget(s_cam, dir);
        }
    }
    void *prev = s_tObj;
    s_tItem = ti;
    s_tObj = ti >= 0 ? s_items[ti].obj : NULL;
    s_tInfo = ti >= 0 ? Use(s_items[ti]) : NULL;
    if (ti >= 0 && s_tInfo) {
        if (s_tObj != prev || s_frame - s_tAllRead >= kRefreshFrames) ReadAll(s_tObj);
        if (s_tObj != prev) LogTarget(s_items[ti], *s_tInfo);
    } else if (prev && LogBudget(&s_tLogBudget, 40)) {
        Log("triggers: target -> none");
    }
    for (int k = 0; k < s_numLab; k++) s_lab[k].info = Use(s_items[s_lab[k].item]);
}

static void InfoFrame()
{
    double t0 = NowMs();
    __try {
        InfoInner();
    } __except (CatchTr("trigger info", GetExceptionInformation())) {
        s_numLab = 0;
        s_tItem = -1;
        s_tInfo = NULL;
        s_tObj = NULL;
        InfoTurnOff("exception while reading the info");
        return;
    }
    double dt = NowMs() - t0;
    s_infoMs += dt;
    s_infoMax = dt > s_infoMax ? dt : s_infoMax;
    s_infoN++;
    if (s_infoN >= 300) {  // ~5 s
        if (LogBudget(&s_infoStatsBudget, 200))
            Log("triggers: info: %d in range, %d labels (%d drawn, %d skipped), target '%s' | read %.3f ms avg (max %.3f)",
                s_inRange, s_numLab, s_labDrawn, s_labSkipped, s_tInfo ? s_tInfo->name : "-", s_infoMs / s_infoN,
                s_infoMax);
        s_infoMs = s_infoMax = 0;
        s_infoN = 0;
    }
}

// ---- drawing (overlay frame, text only) ----
struct TLine { char s[176]; ImU32 col; };
static const ImU32 kTWhite = IM_COL32(255, 255, 255, 255), kTDim = IM_COL32(200, 200, 200, 255),
                   kTOff = IM_COL32(160, 160, 160, 255), kTDest = IM_COL32(255, 210, 120, 255),
                   kTIn = IM_COL32(120, 255, 120, 255);

static int LabelLines(const TItem &it, const TInfo &ti, TLine *l, int max)
{
    int n = 0;
    const bool *sh = g_trigShow;
    ImU32 base = ti.enabled ? kTWhite : kTOff;
    if (sh[TF_NAME] && n < max) {
        TSPRINTF(l[n].s, "%s%s%s", ti.name[0] ? ti.name : "(no name)", ti.enabled ? "" : " [off]",
                 ti.inside ? " [inside]" : "");
        l[n++].col = ti.inside ? kTIn : base;
    }
    if (sh[TF_CLASS] && n < max) TSPRINTF(l[n].s, "%s", ti.cls[0] ? ti.cls : "detector"), l[n++].col = kTDim;
    if (sh[TF_DEST] && ti.dest[0] && n < max) TSPRINTF(l[n].s, "-> %s", ti.destShort), l[n++].col = kTDest;
    // labels leave out the lines with nothing to say (the panel shows them as "-")
    if (sh[TF_EVENTS] && strcmp(ti.eventsShort, "-") && n < max) TSPRINTF(l[n].s, "events: %s", ti.eventsShort), l[n++].col = base;
    if (sh[TF_FILTER] && ti.filter[0] && n < max) {
        char s[96];
        Short(s, sizeof(s), ti.filter[0] ? ti.filter : "-", 60);
        TSPRINTF(l[n].s, "who: %s", s);
        l[n++].col = base;
    }
    if (sh[TF_FLAGS] && ti.flags[0] && n < max) {
        char s[96];
        Short(s, sizeof(s), ti.flags[0] ? ti.flags : "-", 60);
        TSPRINTF(l[n].s, "flags: %s", s);
        l[n++].col = base;
    }
    if (sh[TF_STATE] && n < max) {
        char s[96];
        Short(s, sizeof(s), ti.state, 70);
        TSPRINTF(l[n].s, "%s", s);
        l[n++].col = ti.inside ? kTIn : base;
    }
    if (sh[TF_SHAPE] && n < max) {
        if (it.draw == SH_SPHERE) TSPRINTF(l[n].s, "sphere r %.0f", it.rad);
        else TSPRINTF(l[n].s, "%s %.0f x %.0f x %.0f", it.shape == 1 ? "box (rotated)" : it.shape == 3 ? "capsule" : "box",
                      it.h[0] * 2, it.h[1] * 2, it.h[2] * 2);
        l[n++].col = kTDim;
    }
    if (sh[TF_POS] && n < max) TSPRINTF(l[n].s, "pos (%.0f %.0f %.0f)", it.c[0], it.c[1], it.c[2]), l[n++].col = kTDim;
    return n;
}

static bool TProject(const float *p, float *sx, float *sy)
{
    float c[4];
    ToClip(p, c);
    if (c[2] < 0 || c[3] < 1.0f) return false;
    float iw = 1.0f / c[3];
    *sx = (c[0] * iw * 0.5f + 0.5f) * s_W;
    *sy = (0.5f - c[1] * iw * 0.5f) * s_H;
    return _finite(*sx) && _finite(*sy) && fabsf(*sx) < 1e5f && fabsf(*sy) < 1e5f;
}

struct TRect { float x0, y0, x1, y1; };
static void DrawLabels(ImDrawList *dl)
{
    ImFont *font = ImGui::GetFont();
    float fsz = ImGui::GetFontSize() * 0.85f, lh = fsz + 1.0f;
    TRect placed[kTMaxLabels];
    int np = 0;
    s_labDrawn = s_labSkipped = 0;
    for (int k = 0; k < s_numLab; k++) {
        if (!s_lab[k].info) continue;
        const TItem &it = s_items[s_lab[k].item];
        float p[3], sx, sy;
        LabelPoint(it, p);
        if (!TProject(p, &sx, &sy)) continue;
        TLine lines[10];
        int n = LabelLines(it, *s_lab[k].info, lines, 10);
        if (!n) continue;
        float w = 0;
        for (int i = 0; i < n; i++) w = fmaxf(w, font->CalcTextSizeA(fsz, FLT_MAX, 0, lines[i].s).x);
        TRect r = {sx - w * 0.5f - 3, sy - n * lh - 4, sx + w * 0.5f + 3, sy};
        bool ok = false;
        for (int tries = 0; tries < 4 && !ok; tries++) {  // nearest first; one that hits a placed label moves up
            ok = true;
            for (int q = 0; q < np; q++) {
                const TRect &o = placed[q];
                if (r.x0 < o.x1 && r.x1 > o.x0 && r.y0 < o.y1 && r.y1 > o.y0) {
                    float hgt = r.y1 - r.y0;
                    r.y1 = o.y0 - 2;
                    r.y0 = r.y1 - hgt;
                    ok = false;
                    break;
                }
            }
        }
        if (!ok || r.y0 < 0 || r.x1 < 0 || r.x0 > s_W || r.y1 > s_H) {
            s_labSkipped++;
            continue;
        }
        placed[np++] = r;
        bool isT = g_trigTarget && it.obj == s_tObj;
        dl->AddRectFilled(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1), IM_COL32(0, 0, 0, 115), 2.0f);
        dl->AddRect(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1), isT ? kTgtCol : (it.trig ? IM_COL32(255, 0, 255, 120) :
                    IM_COL32(0, 255, 255, 110)), 2.0f, 0, isT ? 1.5f : 1.0f);
        for (int i = 0; i < n; i++) {
            float tw = font->CalcTextSizeA(fsz, FLT_MAX, 0, lines[i].s).x;
            dl->AddText(font, fsz, ImVec2(floorf(sx - tw * 0.5f), floorf(r.y0 + 2 + i * lh)), lines[i].col, lines[i].s);
        }
        s_labDrawn++;
    }
}

// the target: its shape in yellow (also without Show triggers) + corners around it on the screen
static void DrawHighlight(ImDrawList *dl, const TItem &it)
{
    s_emit = EM_IMGUI;
    s_dl = dl;
    s_zA = s_view.proj[10];
    s_zB = s_view.proj[14];
    BuildPlanes();
    DrawItem(it, 0, kTgtCol, true);
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    int ok = 0;
    for (int k = 0; k < 8; k++) {
        float p[3], sx, sy;
        float e0 = (k & 1) ? 1.0f : -1.0f, e1 = (k & 2) ? 1.0f : -1.0f, e2 = (k & 4) ? 1.0f : -1.0f;
        if (it.draw == SH_SPHERE) {
            p[0] = it.c[0] + e0 * it.rad, p[1] = it.c[1] + e1 * it.rad, p[2] = it.c[2] + e2 * it.rad;
        } else {
            for (int i = 0; i < 3; i++)
                p[i] = it.c[i] + e0 * it.h[0] * it.r[i] + e1 * it.h[1] * it.r[3 + i] + e2 * it.h[2] * it.r[6 + i];
        }
        if (!TProject(p, &sx, &sy)) continue;
        ok++;
        x0 = fminf(x0, sx), y0 = fminf(y0, sy), x1 = fmaxf(x1, sx), y1 = fmaxf(y1, sy);
    }
    if (ok < 8) return;  // partly behind the camera / the camera inside: the yellow shape is enough
    x0 = fmaxf(x0 - 6, 2), y0 = fmaxf(y0 - 6, 2), x1 = fminf(x1 + 6, s_W - 2), y1 = fminf(y1 + 6, s_H - 2);
    float k = fminf(16.0f, fminf((x1 - x0) * 0.3f, (y1 - y0) * 0.3f));
    dl->AddLine(ImVec2(x0, y0), ImVec2(x0 + k, y0), kTgtCol, 2);
    dl->AddLine(ImVec2(x0, y0), ImVec2(x0, y0 + k), kTgtCol, 2);
    dl->AddLine(ImVec2(x1, y0), ImVec2(x1 - k, y0), kTgtCol, 2);
    dl->AddLine(ImVec2(x1, y0), ImVec2(x1, y0 + k), kTgtCol, 2);
    dl->AddLine(ImVec2(x0, y1), ImVec2(x0 + k, y1), kTgtCol, 2);
    dl->AddLine(ImVec2(x0, y1), ImVec2(x0, y1 - k), kTgtCol, 2);
    dl->AddLine(ImVec2(x1, y1), ImVec2(x1 - k, y1), kTgtCol, 2);
    dl->AddLine(ImVec2(x1, y1), ImVec2(x1, y1 - k), kTgtCol, 2);
}

static const ImGuiWindowFlags kTPanel = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize;

static void PanelField(const char *label, const char *text, const ImVec4 &col)
{
    ImGui::TextColored(ImVec4(1.0f, 0.80f, 0.30f, 1.0f), "%s", label);
    ImGui::SameLine();
    ImGui::TextColored(col, "%s", text && text[0] ? text : "-");
}

// left side from 30 % of the height (below the open menu): off the centre, away from the Monsters panel (right)
// and the status lines (bottom left)
static void DrawPanel()
{
    float fs = ImGui::GetFontSize();
    float y = s_H * 0.30f, mb = Menu_PanelBottom();
    if (g_menuOpen && mb > 0) y = fmaxf(y, mb + fs * 0.5f);
    float maxH = s_H - y - fs * 5.5f;
    if (maxH < fs * 5) return;  // the open menu fills the left side: back when it closes (the highlight stays)
    ImGui::SetNextWindowPos(ImVec2(fs * 1.5f, y), ImGuiCond_Always, ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.70f);
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(s_W * 0.40f, maxH));
    ImGui::Begin("##dbgtrigtarget", NULL, kTPanel);
    ImGui::SetWindowFontScale(0.85f);
    float wrap = fmaxf(fs * 14.0f, s_W * 0.38f - ImGui::GetStyle().WindowPadding.x * 2);
    ImGui::PushTextWrapPos(wrap);
    ImGui::TextColored(ImVec4(1.0f, 0.80f, 0.30f, 1.0f), "info: target (Triggers)");
    if (s_tItem < 0 || !s_tInfo) {
        ImGui::TextDisabled("no trigger in the centre of the screen");
        ImGui::PopTextWrapPos();
        ImGui::End();
        return;
    }
    const TItem &it = s_items[s_tItem];
    const TInfo &ti = *s_tInfo;
    const bool *sh = g_trigShow;
    bool any = false;
    for (int i = 0; i < TF_COUNT; i++) any |= sh[i];
    if (!any) {
        ImGui::TextDisabled("all lines are off (Triggers > Display)");
        ImGui::PopTextWrapPos();
        ImGui::End();
        return;
    }
    ImVec4 white(1, 1, 1, 1), dim(0.75f, 0.75f, 0.75f, 1), dest(1.0f, 0.82f, 0.47f, 1), in(0.47f, 1, 0.47f, 1);
    if (sh[TF_NAME]) ImGui::Text("%s%s", ti.name[0] ? ti.name : "(no name)", ti.enabled ? "" : "  [off]");
    if (sh[TF_CLASS]) PanelField("class", ti.chain, dim);
    if (sh[TF_DEST]) PanelField("leads to", ti.dest, dest);
    if (sh[TF_EVENTS]) PanelField("events", ti.events, white);
    if (sh[TF_FILTER]) PanelField("who", ti.filter, white);
    if (sh[TF_FLAGS]) PanelField("flags", ti.flags, white);
    if (sh[TF_STATE]) PanelField("state", ti.state, ti.inside ? in : white);
    if (sh[TF_SHAPE]) {
        char s[160];
        static const char *const kShape[] = {"box", "rotated box", "sphere", "capsule (drawn as a box)", "box (shape ?)"};
        if (it.draw == SH_SPHERE) TSPRINTF(s, "%s, radius %.0f (%.1f m)", kShape[it.shape], it.rad, it.rad / kTUnitsPerM);
        else TSPRINTF(s, "%s %.0f x %.0f x %.0f (%.1f x %.1f x %.1f m)", kShape[it.shape], it.h[0] * 2, it.h[1] * 2,
                      it.h[2] * 2, it.h[0] * 0.02f, it.h[1] * 0.02f, it.h[2] * 0.02f);
        PanelField("shape", s, dim);
    }
    if (sh[TF_POS]) {
        char s[80];
        TSPRINTF(s, "(%.1f %.1f %.1f)", it.c[0], it.c[1], it.c[2]);
        PanelField("pos", s, dim);
    }
    if (sh[TF_DIST]) {
        char s[160];
        float dp = s_refOk && !s_refCam ? DistTo(it, s_ref) : -1;
        if (dp >= 0) TSPRINTF(s, "player %.1f m%s, camera %.1f m (%s)", dp / kTUnitsPerM, dp == 0 ? " (inside)" : "",
                              s_tDist / kTUnitsPerM, s_tHow);
        else TSPRINTF(s, "camera %.1f m (%s)", s_tDist / kTUnitsPerM, s_tHow);
        PanelField("distance", s, dim);
    }
    if (sh[TF_ALL]) {
        ImGui::Separator();
        ImGui::TextDisabled("all properties (%d, %d not read):", ti.nProps, ti.nUnread);
        // as many as fit in the panel; the rest is in the log (target change)
        float room = maxH - ImGui::GetCursorPosY() - fs * 2.0f;
        int fit = (int)(room / ImGui::GetTextLineHeightWithSpacing());
        int shown = 0;
        for (int i = 0; i < s_tAllN && shown < fit; i++, shown++) {
            if (s_tAll[i][0] == '-') ImGui::TextColored(ImVec4(1.0f, 0.80f, 0.30f, 0.8f), "%s", s_tAll[i]);
            else ImGui::TextDisabled("%s", s_tAll[i]);
        }
        if (shown < s_tAllN || s_tAllTotal > s_tAllN)
            ImGui::TextDisabled("... %d more lines (dbgmenu.log, when the target changes)", s_tAllN - shown + (s_tAllTotal > s_tAllN ? s_tAllTotal - s_tAllN : 0));
    }
    ImGui::PopTextWrapPos();
    ImGui::End();
}

void Trig_InfoDraw2D()
{
    if (!g_trigNearby && !g_trigTarget) return;
    if (!ViewFresh()) return;  // loading / menus without 3D
    ImGuiIO &io = ImGui::GetIO();
    s_W = io.DisplaySize.x;
    s_H = io.DisplaySize.y;
    __try {
        ImDrawList *dl = ImGui::GetBackgroundDrawList();
        if (g_trigTarget && s_tItem >= 0 && s_tItem < s_numItems) DrawHighlight(dl, s_items[s_tItem]);
        if (g_trigNearby) DrawLabels(dl);
        if (g_trigTarget) DrawPanel();
    } __except (DrawFilter("trigger info drawing", GetExceptionInformation())) {
        InfoTurnOff("exception while drawing the info");
    }
}

static void InfoStatus()
{
    if (!g_trigNearby && !g_trigTarget) return;
    ImGui::SameLine();
    char range[24];
    if (kTRangeM[g_trigRange]) TSPRINTF(range, "%.0f m", kTRangeM[g_trigRange]);
    else TSTRCPY(range, "unlimited");
    if (g_trigNearby) ImGui::TextDisabled("| labels %d / %d in %s%s", s_labDrawn, s_inRange, range,
                                          s_refCam ? " of the camera" : "");
    if (g_trigTarget) {
        ImGui::SameLine();
        ImGui::TextDisabled("| target: %s", s_tInfo ? (s_tInfo->name[0] ? s_tInfo->name : "(no name)") : "-");
    }
}

// ======================= offline test controls =======================
// 8 = projection self-check, 9 = level unloaded (empty list), 10 = broken list (bad node + cycle), 11 = normal,
// 13 = exception in the next collection, 14 = 8000 objects (timing), 15 = task 09 detector enter/exit log
// task 13: 29 / 30 the player enters / leaves the door trigger (body list + the task 09 hooks), 31 exception
static void TestDoor(bool enter)
{
    static BYTE wrap[16], col[0x40];
    *(void **)(wrap + 0xC) = s_fakePlayer;
    *(void **)(col + 0x3C) = wrap;
    int &n = *(int *)(s_fBody + 0x2C);
    if (enter) {
        Trig_OnDetectorAdd(s_fBody, col);  // the hook looks first, then the game adds it
        s_fBodyObjs[0] = s_fakePlayer;
        s_fBodyCounts[0] = 1;
        n = 1;
        s_fDoneEnter = 1;
    } else {
        Trig_OnDetectorRemove(s_fBody, s_fakePlayer);
        n = 0;
    }
    Log("test: the player %s the door trigger", enter ? "enters" : "leaves");
}

void Trig_TestControl(int cmd)
{
    if (!TrigFake()) return;
    if (cmd == 29 || cmd == 30) {
        TestDoor(cmd == 29);
        return;
    }
    if (cmd == 31) {
        s_infoThrow = true;
        Log("test: forcing an exception in the next trigger info reading");
        return;
    }
    if (cmd == 15) {
        TestDetectorEvents();
        return;
    }
    if (cmd == 8) {
        if (!s_view.ok) {
            Log("test: projection check: no view yet");
            return;
        }
        float pts[3][3] = {{700, 200, 300}, {700, 100, 300}, {700, 200, 400}};
        for (int i = 0; i < 3; i++) {
            float c[4];
            ToClip(pts[i], c);
            Log("test: projection check: (%.0f %.0f %.0f) -> clip (%.2f %.2f %.4f %.2f) -> ndc x %.4f y %.4f z %.6f",
                pts[i][0], pts[i][1], pts[i][2], c[0], c[1], c[2], c[3], c[0] / c[3], c[1] / c[3], c[2] / c[3]);
        }
        LogView("test", (UINT)s_W, (UINT)s_H);
        LogStats("test check");
        Log("test: bad vertices so far %ld, last drawn %d, last collect %.3f ms, last draw %.3f ms (%s)", s_badVtx,
            s_drawn, s_lastCollectMs, s_lastDrawMs, g_trigDepthMode == TD_DEPTH ? "depth" : "ImGui");
        return;
    }
    if (cmd == 13) {
        s_testThrow = true;
        Log("test: forcing an exception in the next trigger collection");
        return;
    }
    s_fakeMode = cmd == 11 ? 0 : cmd;
    s_logOnEnable = true;  // log the stats of the next collection
    Log("test: triggers fake list mode %d", s_fakeMode);
}

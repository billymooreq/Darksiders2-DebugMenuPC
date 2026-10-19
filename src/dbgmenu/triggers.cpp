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
#include "common.h"
#include <math.h>
#include <stdio.h>
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
static __declspec(align(16)) BYTE s_fakeObj[16][0xE0];
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
    memset(o, 0, 0xE0);
    *(void ***)o = vt;
    *(int *)(o + OFS_OBJ_SHAPE) = shape;
    float *p = (float *)(o + OFS_OBJ_POS), *r = (float *)(o + OFS_OBJ_ROT), *s = (float *)(o + OFS_OBJ_SCALE);
    p[0] = x; p[1] = y; p[2] = z;
    r[2] = rz;
    s[0] = sx; s[1] = sy; s[2] = sz;
    o[OFS_OBJ_ENABLED] = enabled;
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
    int used = n;
    if (s_fakeMode == 9) used = 0;
    if (s_fakeMode == 14) {  // a 20 x 20 x 20 grid ahead of the camera, every shape, every 5th disabled
        if (!s_fakeBigObj) {
            s_fakeBigObj = (BYTE *)VirtualAlloc(NULL, kFakeBig * 0xE0, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            s_fakeBigNodes = (FakeNode *)VirtualAlloc(NULL, kFakeBig * sizeof(FakeNode), MEM_COMMIT | MEM_RESERVE,
                                                      PAGE_READWRITE);
        }
        if (s_fakeBigObj && s_fakeBigNodes) {
            for (int i = 0; i < kFakeBig; i++) {
                BYTE *o = s_fakeBigObj + i * 0xE0;
                memset(o, 0, 0xE0);
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
    if (!g_showTriggers) {
        s_wasOn = false;
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
        return;
    }
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

// task 11: the EventManagers of the loaded world (their actor list +0x8 has the characters); real game only
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
void Trig_DrawStatus()
{
    if (!g_showTriggers) return;
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
    ImGui::Text("triggers: %d drawn / %d total", ViewFresh() ? s_drawn : 0, s_numItems);
    ImGui::SameLine();
    if (g_trigDepthMode == TD_DEPTH) ImGui::TextDisabled(s_depthNA ? "[depth: n/a, through walls]" : "[depth]");
    else ImGui::TextDisabled("[through walls]");
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

// ======================= offline test controls =======================
// 8 = projection self-check, 9 = level unloaded (empty list), 10 = broken list (bad node + cycle), 11 = normal,
// 13 = exception in the next collection, 14 = 8000 objects (timing), 15 = task 09 detector enter/exit log
void Trig_TestControl(int cmd)
{
    if (!TrigFake()) return;
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

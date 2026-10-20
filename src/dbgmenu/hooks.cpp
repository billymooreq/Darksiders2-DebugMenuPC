// Hooks: IAT (Direct3DCreate9, GetRawInputData, ShowCursor/ClipCursor/SetCursorPos) -> MinHook on d3d9.dll methods
// (IDirect3D9::CreateDevice, IDirect3DDevice9::Present/Reset/Release/SetRenderTarget/SetDepthStencilSurface/Clear,
// IDirect3DSwapChain9::Present) and on game functions (frame 0x00B3D3E4, Darksiders::getCameraMatrix 0x004CD9DA;
// task 07: render view setup 0x00983AFC, HUD pass 0x00B3B8D2).
#include "common.h"
#include "MinHook.h"
#include <intrin.h>

// Expected IAT slots in Darksiders2.exe (pefile, task 01) — logged against the slot found by name.
#define IAT_DIRECT3DCREATE9 0x01155730u
#define IAT_GETRAWINPUTDATA 0x011555D0u

typedef IDirect3D9 *(WINAPI *Direct3DCreate9_t)(UINT);
typedef HRESULT(STDMETHODCALLTYPE *CreateDevice_t)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD,
                                                    D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
typedef HRESULT(STDMETHODCALLTYPE *Reset_t)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
typedef HRESULT(STDMETHODCALLTYPE *Present_t)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
typedef HRESULT(STDMETHODCALLTYPE *SCPresent_t)(IDirect3DSwapChain9 *, const RECT *, const RECT *, HWND,
                                                 const RGNDATA *, DWORD);
typedef ULONG(STDMETHODCALLTYPE *Release_t)(IDirect3DDevice9 *);

static Direct3DCreate9_t o_Direct3DCreate9;
static CreateDevice_t o_CreateDevice;
static Reset_t o_Reset;
static Present_t o_Present;
static SCPresent_t o_SCPresent;
static Release_t o_Release;

static __declspec(thread) int t_presentDepth;  // device Present may call the swap chain Present inside
static __declspec(thread) int t_inRelease;

// ---- helpers ----
void **FindImportSlot(HMODULE mod, const char *dll, const char *func)
{
    BYTE *base = (BYTE *)mod;
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return NULL;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++) {
        if (_stricmp((char *)(base + d->Name), dll) != 0) continue;
        IMAGE_THUNK_DATA32 *names = (IMAGE_THUNK_DATA32 *)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        void **slots = (void **)(base + d->FirstThunk);
        for (int i = 0; names[i].u1.AddressOfData; i++) {
            if (IMAGE_SNAP_BY_ORDINAL32(names[i].u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *)(base + names[i].u1.AddressOfData);
            if (strcmp((char *)ibn->Name, func) == 0) return &slots[i];
        }
    }
    return NULL;
}

bool PatchSlot(void **slot, void *fn, void **orig)
{
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) return false;
    *orig = *slot;
    *slot = fn;
    VirtualProtect(slot, sizeof(void *), old, &old);
    return true;
}

// MinHook on the implementation behind vtable[idx]; d3d9.dll shares it for every object of that class.
static bool HookVfunc(void *obj, int idx, void *detour, void **orig, const char *name)
{
    if (*orig) return true;  // already hooked
    void *target = (*(void ***)obj)[idx];
    MH_STATUS s = MH_CreateHook(target, detour, orig);
    if (s == MH_OK) s = MH_EnableHook(target);
    Log("hook %-28s vt[%2d] -> %08X : %s", name, idx, (unsigned)(UINT_PTR)target, MH_StatusToString(s));
    return s == MH_OK;
}

// ---- IDirect3DDevice9 / IDirect3DSwapChain9 ----
static HRESULT STDMETHODCALLTYPE hkPresent(IDirect3DDevice9 *dev, const RECT *src, const RECT *dst, HWND wnd,
                                           const RGNDATA *dirty)
{
    if (++t_presentDepth == 1) {
        if (!g_frameHooked) {  // fallback (offline test / frame hook refused)
            Trig_Frame();
            Flight_Frame();
            Mon_Frame();
            Cheats_Frame();
        }
        Trig_OnPresent(dev);  // depth mode, if the HUD pass did not draw this frame
        Overlay_OnFrame(dev);
    }
    HRESULT hr = o_Present(dev, src, dst, wnd, dirty);
    --t_presentDepth;
    return hr;
}

static HRESULT STDMETHODCALLTYPE hkSCPresent(IDirect3DSwapChain9 *sc, const RECT *src, const RECT *dst, HWND wnd,
                                             const RGNDATA *dirty, DWORD flags)
{
    if (++t_presentDepth == 1) {
        IDirect3DDevice9 *dev = NULL;
        if (!g_frameHooked) {
            Trig_Frame();
            Flight_Frame();
            Mon_Frame();
            Cheats_Frame();
        }
        if (SUCCEEDED(sc->GetDevice(&dev)) && dev) {
            Trig_OnPresent(dev);
            Overlay_OnFrame(dev);
            t_inRelease++;  // our own reference, not the game's
            dev->Release();
            t_inRelease--;
        }
    }
    HRESULT hr = o_SCPresent(sc, src, dst, wnd, dirty, flags);
    --t_presentDepth;
    return hr;
}

static HRESULT STDMETHODCALLTYPE hkReset(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp)
{
    static volatile LONG budget;
    static volatile LONG flyBudget;
    if (g_flying && LogBudget(&flyBudget, 50)) Log("Reset during the flight: the flight goes on");  // task 05
    Overlay_BeforeReset(dev);  // D3DPOOL_DEFAULT resources must be gone before Reset
    HRESULT hr = o_Reset(dev, pp);
    if (LogBudget(&budget, 50))
        Log("Reset(dev=%08X) bb=%ux%u fmt=%d windowed=%d refresh=%u -> hr=%08X", (unsigned)(UINT_PTR)dev,
            pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0, pp ? pp->BackBufferFormat : 0,
            pp ? pp->Windowed : -1, pp ? pp->FullScreen_RefreshRateInHz : 0, hr);
    return hr;
}

// task 07: render target / depth / Clear calls, logged for one frame when the triggers depth mode is switched on
typedef HRESULT(STDMETHODCALLTYPE *SetRT_t)(IDirect3DDevice9 *, DWORD, IDirect3DSurface9 *);
typedef HRESULT(STDMETHODCALLTYPE *SetDS_t)(IDirect3DDevice9 *, IDirect3DSurface9 *);
typedef HRESULT(STDMETHODCALLTYPE *Clear_t)(IDirect3DDevice9 *, DWORD, const D3DRECT *, DWORD, D3DCOLOR, float, DWORD);
static SetRT_t o_SetRT;
static SetDS_t o_SetDS;
static Clear_t o_Clear;

static HRESULT STDMETHODCALLTYPE hkSetRT(IDirect3DDevice9 *dev, DWORD i, IDirect3DSurface9 *s)
{
    Trig_D3DEvent(TE_SETRT, dev, i, (DWORD)(UINT_PTR)_ReturnAddress(), s);
    return o_SetRT(dev, i, s);
}

static HRESULT STDMETHODCALLTYPE hkSetDS(IDirect3DDevice9 *dev, IDirect3DSurface9 *s)
{
    Trig_D3DEvent(TE_SETDS, dev, 0, (DWORD)(UINT_PTR)_ReturnAddress(), s);
    return o_SetDS(dev, s);
}

static HRESULT STDMETHODCALLTYPE hkClear(IDirect3DDevice9 *dev, DWORD n, const D3DRECT *r, DWORD flags, D3DCOLOR c,
                                         float z, DWORD st)
{
    Trig_D3DEvent(TE_CLEAR, dev, flags, (DWORD)(UINT_PTR)_ReturnAddress(), NULL);
    return o_Clear(dev, n, r, flags, c, z, st);
}

static ULONG PeekRefs(IDirect3DDevice9 *dev)
{
    dev->AddRef();
    return o_Release(dev);
}

// The overlay keeps references to the device: ImGui's DX9 backend AddRef()s it, and in D3D9 every resource
// (ImGui vertex/index buffer, font texture) holds one too. A game device normally has hundreds (one per
// resource), so a release at a low count means the game is tearing it down: free our resources, and if only
// the game's last reference + ImGui's remain, shut the backend down so the device can really be destroyed.
static ULONG STDMETHODCALLTYPE hkRelease(IDirect3DDevice9 *dev)
{
    if (!t_inRelease && !Overlay_Internal() && dev == Overlay_BoundDevice()) {
        t_inRelease++;
        ULONG refs = PeekRefs(dev);
        if (refs <= 8) {
            Overlay_FreeResources(dev);
            ULONG left = PeekRefs(dev);
            static volatile LONG budget;
            if (LogBudget(&budget, 20))
                Log("device %08X release at refcount %lu -> overlay resources freed, %lu left", (unsigned)(UINT_PTR)dev,
                    refs, left);
            if (left <= 2) Overlay_DeviceDying(dev);
        }
        t_inRelease--;
    }
    return o_Release(dev);
}

static void HookDevice(IDirect3DDevice9 *dev)
{
    HookVfunc(dev, 2, (void *)hkRelease, (void **)&o_Release, "IDirect3DDevice9::Release");
    HookVfunc(dev, 16, (void *)hkReset, (void **)&o_Reset, "IDirect3DDevice9::Reset");
    HookVfunc(dev, 17, (void *)hkPresent, (void **)&o_Present, "IDirect3DDevice9::Present");
    HookVfunc(dev, 37, (void *)hkSetRT, (void **)&o_SetRT, "IDirect3DDevice9::SetRenderTarget");
    HookVfunc(dev, 39, (void *)hkSetDS, (void **)&o_SetDS, "IDirect3DDevice9::SetDepthStencilSurface");
    HookVfunc(dev, 43, (void *)hkClear, (void **)&o_Clear, "IDirect3DDevice9::Clear");
    IDirect3DSwapChain9 *sc = NULL;
    if (SUCCEEDED(dev->GetSwapChain(0, &sc)) && sc) {
        HookVfunc(sc, 3, (void *)hkSCPresent, (void **)&o_SCPresent, "IDirect3DSwapChain9::Present");
        sc->Release();
    }
}

// ---- IDirect3D9 ----
static HRESULT STDMETHODCALLTYPE hkCreateDevice(IDirect3D9 *d3d, UINT adapter, D3DDEVTYPE type, HWND focus,
                                                DWORD flags, D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out)
{
    HRESULT hr = o_CreateDevice(d3d, adapter, type, focus, flags, pp, out);
    IDirect3DDevice9 *dev = (SUCCEEDED(hr) && out) ? *out : NULL;
    Log("CreateDevice(adapter=%u type=%d focus=%08X flags=%08X) -> hr=%08X dev=%08X", adapter, type,
        (unsigned)(UINT_PTR)focus, flags, hr, (unsigned)(UINT_PTR)dev);
    if (pp)
        Log("  pp: bb=%ux%u fmt=%d count=%u ms=%d swap=%d hDeviceWindow=%08X windowed=%d depth=%d/%d refresh=%u interval=%08X",
            pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat, pp->BackBufferCount, pp->MultiSampleType,
            pp->SwapEffect, (unsigned)(UINT_PTR)pp->hDeviceWindow, pp->Windowed, pp->EnableAutoDepthStencil,
            pp->AutoDepthStencilFormat, pp->FullScreen_RefreshRateInHz, pp->PresentationInterval);
    if (dev) {
        HWND wnd = focus ? focus : (pp ? pp->hDeviceWindow : NULL);
        HookDevice(dev);
        Overlay_SetDevice(dev, wnd);
        Input_Attach(wnd);
    }
    return hr;
}

static IDirect3D9 *WINAPI hkDirect3DCreate9(UINT sdk)
{
    IDirect3D9 *d3d = o_Direct3DCreate9(sdk);
    Log("Direct3DCreate9(sdk=%u) -> %08X (caller %08X)", sdk, (unsigned)(UINT_PTR)d3d,
        (unsigned)(UINT_PTR)_ReturnAddress());
    if (d3d) HookVfunc(d3d, 16, (void *)hkCreateDevice, (void **)&o_CreateDevice, "IDirect3D9::CreateDevice");
    return d3d;
}

// ---- game functions (task 04, notes/camera_teleport_pc.md) ----
#define PC_GAME_FRAME   0x00B3D3E4u  // __fastcall frame(Darksiders*), called by Darksiders::run 0x00B3D821
#define PC_GAME_CAMMAT  0x004CD9DAu  // Darksiders vt[0x7C] getCameraMatrix(out) = copy of (game+0x138)+0xC0
#define PC_VT_GAME      0x0116E230u
#define PC_SETPAUSED    0x00B3C287u
#define PC_RENDER_VIEW  0x00983AFCu  // task 07 (notes/triggers_pc.md)
#define PC_HUD_PASS     0x00B3B8D2u
// camera A of v0.3 (hooks of StateCamera tick 0x005B8599 / update 0x005B5D33 + writes of cam+0x80/+0xC0) was
// removed in task 05: the user chose B; it is in git history (tag review-04).

bool g_frameHooked;

typedef void(__fastcall *Frame_t)(void *game);
typedef void *(__fastcall *GetMat_t)(void *game, void *edx, float *out);
static Frame_t o_Frame;
static GetMat_t o_GetCamMatrix;

static void __fastcall hkFrame(void *game)
{
    Trig_Frame();    // task 07: trigger snapshot (before the frame, like the flight)
    Flight_Frame();  // between frames: requests, pause, teleport, camera matrix for this frame
    Mon_Frame();     // task 11: freeze monsters / damage info flags follow the menu
    Cheats_Frame();  // task 15: the invincible byte follows the menu; kill all monsters (requested by the menu)
    o_Frame(game);
}

// task 07: 0x00983AFC __thiscall setupView(renderView, camMat*, proj*, fov, aspect, near?, far?) (ret 0x18) - the
// matrices of the frame; 0x00B3B8D2 __fastcall hudPass(game) - 2D/HUD after the 3D scene
typedef void *(__fastcall *RenderView_t)(void *rv, void *edx, const float *cam, const float *proj, float a, float b,
                                         float c, float d);
typedef void(__fastcall *Hud_t)(void *game);
static RenderView_t o_RenderView;
static Hud_t o_Hud;

static void *__fastcall hkRenderView(void *rv, void *edx, const float *cam, const float *proj, float a, float b,
                                     float c, float d)
{
    if (cam && proj) Trig_OnRenderView(cam, proj, _ReturnAddress());
    return o_RenderView(rv, edx, cam, proj, a, b, c, d);
}

static void __fastcall hkHud(void *game)
{
    Trig_OnHud();
    o_Hud(game);
}

// task 09: detector body overlap callbacks (triggers.cpp) - only looked at, for the "trigger fired" log
#define PC_DET_ADD      0x008B82DDu  // __thiscall add(body, collidable), ret 4
#define PC_DET_REMOVE   0x008B815Au  // __thiscall remove(body, ?, obj), ret 8
typedef void(__fastcall *DetAdd_t)(void *body, void *edx, void *collidable);
typedef void(__fastcall *DetRemove_t)(void *body, void *edx, void *a, void *obj);
static DetAdd_t o_DetAdd;
static DetRemove_t o_DetRemove;

static void __fastcall hkDetAdd(void *body, void *edx, void *collidable)
{
    Trig_OnDetectorAdd(body, collidable);
    o_DetAdd(body, edx, collidable);
}

static void __fastcall hkDetRemove(void *body, void *edx, void *a, void *obj)
{
    Trig_OnDetectorRemove(body, obj);
    o_DetRemove(body, edx, a, obj);
}

// task 11: the options commit (copy into the profile block +0x4C) and "changed" (compare with it) - while the menu
// forces DamageIndicators (+0x2F) on, both see the player's own value, so the profile never keeps ours
#define PC_OPT_COMMIT   0x004DA98Bu  // void __fastcall commit(options), options vt[0x3C]
#define PC_OPT_CHANGED  0x004DA84Du  // bool __fastcall changed(options), options vt[0x40]
typedef void(__fastcall *OptCommit_t)(void *opt);
typedef bool(__fastcall *OptChanged_t)(void *opt);
static OptCommit_t o_OptCommit;
static OptChanged_t o_OptChanged;

static void __fastcall hkOptCommit(void *opt)
{
    int prev = Mon_OptionsHide(opt);
    o_OptCommit(opt);
    if (prev >= 0) Mon_OptionsRestore(opt, prev);
}

static bool __fastcall hkOptChanged(void *opt)
{
    int prev = Mon_OptionsHide(opt);
    bool r = o_OptChanged(opt);
    if (prev >= 0) Mon_OptionsRestore(opt, prev);
    return r;
}

// task 15: Monster::applyDamage (Monster vt[0x16C], every monster class; ret 4) - one-hit kill raises the damage of
// a player's hit to the enemy's HP before the game applies it (cheats.cpp)
#define PC_MON_APPLYDMG 0x007B310Fu
typedef int(__fastcall *MonApply_t)(void *mon, void *edx, void *dmg);
static MonApply_t o_MonApply;

static int __fastcall hkMonApply(void *mon, void *edx, void *dmg)
{
    CheatHit h;
    Cheats_MonsterDamage(mon, dmg, &h);
    int r = o_MonApply(mon, edx, dmg);
    if (h.boosted) Cheats_MonsterDamageDone(mon, &h);
    return r;
}

static void *__fastcall hkGetCamMatrix(void *game, void *edx, float *out)
{
    void *r = o_GetCamMatrix(game, edx, out);
    if (out) Flight_RenderMatrix(out, _ReturnAddress());
    return r;
}

// MinHook on exe code, only if the first bytes are what the static analysis saw
static bool HookGame(UINT_PTR addr, const BYTE *pro, size_t n, void *detour, void **orig, const char *name)
{
    if (memcmp((void *)addr, pro, n) != 0) {
        Log("hook %-28s %08X : unexpected bytes -> NOT hooked", name, (unsigned)addr);
        return false;
    }
    MH_STATUS s = MH_CreateHook((void *)addr, detour, orig);
    if (s == MH_OK) s = MH_EnableHook((void *)addr);
    Log("hook %-28s %08X : %s", name, (unsigned)addr, MH_StatusToString(s));
    return s == MH_OK;
}

static void CheckSlot(UINT_PTR vt, int ofs, UINT_PTR expect, const char *name)
{
    UINT_PTR v = ((UINT_PTR *)vt)[ofs / 4];
    if (v != expect) Log("WARNING: %s vt[0x%X] = %08X, expected %08X", name, ofs, (unsigned)v, (unsigned)expect);
}

static void InstallGameHooks()
{
    static const BYTE kFrame[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x1C};
    static const BYTE kGetMat[] = {0x55, 0x8B, 0xEC, 0x8B, 0x89, 0x38, 0x01, 0x00, 0x00};
    CheckSlot(PC_VT_GAME, 0x68, PC_SETPAUSED, "Darksiders");
    CheckSlot(PC_VT_GAME, 0x7C, PC_GAME_CAMMAT, "Darksiders");
    g_frameHooked = HookGame(PC_GAME_FRAME, kFrame, sizeof(kFrame), (void *)hkFrame, (void **)&o_Frame, "game frame");
    if (!g_frameHooked) Log("WARNING: no frame hook -> playerFly runs from Present (camera one frame late)");
    HookGame(PC_GAME_CAMMAT, kGetMat, sizeof(kGetMat), (void *)hkGetCamMatrix, (void **)&o_GetCamMatrix,
             "Darksiders::getCameraMatrix");
    static const BYTE kAlignedPro[] = {0x53, 0x8B, 0xDC, 0x51, 0x51, 0x83, 0xE4, 0xF0};  // both functions start so
    if (!HookGame(PC_RENDER_VIEW, kAlignedPro, sizeof(kAlignedPro), (void *)hkRenderView, (void **)&o_RenderView,
                  "renderView setup (triggers)"))
        Log("WARNING: no view hook -> Show triggers cannot draw");
    if (!HookGame(PC_HUD_PASS, kAlignedPro, sizeof(kAlignedPro), (void *)hkHud, (void **)&o_Hud,
                  "HUD pass (triggers depth)"))
        Log("WARNING: no HUD hook -> triggers depth mode draws at Present");
    static const BYTE kDetAdd[] = {0x6A, 0x00, 0xB8, 0x4E, 0xE1, 0xFF, 0x00};
    static const BYTE kDetRemove[] = {0x55, 0x8B, 0xEC, 0x51, 0x56, 0x57, 0xFF, 0x75, 0x0C};
    CheckSlot(0x0126A020u, 0x14, PC_DET_ADD, "detector body");
    CheckSlot(0x0126A020u, 0x18, PC_DET_REMOVE, "detector body");
    if (!HookGame(PC_DET_ADD, kDetAdd, sizeof(kDetAdd), (void *)hkDetAdd, (void **)&o_DetAdd, "detector enter (log)") ||
        !HookGame(PC_DET_REMOVE, kDetRemove, sizeof(kDetRemove), (void *)hkDetRemove, (void **)&o_DetRemove,
                  "detector exit (log)"))
        Log("WARNING: no detector hooks -> no \"trigger fired\" lines in the log");
    static const BYTE kOptCommit[] = {0x8B, 0x41, 0x4C, 0x8B, 0x51, 0x08, 0x89, 0x10};
    static const BYTE kOptChanged[] = {0x8B, 0x41, 0x4C, 0x8B, 0x10, 0x53, 0x56, 0x3B, 0x51, 0x08};
    if (!HookGame(PC_OPT_COMMIT, kOptCommit, sizeof(kOptCommit), (void *)hkOptCommit, (void **)&o_OptCommit,
                  "options commit (damage info)") ||
        !HookGame(PC_OPT_CHANGED, kOptChanged, sizeof(kOptChanged), (void *)hkOptChanged, (void **)&o_OptChanged,
                  "options changed (damage info)"))
        g_monDamageNoHooks = true;
    static const BYTE kMonApply[] = {0x6A, 0x10, 0x6A, 0x78, 0xB8, 0x0F, 0x57, 0x04, 0x01};  // push 10; push 78; mov eax
    CheckSlot(0x0121D658u, 0x16C, PC_MON_APPLYDMG, "Monster");
    g_cheatHookOk = HookGame(PC_MON_APPLYDMG, kMonApply, sizeof(kMonApply), (void *)hkMonApply, (void **)&o_MonApply,
                             "Monster::applyDamage (one-hit)");
}

bool Hooks_Install()
{
    HMODULE exe = GetModuleHandleA(NULL);
    MH_STATUS s = MH_Initialize();
    Log("MinHook init: %s", MH_StatusToString(s));
    if (s != MH_OK) return false;
    if (g_gameMatch) InstallGameHooks();

    void **slot = FindImportSlot(exe, "d3d9.dll", "Direct3DCreate9");
    Log("IAT Direct3DCreate9 slot=%08X (expected %08X) value=%08X", (unsigned)(UINT_PTR)slot, IAT_DIRECT3DCREATE9,
        slot ? (unsigned)(UINT_PTR)*slot : 0);
    if (!slot || !PatchSlot(slot, (void *)hkDirect3DCreate9, (void **)&o_Direct3DCreate9))
        Log("ERROR: Direct3DCreate9 IAT hook failed -> no overlay");

    slot = FindImportSlot(exe, "user32.dll", "GetRawInputData");
    Log("IAT GetRawInputData slot=%08X (expected %08X) value=%08X", (unsigned)(UINT_PTR)slot, IAT_GETRAWINPUTDATA,
        slot ? (unsigned)(UINT_PTR)*slot : 0);
    if (!slot || !PatchSlot(slot, (void *)Hook_GetRawInputData, (void **)&o_GetRawInputData))
        Log("WARNING: GetRawInputData IAT hook failed -> raw keyboard/mouse not filtered");
    Cursor_InstallHooks(exe);
    return true;
}

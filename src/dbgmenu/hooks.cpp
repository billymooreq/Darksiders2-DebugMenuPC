// Hooks: IAT (Direct3DCreate9, GetRawInputData) -> MinHook on d3d9.dll methods
// (IDirect3D9::CreateDevice, IDirect3DDevice9::Present/Reset/Release, IDirect3DSwapChain9::Present).
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

static bool PatchSlot(void **slot, void *fn, void **orig)
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
    if (++t_presentDepth == 1) Overlay_OnFrame(dev);
    HRESULT hr = o_Present(dev, src, dst, wnd, dirty);
    --t_presentDepth;
    return hr;
}

static HRESULT STDMETHODCALLTYPE hkSCPresent(IDirect3DSwapChain9 *sc, const RECT *src, const RECT *dst, HWND wnd,
                                             const RGNDATA *dirty, DWORD flags)
{
    if (++t_presentDepth == 1) {
        IDirect3DDevice9 *dev = NULL;
        if (SUCCEEDED(sc->GetDevice(&dev)) && dev) {
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
    Overlay_BeforeReset(dev);  // D3DPOOL_DEFAULT resources must be gone before Reset
    HRESULT hr = o_Reset(dev, pp);
    if (LogBudget(&budget, 50))
        Log("Reset(dev=%08X) bb=%ux%u fmt=%d windowed=%d refresh=%u -> hr=%08X", (unsigned)(UINT_PTR)dev,
            pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0, pp ? pp->BackBufferFormat : 0,
            pp ? pp->Windowed : -1, pp ? pp->FullScreen_RefreshRateInHz : 0, hr);
    return hr;
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

bool Hooks_Install()
{
    HMODULE exe = GetModuleHandleA(NULL);
    MH_STATUS s = MH_Initialize();
    Log("MinHook init: %s", MH_StatusToString(s));
    if (s != MH_OK) return false;

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
    return true;
}

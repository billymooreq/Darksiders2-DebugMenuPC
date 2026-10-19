// Overlay: Dear ImGui (DX9 + Win32 backends) drawn into the back buffer right before Present.
// ImGui is used only for drawing: no ImGui mouse/keyboard (the menu has its own key handling), so the
// game's cursor (hidden/clipped in fullscreen) is never touched.
#include "common.h"
#include <math.h>
#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

static IDirect3DDevice9 *s_mainDev;  // last device created by the game (CreateDevice hook)
static HWND s_mainHwnd;
static IDirect3DDevice9 *s_dev;      // device the DX9 backend is bound to (it holds one reference)
static HWND s_hwnd;
static bool s_ctx, s_win32, s_dx9, s_failed;
static float s_fontPx;
static volatile LONG s_frames;
static __declspec(thread) int t_internal;  // ImGui backend calls that may Release() the device

bool Overlay_Internal() { return t_internal != 0; }

void Overlay_SetDevice(IDirect3DDevice9 *dev, HWND hwnd)
{
    s_mainDev = dev;
    s_mainHwnd = hwnd;
}

IDirect3DDevice9 *Overlay_BoundDevice() { return s_dx9 ? s_dev : NULL; }
IDirect3DDevice9 *Overlay_MainDevice() { return s_mainDev; }

static void ApplyXboxStyle()
{
    ImGuiStyle &st = ImGui::GetStyle();
    ImGui::StyleColorsDark(&st);
    st.WindowRounding = 0.0f;
    st.WindowBorderSize = 1.0f;
    st.WindowPadding = ImVec2(10, 8);
    st.ItemSpacing = ImVec2(6, 2);
    st.Colors[ImGuiCol_WindowBg] = ImVec4(0.02f, 0.03f, 0.05f, 1.0f);  // alpha comes from SetNextWindowBgAlpha
    st.Colors[ImGuiCol_Border] = ImVec4(0.45f, 0.50f, 0.55f, 0.60f);
    st.Colors[ImGuiCol_Separator] = ImVec4(0.45f, 0.50f, 0.55f, 0.60f);
    st.Colors[ImGuiCol_Text] = ImVec4(0.92f, 0.92f, 0.92f, 1.0f);
    st.Colors[ImGuiCol_TextDisabled] = ImVec4(0.60f, 0.62f, 0.65f, 1.0f);
}

static void BuildFont(float px)
{
    ImGuiIO &io = ImGui::GetIO();
    char path[MAX_PATH];
    io.Fonts->Clear();
    GetWindowsDirectoryA(path, MAX_PATH);
    strcat_s(path, "\\Fonts\\consola.ttf");
    ImFont *f = NULL;
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) f = io.Fonts->AddFontFromFileTTF(path, px);
    bool consolas = f != NULL;
    if (!f) {
        ImFontConfig cfg;
        cfg.SizePixels = px;
        f = io.Fonts->AddFontDefault(&cfg);
    }
    if (s_dx9) {
        t_internal++;
        ImGui_ImplDX9_InvalidateDeviceObjects();  // font texture is rebuilt by ImGui_ImplDX9_NewFrame
        t_internal--;
    }
    Log("overlay: font %s %.0f px", consolas ? "Consolas" : "ImGui default", px);
    s_fontPx = px;
}

static bool EnsureImGui(IDirect3DDevice9 *dev, UINT height)
{
    if (!s_ctx) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = NULL;  // no imgui.ini in the game folder
        io.LogFilename = NULL;
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange | ImGuiConfigFlags_NoKeyboard;
        io.MouseDrawCursor = false;
        ApplyXboxStyle();
        s_ctx = true;
        Log("overlay: ImGui %s context created", IMGUI_VERSION);
    }
    if (s_win32 && s_hwnd != s_mainHwnd) {
        ImGui_ImplWin32_Shutdown();
        s_win32 = false;
    }
    if (!s_win32) {
        if (!ImGui_ImplWin32_Init(s_mainHwnd)) {
            Log("ERROR: ImGui_ImplWin32_Init failed");
            return false;
        }
        s_win32 = true;
        s_hwnd = s_mainHwnd;
    }
    if (s_dx9 && s_dev != dev) {  // another live device (a dead one is dropped in Overlay_DeviceDying)
        t_internal++;
        ImGui_ImplDX9_Shutdown();
        t_internal--;
        s_dx9 = false;
        Log("overlay: DX9 backend moved from device %08X", (unsigned)(UINT_PTR)s_dev);
    }
    if (!s_dx9) {
        if (!ImGui_ImplDX9_Init(dev)) {
            Log("ERROR: ImGui_ImplDX9_Init failed");
            return false;
        }
        s_dx9 = true;
        s_dev = dev;
        Log("overlay: DX9 backend bound to device %08X", (unsigned)(UINT_PTR)dev);
    }
    float px = floorf((float)height / 45.0f);  // ~24 px at 1080p
    px = px < 14.0f ? 14.0f : (px > 48.0f ? 48.0f : px);
    if (fabsf(px - s_fontPx) > 0.5f) BuildFont(px);
    return true;
}

static void RenderToBackBuffer(IDirect3DDevice9 *dev, IDirect3DSurface9 *bb)
{
    IDirect3DSurface9 *oldRT = NULL, *oldDS = NULL;
    IDirect3DStateBlock9 *sb = NULL;
    D3DVIEWPORT9 vp;
    dev->GetViewport(&vp);
    dev->GetRenderTarget(0, &oldRT);
    dev->GetDepthStencilSurface(&oldDS);
    dev->CreateStateBlock(D3DSBT_ALL, &sb);

    dev->SetRenderTarget(0, bb);  // the game may leave an off-screen target bound
    dev->SetDepthStencilSurface(NULL);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    if (SUCCEEDED(dev->BeginScene())) {
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
        dev->EndScene();
    }

    if (sb) {
        sb->Apply();
        sb->Release();
    }
    dev->SetRenderTarget(0, oldRT);
    dev->SetDepthStencilSurface(oldDS);
    dev->SetViewport(&vp);
    if (oldRT) oldRT->Release();
    if (oldDS) oldDS->Release();
}

static void FrameInner(IDirect3DDevice9 *dev)
{
    int vk;
    if (InterlockedIncrement(&s_frames) == 1) {
        Log("overlay: first Present, render thread %lu", GetCurrentThreadId());
        InterlockedExchange(&g_overlayReady, 1);  // from now on F1 may open the menu
    }
    Input_PerFrame();
    while ((vk = Input_PopKey()) != 0) Menu_HandleKey(vk);
    if (!Menu_WantsDraw()) return;  // nothing on screen -> no work at all
    if (dev->TestCooperativeLevel() != D3D_OK) return;

    IDirect3DSurface9 *bb = NULL;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    D3DSURFACE_DESC d;
    bb->GetDesc(&d);
    if (EnsureImGui(dev, d.Height)) {
        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::GetIO().DisplaySize = ImVec2((float)d.Width, (float)d.Height);  // back buffer, not client rect
        ImGui::NewFrame();
        Trig_Draw2D();  // background draw list: under the menu panels
        Trig_DrawStatus();
        if (g_debugOn) Mon_Draw2D();  // task 11: monster labels (background), target panel, summary line
        Menu_Draw();
        Info_Draw();
        ImGui::Render();
        RenderToBackBuffer(dev, bb);
    }
    bb->Release();
}

static int FrameFilter(EXCEPTION_POINTERS *ep)
{
    Log("ERROR: exception %08X at %08X in overlay frame -> overlay disabled until restart",
        ep->ExceptionRecord->ExceptionCode, (unsigned)(UINT_PTR)ep->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;
}

void Overlay_OnFrame(IDirect3DDevice9 *dev)
{
    static volatile LONG otherDev;
    if (s_failed) return;
    if (dev != s_mainDev) {
        if (LogBudget(&otherDev, 3)) Log("overlay: Present on device %08X (not the game's main device), skipped",
                                         (unsigned)(UINT_PTR)dev);
        return;
    }
    t_internal++;  // releases of back buffer / render target surfaces inside the frame are ours
    __try {
        FrameInner(dev);
    } __except (FrameFilter(GetExceptionInformation())) {
        s_failed = true;  // ImGui state may be half-way through a frame: do not touch it again
        InterlockedExchange(&g_overlayReady, 0);
        Input_SetMenuOpen(false);  // never leave the game without input behind an invisible menu
    }
    t_internal--;
}

void Overlay_FreeResources(IDirect3DDevice9 *dev)
{
    if (!s_dx9 || dev != s_dev) return;
    t_internal++;
    __try {
        ImGui_ImplDX9_InvalidateDeviceObjects();  // recreated lazily by the next ImGui_ImplDX9_NewFrame
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("ERROR: exception %08X in ImGui_ImplDX9_InvalidateDeviceObjects", GetExceptionCode());
    }
    t_internal--;
}

void Overlay_BeforeReset(IDirect3DDevice9 *dev) { Overlay_FreeResources(dev); }

void Overlay_DeviceDying(IDirect3DDevice9 *dev)
{
    if (!s_dx9 || dev != s_dev) return;
    t_internal++;
    __try {
        ImGui_ImplDX9_Shutdown();  // frees VB/IB/font texture and ImGui's device reference
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("ERROR: exception %08X in ImGui_ImplDX9_Shutdown", GetExceptionCode());
    }
    t_internal--;
    s_dx9 = false;
    s_dev = NULL;
    if (dev == s_mainDev) s_mainDev = NULL;
    Log("overlay: game released device %08X -> DX9 backend shut down", (unsigned)(UINT_PTR)dev);
}

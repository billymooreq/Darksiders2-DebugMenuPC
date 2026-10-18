// Offline test of dbgmenu dinput8.dll without the game (DBGMENU_TEST=1).
// Hidden D3D9 window; keys are posted as WM_KEYDOWN/WM_KEYUP; Reset and device re-creation are exercised;
// back buffer snapshots are written as shotN.bmp next to the exe.
// Build: msvc32.bat cl /nologo /EHsc test_d3d.cpp d3d9.lib user32.lib   Run from a folder with dinput8.dll.
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

static HWND g_wnd;
static IDirect3D9 *g_d3d;
static IDirect3DDevice9 *g_dev;
static D3DPRESENT_PARAMETERS g_pp;

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_INPUT) {  // keeps GetRawInputData in the IAT, like the game
        UINT sz = 0;
        GetRawInputData((HRAWINPUT)l, RID_INPUT, NULL, &sz, sizeof(RAWINPUTHEADER));
    }
    if (m == WM_KEYDOWN) printf("  game WndProc got WM_KEYDOWN %02X\n", (unsigned)w);
    return DefWindowProcA(h, m, w, l);
}

static bool CreateDev(UINT w, UINT h)
{
    ZeroMemory(&g_pp, sizeof(g_pp));
    g_pp.Windowed = TRUE;
    g_pp.SwapEffect = D3DSWAPEFFECT_COPY;  // back buffer survives Present -> snapshot after it
    g_pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    g_pp.BackBufferWidth = w;
    g_pp.BackBufferHeight = h;
    g_pp.hDeviceWindow = g_wnd;
    g_pp.EnableAutoDepthStencil = TRUE;
    g_pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    g_pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    HRESULT hr = g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_wnd, D3DCREATE_HARDWARE_VERTEXPROCESSING,
                                     &g_pp, &g_dev);
    printf("CreateDevice %ux%u -> %08lX\n", w, h, hr);
    return SUCCEEDED(hr);
}

static void Snapshot(const char *name)
{
    IDirect3DSurface9 *bb = NULL, *sys = NULL;
    D3DSURFACE_DESC d;
    D3DLOCKED_RECT lr;
    g_dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    bb->GetDesc(&d);
    g_dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, NULL);
    g_dev->GetRenderTargetData(bb, sys);
    sys->LockRect(&lr, NULL, D3DLOCK_READONLY);
    FILE *f = fopen(name, "wb");
    BITMAPFILEHEADER fh = {0x4D42, (DWORD)(54 + d.Width * d.Height * 4), 0, 0, 54};
    BITMAPINFOHEADER ih = {40, (LONG)d.Width, -(LONG)d.Height, 1, 32, BI_RGB};
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    for (UINT y = 0; y < d.Height; y++) fwrite((BYTE *)lr.pBits + y * lr.Pitch, 4, d.Width, f);
    fclose(f);
    sys->UnlockRect();
    sys->Release();
    bb->Release();
    printf("snapshot %s %ux%u\n", name, d.Width, d.Height);
}

static void Key(int vk)
{
    PostMessageA(g_wnd, WM_KEYDOWN, vk, 1);
    PostMessageA(g_wnd, WM_KEYUP, vk, 0xC0000001);
}

int main()
{
    setvbuf(stdout, NULL, _IONBF, 0);
    SetEnvironmentVariableA("DBGMENU_TEST", "1");
    HMODULE dll = LoadLibraryA(".\\dinput8.dll");
    printf("dinput8.dll %p\n", (void *)dll);
    if (!dll) return 1;

    WNDCLASSEXA wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "dbgmenu_test";
    RegisterClassExA(&wc);
    g_wnd = CreateWindowExA(0, "dbgmenu_test", "dbgmenu test", WS_OVERLAPPEDWINDOW, 0, 0, 1280, 720, NULL, NULL,
                            wc.hInstance, NULL);  // never shown: does not disturb the desktop
    g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_d3d || !CreateDev(1280, 720)) return 2;

    for (int frame = 1; frame <= 140; frame++) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        switch (frame) {
        case 3: Key('W'); break;                            // menu closed: must reach the game
        case 5: Key(VK_F1); break;                          // open
        case 8: Key(VK_DOWN); break;                        // -> Camera
        case 11: Key(VK_RETURN); break;                     // enter category
        case 14: Key(VK_RETURN); break;                     // showCameraPosition on
        case 16: Key('W'); break;                           // menu open: must NOT reach the game
        case 25: Snapshot("shot1.bmp"); break;
        case 27: Key(VK_ESCAPE); Key(VK_DOWN); Key(VK_DOWN); Key(VK_RETURN); Key(VK_LEFT); Key(VK_LEFT); break;  // Menu > opacity -0.10
        case 30:
            g_pp.BackBufferWidth = 1024;
            g_pp.BackBufferHeight = 768;
            printf("Reset -> %08lX\n", g_dev->Reset(&g_pp));
            break;
        case 45: Snapshot("shot2.bmp"); break;
        case 50: Key(VK_ESCAPE); Key(VK_ESCAPE); Key(VK_F2); break;  // close menu, F2 toast
        case 55: Snapshot("shot3.bmp"); break;
        case 60: {
            ULONG r = g_dev->Release();
            printf("device released, refcount %lu\n", r);
            g_dev = NULL;
            if (!CreateDev(800, 600)) return 3;
            break;
        }
        case 100: Key(VK_F1); break;
        case 110: Snapshot("shot4.bmp"); break;
        case 115: Key(VK_F1); break;
        }
        g_dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(60, 90, 130), 1.0f, 0);
        g_dev->BeginScene();
        g_dev->EndScene();
        g_dev->Present(NULL, NULL, NULL, NULL);
        Sleep(10);
    }
    printf("final device release refcount %lu\n", g_dev->Release());
    g_d3d->Release();
    DestroyWindow(g_wnd);
    printf("done\n");
    return 0;
}

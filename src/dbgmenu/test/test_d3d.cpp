// Offline test of dbgmenu dinput8.dll without the game (DBGMENU_TEST=1).
// Hidden D3D9 window; keys are posted as WM_KEYDOWN/WM_KEYUP; Reset and device re-creation are exercised;
// back buffer snapshots are written as shotN.bmp next to the exe.
// Task 05: the DLL simulates the system cursor in test mode (the desktop cursor is never clipped or hidden);
// ShowCursor / ClipCursor called here play the game's own cursor calls (IAT-hooked like in the game).
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
    if (m == WM_KEYUP) printf("  game WndProc got WM_KEYUP %02X\n", (unsigned)w);
    if (m == WM_LBUTTONDOWN) printf("  game WndProc got WM_LBUTTONDOWN\n");
    if (m == WM_RBUTTONDOWN) printf("  game WndProc got WM_RBUTTONDOWN\n");
    if (m == WM_MOUSEMOVE) printf("  game WndProc got WM_MOUSEMOVE\n");
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
static void KeyDown(int vk) { PostMessageA(g_wnd, WM_KEYDOWN, vk, 1); }
static void KeyUp(int vk) { PostMessageA(g_wnd, WM_KEYUP, vk, 0xC0000001); }
static void Mouse(int x, int y) { PostMessageA(g_wnd, WM_MOUSEMOVE, 0, MAKELPARAM(x, y)); }
static void Click() { PostMessageA(g_wnd, WM_LBUTTONDOWN, MK_LBUTTON, 0); PostMessageA(g_wnd, WM_LBUTTONUP, 0, 0); }
static void Focus(bool on)
{
    PostMessageA(g_wnd, WM_ACTIVATEAPP, on, 0);
    PostMessageA(g_wnd, on ? WM_SETFOCUS : WM_KILLFOCUS, 0, 0);
}

typedef void(__cdecl *DbgMenuTest_t)(int);
static DbgMenuTest_t g_ctl;
static void Ctl(int cmd)
{
    printf("-- DbgMenuTest(%d)\n", cmd);
    if (g_ctl) g_ctl(cmd);
}
static void Say(const char *s) { printf("== %s\n", s); }

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

    g_ctl = (DbgMenuTest_t)GetProcAddress(dll, "DbgMenuTest");
    printf("DbgMenuTest export %p\n", (void *)g_ctl);
    for (int frame = 1; frame <= 520 + 110; frame++) {
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
        // ---- task 04/05: playerFly ----
        case 125: Say("F2 before the level is loaded: unavailable"); Key(VK_F2); break;
        case 130: Ctl(1); Ctl(6); break;
        case 135: Say("F2: enter flight"); Key(VK_F2); break;
        case 137: Say("cursor in flight: hidden + clipped to the centre box"); Ctl(6); break;
        case 138: {
            Say("the game's own ShowCursor(TRUE) / ClipCursor(NULL) while captured: only recorded");
            int r = ShowCursor(TRUE);
            BOOL c = ClipCursor(NULL);
            printf("  ShowCursor(TRUE) -> %d, ClipCursor(NULL) -> %d\n", r, c);
            Ctl(6);
            break;
        }
        case 140: Say("hold W (must not reach the game)"); KeyDown('W'); break;
        case 150: KeyUp('W'); break;
        case 153: Say("mouse right 50 px"); Mouse(100, 100); break;
        case 154: Mouse(150, 100); break;
        case 158: Say("Shift+D"); KeyDown(VK_SHIFT); KeyDown('D'); break;
        case 165: KeyUp('D'); KeyUp(VK_SHIFT); break;
        case 167: Snapshot("shot5.bmp"); break;
        case 170: Say("Alt+Tab away while W is held: flight frozen, cursor released, W forgotten"); KeyDown('W'); break;
        case 175: Focus(false); break;  // the W release goes to another window: never posted here
        case 177: Ctl(6); break;
        case 180: Snapshot("shot6.bmp"); break;
        case 190: Say("focus back: flight resumes, W not held"); Focus(true); break;
        case 191: Say("click right after the focus came back: ignored"); Click(); break;
        case 193: Ctl(6); break;
        case 200: Say("device Reset in flight: the flight goes on"); printf("Reset -> %08lX\n", g_dev->Reset(&g_pp)); break;
        case 230: Say("Enter: teleport, camera reset + blend, hover"); Key(VK_RETURN); break;
        case 260: Say("hovering: menu open + W must not release it"); Key(VK_F1); break;
        case 262: KeyDown('W'); break;
        case 264: KeyUp('W'); break;
        case 266: Key(VK_F1); break;
        case 268: Snapshot("shot7.bmp"); break;
        case 270: Say("Space: hover released, Space reaches the game, falls from vz 0"); KeyDown(VK_SPACE); break;
        case 272: KeyUp(VK_SPACE); break;
        case 310: Say("W after the hover must reach the game; cursor back to the game's state"); Key('W'); Ctl(6); break;
        case 315: Say("enter, hold A, F2 exit, release A (release must be swallowed)"); Key(VK_F2); break;
        case 318: KeyDown('A'); break;
        case 321: Key(VK_F2); break;
        case 324: KeyUp('A'); break;
        case 330: Say("enter, teleport by a held left button -> hover; its release does nothing; right button releases"); Key(VK_F2); break;
        case 334: PostMessageA(g_wnd, WM_LBUTTONDOWN, MK_LBUTTON, 0); break;
        case 345: PostMessageA(g_wnd, WM_LBUTTONUP, 0, 0); break;
        case 350: PostMessageA(g_wnd, WM_RBUTTONDOWN, MK_RBUTTON, 0); break;
        case 351: PostMessageA(g_wnd, WM_RBUTTONUP, 0, 0); break;
        case 360: Say("enter + teleport -> hover; the game moves the player (respawn): hover stops"); Key(VK_F2); break;
        case 363: Key(VK_RETURN); break;
        case 370: Ctl(7); break;
        case 380: Say("enter + teleport -> hover; F2 -> flight again, F2 -> exit"); Key(VK_F2); break;
        case 383: Key(VK_RETURN); break;
        case 390: Key(VK_F2); break;
        case 395: Key(VK_F2); break;
        case 400: Say("menu: General -> hover after teleport -> off"); Key(VK_F1); Key(VK_DOWN); Key(VK_RETURN); Key(VK_DOWN); Key(VK_RETURN); break;
        case 402: Snapshot("shot8.bmp"); break;
        case 404: Key(VK_F1); break;
        case 410: Say("hover off: teleport -> falls at once"); Key(VK_F2); break;
        case 413: Key(VK_RETURN); break;
        case 450: Say("enter, exception in the flight frame -> emergency exit"); Key(VK_F2); break;
        case 452: Ctl(5); break;
        case 460: Say("enter, player lost -> emergency exit"); Key(VK_F2); break;
        case 462: Ctl(2); break;
        case 470: Say("F2 with no player: unavailable"); Key(VK_F2); break;
        case 472: Ctl(1); break;
        case 475: Say("enter, menu open in flight, W held (not to the game, flight not controlled)"); Key(VK_F2); break;
        case 477: Key(VK_F1); break;
        case 480: KeyDown('W'); break;
        case 485: KeyUp('W'); break;
        case 487: Snapshot("shot9.bmp"); break;
        case 490: Key(VK_F1); break;
        case 495: Say("left click: teleport + exit (hover off)"); Click(); break;
        case 505: Say("click after the flight must reach the game"); Click(); break;
        case 510: Say("final cursor state: released"); Ctl(6); break;
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

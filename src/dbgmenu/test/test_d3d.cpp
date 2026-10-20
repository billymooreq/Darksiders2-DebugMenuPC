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
    if (m == WM_SYSKEYDOWN) printf("  game WndProc got WM_SYSKEYDOWN %02X\n", (unsigned)w);
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
static void SysKey(int vk)  // F10 comes as WM_SYSKEYDOWN in Windows
{
    PostMessageA(g_wnd, WM_SYSKEYDOWN, vk, 1);
    PostMessageA(g_wnd, WM_SYSKEYUP, vk, 0xC0000001);
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

// task 07: a dark "wall" over the left half of the screen at depth 0.5 (writes the depth buffer)
static void DrawOccluder()
{
    struct V { float x, y, z, rhw; DWORD c; };
    float w = (float)g_pp.BackBufferWidth * 0.5f, h = (float)g_pp.BackBufferHeight;
    V v[4] = {{0, 0, 0.5f, 1, 0xFF283C50}, {w, 0, 0.5f, 1, 0xFF283C50}, {0, h, 0.5f, 1, 0xFF283C50},
              {w, h, 0.5f, 1, 0xFF283C50}};
    g_dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    g_dev->SetRenderState(D3DRS_ZENABLE, TRUE);
    g_dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    g_dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    g_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_dev->SetTexture(0, NULL);
    g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    g_dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(V));
}

typedef void(__cdecl *DbgMenuTest_t)(int);
static DbgMenuTest_t g_ctl;
static void Ctl(int cmd)
{
    printf("-- DbgMenuTest(%d)\n", cmd);
    if (g_ctl) g_ctl(cmd);
}
static void Say(const char *s) { printf("== %s\n", s); }

// task 10: the keys file next to the DLL (= the current folder)
static const char *kKeysFile = "dbgmenu_keys.ini";
static void PrintFile()
{
    char line[256];
    FILE *f = fopen(kKeysFile, "r");
    printf("  --- %s ---\n", kKeysFile);
    if (!f) {
        printf("  (missing)\n");
        return;
    }
    while (fgets(line, sizeof(line), f))
        if (line[0] != ';') printf("  | %s", line);
    fclose(f);
}
static void WriteFile(const char *text)
{
    FILE *f = fopen(kKeysFile, "w");
    fputs(text, f);
    fclose(f);
    printf("  wrote %s:\n%s", kKeysFile, text);
}

int main()
{
    setvbuf(stdout, NULL, _IONBF, 0);
    SetEnvironmentVariableA("DBGMENU_TEST", "1");
    HMODULE dll = LoadLibraryA(".\\dinput8.dll");
    printf("dinput8.dll %p\n", (void *)dll);
    if (!dll) return 1;
    Say("10a: keys file after the DLL loaded (created with the defaults if it was missing)");
    PrintFile();

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
    for (int frame = 1; frame <= 1862; frame++) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        switch (frame) {
        // ---- task 10: the debug menu starts OFF: F1/F2/F3 reach the game, nothing on screen; F10 turns it on ----
        case 1: Say("10a: debug off: F1, F2, F3 must reach the game"); Key(VK_F1); Key(VK_F2); Key(VK_F3); break;
        case 2: Snapshot("shot0.bmp"); break;
        case 3: Key('W'); break;                            // menu closed: must reach the game
        case 4: Say("10a: F10 (WM_SYSKEYDOWN): debug on, not to the game"); SysKey(VK_F10); break;
        case 5: Key(VK_F1); break;                          // open
        case 8: Key(VK_DOWN); Key(VK_DOWN); Key(VK_DOWN); break;  // playerFly -> Triggers -> Monsters (11) -> Camera
        case 11: Key(VK_RETURN); break;                     // enter category
        case 14: Key(VK_RETURN); break;                     // showCameraPosition on
        case 16: Key('W'); break;                           // menu open: must NOT reach the game
        case 25: Snapshot("shot1.bmp"); break;
        case 27: Key(VK_ESCAPE); Key(VK_DOWN); Key(VK_RETURN); Key(VK_LEFT); Key(VK_LEFT); break;  // Menu > opacity -0.10
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
        case 400: Say("menu: playerFly -> hover after teleport -> off"); Key(VK_F1); Key(VK_DOWN); Key(VK_DOWN); Key(VK_RETURN); Key(VK_DOWN); Key(VK_DOWN); Key(VK_DOWN); Key(VK_DOWN); Key(VK_RETURN); break;  // 09: 3 new items before it; 10: Menu -> Keys -> playerFly
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
        // ---- task 06: jump from the hover (the fake game: ground / fall with a second chance / jump / double) ----
        // Space is held 2 frames: the fake game and our air phase look at held keys, not at the message queue
        case 520: Say("menu: hover after teleport -> on, select hover jump (game)"); Key(VK_F1); Key(VK_RETURN); Key(VK_DOWN); break;
        case 522: Key(VK_F1); break;
        case 525: Say("06a: enter, fly up, teleport -> hover high above the ground"); Key(VK_F2); break;
        case 528: KeyDown(VK_SPACE); break;
        case 560: KeyUp(VK_SPACE); break;
        case 563: Key(VK_RETURN); break;
        case 580: Say("06a: hovering, Space -> the game jumps from the hover"); KeyDown(VK_SPACE); break;
        case 582: KeyUp(VK_SPACE); break;
        case 600: Say("06a: in the jump, Space -> double jump"); KeyDown(VK_SPACE); break;
        case 602: KeyUp(VK_SPACE); break;
        case 660: Say("06b: hover, W releases -> falls; Space in the fall -> jump; Space -> double jump"); Key(VK_F2); break;
        case 663: KeyDown(VK_SPACE); break;
        case 695: KeyUp(VK_SPACE); break;
        case 698: Key(VK_RETURN); break;
        case 710: KeyDown('W'); break;
        case 712: KeyUp('W'); break;
        case 725: KeyDown(VK_SPACE); break;
        case 727: KeyUp(VK_SPACE); break;
        case 740: KeyDown(VK_SPACE); break;
        case 742: KeyUp(VK_SPACE); break;
        case 800: Say("menu: hover jump -> direct"); Key(VK_F1); Key(VK_RIGHT); break;
        case 802: Key(VK_F1); break;
        case 805: Say("06c: direct: hover, Space -> our changeState one frame later"); Key(VK_F2); break;
        case 808: KeyDown(VK_SPACE); break;
        case 840: KeyUp(VK_SPACE); break;
        case 843: Key(VK_RETURN); break;
        case 855: KeyDown(VK_SPACE); break;
        case 857: KeyUp(VK_SPACE); break;
        case 875: Say("06c: direct: in the jump, Space -> double jump (the game)"); KeyDown(VK_SPACE); break;
        case 877: KeyUp(VK_SPACE); break;
        case 930: Say("06d: direct: hover, W -> falls; Space in the fall -> our jump; Space -> double"); Key(VK_F2); break;
        case 933: KeyDown(VK_SPACE); break;
        case 965: KeyUp(VK_SPACE); break;
        case 968: Key(VK_RETURN); break;
        case 980: KeyDown('W'); break;
        case 982: KeyUp('W'); break;
        case 995: KeyDown(VK_SPACE); break;
        case 997: KeyUp(VK_SPACE); break;
        case 1010: KeyDown(VK_SPACE); break;
        case 1012: KeyUp(VK_SPACE); break;
        case 1070: Say("06e: F2 during the air phase: no jump help after the flight"); Key(VK_F2); break;
        case 1073: KeyDown(VK_SPACE); break;
        case 1100: KeyUp(VK_SPACE); break;
        case 1103: Key(VK_RETURN); break;
        case 1110: KeyDown('W'); break;
        case 1112: KeyUp('W'); break;
        case 1115: Key(VK_F2); break;
        case 1120: Key(VK_F2); break;
        case 1140: Say("menu: hover jump -> game"); Key(VK_F1); Key(VK_LEFT); break;
        case 1142: Snapshot("shot10.bmp"); break;
        case 1144: Key(VK_F1); break;
        // ---- task 07: Show triggers (fake EventManager in triggers.cpp, camera back at (100 200 300) looking +X) ----
        // from frame 1150 the "game" draws a depth occluder over the left half (z = 0.5) before Present
        case 1150: Say("07a: F3 -> triggers on, through walls"); Ctl(12); Key(VK_F3); break;
        case 1156: Ctl(8); break;
        case 1158: Snapshot("shot11.bmp"); break;
        case 1160: Say("07b: playerFly: strafe left + mouse; the triggers stay in the world"); Key(VK_F2); break;
        case 1163: KeyDown('A'); Mouse(300, 300); break;
        case 1164: Mouse(330, 290); break;
        case 1175: KeyUp('A'); Ctl(8); break;
        case 1177: Snapshot("shot12.bmp"); break;
        case 1180: Key(VK_F2); break;
        case 1183: Say("07c: menu Triggers -> depth: depth"); Ctl(12); Key(VK_F1); Key(VK_ESCAPE); Key(VK_DOWN); break;
        case 1185: Key(VK_RETURN); Key(VK_DOWN); Key(VK_RIGHT); break;
        case 1187: Snapshot("shot13.bmp"); break;
        case 1188: Key(VK_F1); break;
        case 1195: Snapshot("shot14.bmp"); break;
        case 1198: Say("07d: device Reset in depth mode"); printf("Reset -> %08lX\n", g_dev->Reset(&g_pp)); break;
        case 1205: Snapshot("shot15.bmp"); break;
        case 1208: Say("07e: empty list, broken list, normal"); Ctl(9); break;
        case 1211: Ctl(8); Ctl(10); break;
        case 1214: Ctl(8); Ctl(11); break;
        case 1217: Ctl(8); break;
        case 1220: Say("07f: show disabled off"); Key(VK_F1); Key(VK_DOWN); Key(VK_LEFT); break;
        case 1222: Key(VK_F1); break;
        case 1226: Snapshot("shot16.bmp"); break;
        case 1228: Say("07f: show disabled on, back to through walls"); Key(VK_F1); Key(VK_RIGHT); Key(VK_UP); Key(VK_LEFT); break;
        case 1230: Key(VK_F1); break;
        case 1234: Say("07g: exception while collecting -> triggers off"); Ctl(13); break;
        case 1238: Say("07g: F3 -> on again"); Key(VK_F3); break;
        case 1242: Snapshot("shot17.bmp"); Ctl(8); break;
        case 1245: Say("07h: 8000 fake triggers, through walls (timing)"); Ctl(14); break;
        case 1250: Ctl(8); Snapshot("shot18.bmp"); break;
        case 1252: Say("07h: 8000 fake triggers, depth"); Key(VK_F1); Key(VK_RIGHT); break;
        case 1253: Key(VK_F1); break;
        case 1256: Ctl(8); Snapshot("shot19.bmp"); break;
        case 1258: Ctl(11); Key(VK_F1); Key(VK_LEFT); break;
        case 1259: Key(VK_F1); break;
        case 1262: Say("07i: F3 -> off"); Key(VK_F3); break;
        case 1266: Snapshot("shot20.bmp"); break;
        // ---- task 08: menu grouped by feature (playerFly / Triggers / Camera / Menu), cursor remembered ----
        case 1270: Say("08a: F1 opens where it was closed: Triggers > depth"); Key(VK_F1); break;
        case 1272: Snapshot("shot21.bmp"); break;
        case 1274: Say("08b: Esc -> the four sections"); Key(VK_ESCAPE); break;
        case 1276: Snapshot("shot22.bmp"); break;
        case 1278: Say("08c: playerFly section (remembered item: hover jump)"); Key(VK_UP); Key(VK_RETURN); break;
        case 1280: Snapshot("shot23.bmp"); break;
        case 1282: Say("08c: playerFly -> playerFly: Enter enters the flight"); Key(VK_DOWN); Key(VK_RETURN); break;  // 09: hover jump is the last of 6
        case 1286: Snapshot("shot24.bmp"); break;
        case 1288: Say("08c: Enter again: flight off"); Key(VK_RETURN); break;
        case 1292: Say("08d: Camera section: showCameraPosition off, on"); Key(VK_ESCAPE); Key(VK_DOWN); Key(VK_DOWN); Key(VK_DOWN); Key(VK_RETURN); Key(VK_RETURN); break;  // 11: + Monsters
        case 1294: Snapshot("shot25.bmp"); break;
        case 1296: Key(VK_RETURN); break;
        case 1298: Say("08e: Menu section: opacity +0.05, text scale +0.25"); Key(VK_ESCAPE); Key(VK_DOWN); Key(VK_RETURN); Key(VK_RIGHT); Key(VK_DOWN); Key(VK_RIGHT); break;
        case 1300: Snapshot("shot26.bmp"); break;
        case 1302: Say("08e: back to opacity 0.70, text 1.00"); Key(VK_LEFT); Key(VK_UP); Key(VK_LEFT); break;
        case 1304: Say("08f: Triggers section: Show triggers by Enter"); Key(VK_ESCAPE); Key(VK_UP); Key(VK_UP); Key(VK_UP); Key(VK_RETURN); Key(VK_UP); Key(VK_RETURN); break;
        case 1308: Ctl(8); Snapshot("shot27.bmp"); break;
        case 1310: Say("08f: F3 with the menu open: off"); Key(VK_F3); break;
        case 1312: Snapshot("shot28.bmp"); break;
        case 1314: Say("08g: Esc Esc closes; F1 opens at the section list on Triggers"); Key(VK_ESCAPE); Key(VK_ESCAPE); break;
        case 1316: Key(VK_F1); break;
        case 1318: Snapshot("shot29.bmp"); break;
        case 1320: Key(VK_F1); break;
        // ---- task 09: time in flight (T, menu), the player carried in front of the camera (R, menu) ----
        case 1325: Say("09a: F2: time runs by default (pause 0 -> 0), player free"); Key(VK_F2); break;
        case 1330: Snapshot("shot30.bmp"); break;
        case 1332: Say("09a: T -> time frozen (pause 0 -> 1); T reaches no game"); Key('T'); break;
        case 1335: Snapshot("shot31.bmp"); break;
        case 1337: Say("09a: T -> time runs (1 -> 0)"); Key('T'); break;
        case 1340: Say("09b: R -> the player carried in front of the camera"); Key('R'); break;
        case 1343: Snapshot("shot32.bmp"); break;
        case 1345: Say("09b: W held: the camera flies, the player follows"); KeyDown('W'); break;
        case 1365: KeyUp('W'); break;
        case 1367: Say("09c: menu in flight: attach distance 200 -> 400 (the player catches up by 48 per frame)");
            Key(VK_F1); Key(VK_UP); Key(VK_RETURN); Key(VK_DOWN); Key(VK_DOWN); Key(VK_DOWN);
            Key(VK_RIGHT); Key(VK_RIGHT); Key(VK_RIGHT); Key(VK_RIGHT); break;
        case 1369: Snapshot("shot33.bmp"); break;
        case 1371: Key(VK_F1); break;
        case 1380: Say("09d: Alt+Tab while carried: frozen, held"); Focus(false); break;
        case 1385: Focus(true); break;
        case 1395: Say("09e: the game moves the player by itself -> let go, flight goes on"); Ctl(7); break;
        case 1400: Snapshot("shot34.bmp"); break;
        case 1403: Say("09f: R again, F2: the player stays where he was carried, hover, camera behind him"); Key('R'); break;
        case 1406: Key(VK_F2); break;
        case 1412: Snapshot("shot35.bmp"); break;
        case 1415: Say("09f: Space releases the hover"); KeyDown(VK_SPACE); break;
        case 1417: KeyUp(VK_SPACE); break;
        case 1440: Say("09g: menu: attach distance back to 200, freeze time in flight on");
            Key(VK_F1); Key(VK_LEFT); Key(VK_LEFT); Key(VK_LEFT); Key(VK_LEFT); Key(VK_UP); Key(VK_UP); Key(VK_RETURN); break;
        case 1442: Key(VK_F1); break;
        case 1445: Say("09g: F2: the flight starts frozen (pause 0 -> 1)"); Key(VK_F2); break;
        case 1448: Snapshot("shot36.bmp"); break;
        case 1450: Say("09g: menu in flight: freeze time in flight off -> time runs at once"); Key(VK_F1); Key(VK_RETURN); break;
        case 1452: Key(VK_F1); break;
        case 1455: Say("09h: T frozen, R carried, Enter: (v0.8.1) the carried player stays there, pause back to 0"); Key('T'); break;
        case 1458: Key('R'); break;
        case 1461: Key(VK_RETURN); break;
        case 1470: KeyDown(VK_SPACE); break;
        case 1472: KeyUp(VK_SPACE); break;
        case 1480: Say("09i: F2, T, R, player lost -> emergency exit, pause 1 -> 0"); Key(VK_F2); break;
        case 1483: Key('T'); break;
        case 1485: Key('R'); break;
        case 1488: Ctl(2); break;
        case 1492: Ctl(1); break;
        case 1495: Say("09j: F2, T, exception in the flight frame -> emergency exit, pause 1 -> 0"); Key(VK_F2); break;
        case 1498: Key('T'); break;
        case 1500: Ctl(5); break;
        case 1505: Say("09k: detector enter/exit log (fake body)"); Ctl(15); break;
        case 1510: Say("09l: T / R outside the flight reach the game"); Key('T'); Key('R'); break;
        case 1515: Say("09m: F2, R, left click: (v0.8.1) the carried player stays there"); Key(VK_F2); break;
        case 1518: Key('R'); break;
        case 1522: Snapshot("shot37.bmp"); break;
        case 1524: Click(); break;
        case 1530: Key(VK_SPACE); break;
        case 1535: Say("cursor state: released"); Ctl(6); break;
        // ---- task 10: F10 master switch, menu Keys, dbgmenu_keys.ini ----
        case 1540: Say("10b: showCameraPosition on, F3 triggers on, F2 flight, R carried, F1 menu open");
            Key(VK_F1); Key(VK_ESCAPE); Key(VK_DOWN); Key(VK_DOWN); Key(VK_DOWN); Key(VK_RETURN); Key(VK_RIGHT); break;
        case 1542: Key(VK_F1); break;
        case 1543: Key(VK_F3); break;
        case 1545: Key(VK_F2); break;
        case 1548: Key('R'); break;
        case 1551: Key(VK_F1); break;
        case 1553: Snapshot("shot38.bmp"); break;
        case 1555: Say("10b: F10 in flight: menu closed, the carried player stays (hover), triggers + panel off"); SysKey(VK_F10); break;
        case 1558: Snapshot("shot39.bmp"); break;
        case 1560: Say("10b: debug off: F1, F2, F3 reach the game, no flight, the hover stays"); Key(VK_F1); Key(VK_F2); Key(VK_F3); break;
        case 1562: Say("10b: T is a game key now: reaches the game and releases the hover"); Key('T'); break;
        case 1566: Snapshot("shot40.bmp"); break;
        case 1570: Say("10c: F10 on: only the label; triggers / panel stay off"); SysKey(VK_F10); break;
        case 1573: Snapshot("shot41.bmp"); break;
        case 1575: Say("10d: menu Keys > menu: Enter -> waiting for a key");
            Key(VK_F1); Key(VK_ESCAPE); Key(VK_DOWN); Key(VK_DOWN); Key(VK_RETURN); Key(VK_DOWN); Key(VK_RETURN); break;
        case 1578: Snapshot("shot42.bmp"); break;
        case 1580: Say("10d: Space, W, left click: key not allowed"); Key(VK_SPACE); break;
        case 1582: Key('W'); break;
        case 1584: Click(); break;
        case 1586: Snapshot("shot43.bmp"); break;
        case 1588: Say("10d: F6 -> menu = F6"); Key(VK_F6); break;
        case 1590: Snapshot("shot44.bmp"); break;
        case 1592: Say("10d: F6 closes the menu; F1 now reaches the game"); Key(VK_F6); Key(VK_F1); break;
        case 1595: Say("10d: Keys > playerFly = F6: conflict, swapped (menu = F2)"); Key(VK_F6); Key(VK_DOWN); Key(VK_RETURN); break;
        case 1597: Key(VK_F6); break;
        case 1599: Snapshot("shot45.bmp"); break;
        case 1601: Say("10d: attach = Y, time = R"); Key(VK_DOWN); Key(VK_DOWN); Key(VK_RETURN); break;
        case 1603: Key('Y'); break;
        case 1604: Key(VK_DOWN); Key(VK_RETURN); break;
        case 1606: Key('R'); break;
        case 1608: Say("10e: the file after the changes"); PrintFile(); break;
        case 1610: Say("10e: F2 closes the menu, F6 flight, Y carried, T nothing, R time frozen"); Key(VK_F2); break;
        case 1612: Key(VK_F6); break;
        case 1615: Key('Y'); break;
        case 1617: Key('T'); break;
        case 1618: Key('R'); break;
        case 1620: Snapshot("shot46.bmp"); break;
        case 1622: Say("10e: F6: the carried player stays, hover"); Key(VK_F6); break;
        case 1624: Snapshot("shot47.bmp"); break;
        case 1626: KeyDown(VK_SPACE); break;
        case 1628: KeyUp(VK_SPACE); break;
        case 1630: Say("10f: a broken file, read again (cmd 16)");
            WriteFile("; test\ndebug = F11\nmenu = Space\nplayerFly = F7\nshowTriggers = f7\nattach = Blah\n"
                      "this line is garbage\ntime = q ; comment\ntime = Z\nunknown = F9\n");
            Ctl(16); PrintFile(); break;
        case 1632: Say("10f: F10 is no longer ours -> the game; F11 off, F11 on"); SysKey(VK_F10); break;
        case 1634: Key(VK_F11); break;
        case 1635: Key(VK_F1); break;
        case 1636: Key(VK_F11); break;
        case 1640: Say("10g: file with only menu = F2: playerFly gets the free default F1");
            WriteFile("menu = F2\n"); Ctl(16); break;
        case 1643: Say("10g: F2 opens the menu; Keys > reset to defaults"); Key(VK_F2); Key(VK_DOWN); Key(VK_DOWN); Key(VK_RETURN); break;  // 11: + info: target
        case 1645: Snapshot("shot48.bmp"); break;
        case 1647: Key(VK_F1); PrintFile(); break;
        case 1650: Say("10g: read again: no problems"); Ctl(16); break;
        case 1652: Say("10h: F10 off, F1 reaches the game"); SysKey(VK_F10); break;
        case 1654: Key(VK_F1); break;
        case 1660: Say("final cursor state: released"); Ctl(6); break;
        // ---- task 11: section Monsters (fake actors in monsters.cpp, placed in front of the camera by cmd 17:
        // Skel_A01 straight ahead 600 units, Ghoul_07 / Ghoul_08 side by side (their labels overlap), Skel_Far
        // 4500 units (45 m), Behind_01 behind the camera, Vulgrim (NPC), Death (player), Coin_03 (not a character),
        // one bad object). The menu keys are read by the next overlay frame: F1 that closes the menu comes a frame
        // later. ----
        case 1665: Say("11a: debug off: F5 (info: target) reaches the game"); Key(VK_F5); break;
        case 1667: SysKey(VK_F10); break;
        case 1669: Ctl(17); break;
        case 1670: Say("11b: six sections, Monsters after Triggers"); Key(VK_F1); Key(VK_ESCAPE); break;
        case 1672: Snapshot("shot49.bmp"); break;
        case 1673: Key(VK_UP); Key(VK_UP); Key(VK_UP); Key(VK_RETURN); break;  // Keys -> Menu -> Camera -> Monsters
        case 1675: Snapshot("shot50.bmp"); break;
        case 1676: Say("11b: info: nearby on (level 1, 30 m)"); Key(VK_RETURN); break;
        case 1677: Key(VK_F1); break;
        case 1680: Ctl(18); Snapshot("shot51.bmp"); break;
        // task 12: Monsters = nearby, target, range, show, Display >, damage, freeze; Display = name, class, HP,
        // level, stats, move, AI, cooldowns, target, distance, pos, hits, all on, all off (no info level any more)
        case 1682: Say("11b: Display > all on (was info level 4)"); Key(VK_F1); Key(VK_DOWN); Key(VK_DOWN); Key(VK_DOWN); Key(VK_DOWN); Key(VK_RETURN); Key(VK_UP); Key(VK_UP); Key(VK_RETURN); Key(VK_ESCAPE); break;
        case 1683: Key(VK_F1); break;
        case 1686: Snapshot("shot52.bmp"); Ctl(18); break;
        case 1688: Say("11c: nearby range 30 m -> unlimited: Skel_Far gets a label"); Key(VK_F1); Key(VK_UP); Key(VK_UP); Key(VK_RIGHT); Key(VK_RIGHT); Key(VK_RIGHT); Key(VK_RIGHT); Key(VK_RIGHT); break;
        case 1689: Key(VK_F1); break;
        case 1692: Snapshot("shot53.bmp"); Ctl(18); break;
        case 1694: Say("11c: back to 30 m; Display: all off, then name HP move AI (was level 2)"); Key(VK_F1);
            Key(VK_LEFT); Key(VK_LEFT); Key(VK_LEFT); Key(VK_LEFT); Key(VK_LEFT); Key(VK_DOWN); Key(VK_DOWN); Key(VK_RETURN);
            Key(VK_DOWN); Key(VK_RETURN); Key(VK_DOWN); Key(VK_RETURN); Key(VK_DOWN); Key(VK_DOWN); Key(VK_RETURN);
            Key(VK_DOWN); Key(VK_DOWN); Key(VK_DOWN); Key(VK_RETURN); Key(VK_DOWN); Key(VK_RETURN); Key(VK_ESCAPE); break;
        case 1695: Key(VK_F1); break;
        case 1696: Say("11d: F5 -> info: target on (the monster in the centre)"); Key(VK_F5); break;
        case 1700: Snapshot("shot54.bmp"); Ctl(18); break;
        case 1702: Say("11e: show: monsters + NPC, all characters"); Key(VK_F1); Key(VK_UP); Key(VK_RIGHT); Key(VK_RIGHT); break;
        case 1703: Key(VK_F1); break;
        case 1705: Snapshot("shot55.bmp"); Ctl(18); break;
        case 1707: Key(VK_F1); Key(VK_LEFT); Key(VK_LEFT); break;
        case 1708: Key(VK_F1); break;
        case 1710: Say("11f: damage info on + Display > hits, two hits of 25 on Skel_A01"); Key(VK_F1); Key(VK_DOWN);
            Key(VK_DOWN); Key(VK_RETURN); Key(VK_UP); Key(VK_RETURN); Key(VK_DOWN); Key(VK_DOWN); Key(VK_DOWN); Key(VK_DOWN);
            Key(VK_DOWN); Key(VK_RETURN); Key(VK_ESCAPE); break;
        case 1711: Key(VK_F1); break;
        case 1713: Ctl(19); break;
        case 1716: Ctl(19); break;
        case 1718: Snapshot("shot56.bmp"); Ctl(18); break;
        case 1719: Say("11f: the game saves its options: the profile keeps 0, the live value stays 1"); Ctl(24); break;
        case 1720: Say("11g: freeze monsters on"); Key(VK_F1); Key(VK_DOWN); Key(VK_DOWN); Key(VK_RETURN); break;
        case 1721: Key(VK_F1); break;
        case 1723: Snapshot("shot57.bmp"); Ctl(18); break;
        case 1724: Say("11h: freeze + flight with time frozen, then exit: the flag stays 0"); Key(VK_F2); break;
        case 1726: Key('T'); break;
        case 1728: Ctl(18); Key(VK_F2); break;
        case 1730: Say("11h: level load -> freeze off by itself"); Ctl(21); break;
        case 1732: Ctl(18); break;
        case 1734: Say("11h: freeze on again, the player dies -> off by itself"); Key(VK_F1); Key(VK_RETURN); break;
        case 1735: Key(VK_F1); break;
        case 1737: Ctl(20); break;  // after the next frame applied the freeze
        case 1738: Ctl(18); Ctl(26); break;
        case 1740: Say("11i: the target is gone from the list -> another target"); Ctl(25); break;
        case 1742: Ctl(18); Ctl(17); break;
        case 1744: Say("11j: exception while collecting -> info off"); Ctl(22); break;
        case 1746: Ctl(18); Snapshot("shot58.bmp"); break;
        case 1748: Say("11j: nearby + target on again"); Key(VK_F1); Key(VK_UP); Key(VK_UP); Key(VK_UP); Key(VK_UP); Key(VK_UP); Key(VK_UP); Key(VK_RETURN); break;
        case 1749: Key(VK_F1); Key(VK_F5); break;
        case 1750: Say("11k: 300 fake monsters (timing)"); Ctl(23); break;
        case 1754: Snapshot("shot59.bmp"); Ctl(18); break;
        case 1756: Ctl(17); break;
        case 1758: Say("11l: freeze on (damage info is still on), F10 off: everything off, the flags back"); Key(VK_F1); Key(VK_DOWN); Key(VK_DOWN); Key(VK_DOWN); Key(VK_DOWN); Key(VK_DOWN); Key(VK_DOWN); Key(VK_RETURN); break;
        case 1759: Key(VK_F1); break;
        case 1760: Ctl(18); break;
        case 1762: SysKey(VK_F10); break;
        case 1764: Ctl(18); Snapshot("shot60.bmp"); break;
        case 1765: Say("11l: debug off: F5 reaches the game"); Key(VK_F5); break;
        case 1766: Say("11l: F10 on: the Monsters items stay off"); SysKey(VK_F10); break;
        case 1768: Ctl(18); break;
        case 1770: Say("11m: Keys > info: target = F7 (F5 back to the game)"); Key(VK_F1); Key(VK_ESCAPE); Key(VK_DOWN); Key(VK_DOWN); Key(VK_DOWN); Key(VK_RETURN); Key(VK_UP); Key(VK_RETURN); break;
        case 1772: Key(VK_F7); break;
        case 1774: Snapshot("shot61.bmp"); PrintFile(); break;
        case 1776: Key(VK_F1); break;
        case 1777: Key(VK_F7); Key(VK_F5); break;
        case 1779: Snapshot("shot62.bmp"); break;
        case 1780: Say("11m: reset to defaults"); Key(VK_F7); Key(VK_F1); Key(VK_DOWN); Key(VK_RETURN); break;
        case 1781: Key(VK_F1); break;
        case 1784: Say("11n: a keys file of v0.9 (no infoTarget line): the line is added with F5"); WriteFile("debug = F10\nmenu = F1\nplayerFly = F2\nshowTriggers = F3\nattach = R\ntime = T\n"); Ctl(16); PrintFile(); break;
        case 1788: Say("11n: an old file where menu = F5: infoTarget gets another free default"); WriteFile("debug = F10\nmenu = F5\nplayerFly = F2\nshowTriggers = F3\nattach = R\ntime = T\n"); Ctl(16); PrintFile(); break;
        case 1790: Say("11n: back to the defaults (F5 opens the menu now)"); Key(VK_F5); Key(VK_RETURN); break;
        case 1791: Key(VK_F1); break;
        case 1792: Ctl(16); PrintFile(); break;
        case 1794: Say("11: F10 off"); SysKey(VK_F10); Ctl(18); break;
        // ---- task 12: sub-folder Monsters > Display (fake monsters of task 11) ----
        case 1800: Say("12a: F10 on; Main > Monsters > Display: the path in the header"); SysKey(VK_F10); Ctl(17); break;
        case 1802: Key(VK_F1); Key(VK_ESCAPE); Key(VK_UP); Key(VK_UP); Key(VK_UP); Key(VK_RETURN);  // Keys -> Monsters
            for (int i = 0; i < 6; i++) Key(VK_UP);  // freeze -> info: nearby
            Key(VK_RETURN); Key(VK_DOWN); Key(VK_DOWN); Key(VK_DOWN); Key(VK_DOWN); Key(VK_RETURN); break;
        case 1804: Snapshot("shot63.bmp"); break;
        case 1805: Say("12b: Esc - back to Monsters, the cursor on Display"); Key(VK_ESCAPE); break;
        case 1807: Snapshot("shot64.bmp"); break;
        case 1808: Say("12c: Enter - Display at the remembered row (hits); all off -> no labels"); Key(VK_RETURN); Key(VK_DOWN); Key(VK_DOWN); Key(VK_RETURN); break;
        case 1810: Snapshot("shot65.bmp"); Ctl(18); break;
        case 1811: Say("12d: F1 closes inside the folder, F1 opens it there again"); Key(VK_F1); break;
        case 1812: Key(VK_F1); break;
        case 1814: Snapshot("shot66.bmp"); break;
        case 1815: Say("12e: name on: labels with the name only; F5: the panel"); Key(VK_DOWN); Key(VK_RETURN); break;
        case 1816: Key(VK_F1); break;
        case 1817: Key(VK_F5); break;
        case 1819: Snapshot("shot67.bmp"); Ctl(18); break;
        case 1820: Say("12e: + class (panel only), HP, level"); Key(VK_F1); Key(VK_DOWN); Key(VK_RETURN); Key(VK_DOWN); Key(VK_RETURN); Key(VK_DOWN); Key(VK_RETURN); break;
        case 1821: Key(VK_F1); break;
        case 1823: Snapshot("shot68.bmp"); break;
        case 1824: Say("12e: + stats, move, AI, cooldowns"); Key(VK_F1);
            for (int i = 0; i < 4; i++) Key(VK_DOWN), Key(VK_RETURN);
            break;
        case 1825: Key(VK_F1); break;
        case 1827: Snapshot("shot69.bmp"); break;
        case 1828: Say("12e: + target, distance (panel only), position, hits (damage info off: no hit line)"); Key(VK_F1);
            for (int i = 0; i < 4; i++) Key(VK_DOWN), Key(VK_RETURN);
            break;
        case 1829: Key(VK_F1); break;
        case 1831: Snapshot("shot70.bmp"); Ctl(18); break;
        case 1832: Say("12f: Left on a switch = off (not back), Right = on"); Key(VK_F1); Key(VK_LEFT); break;
        case 1834: Key(VK_RIGHT); break;
        case 1835: Key(VK_F1); break;
        case 1836: Say("12g: all off: no labels, the panel says so"); Key(VK_F1); Key(VK_DOWN); Key(VK_DOWN); Key(VK_RETURN); break;
        case 1837: Key(VK_F1); break;
        case 1839: Snapshot("shot71.bmp"); Ctl(18); break;
        case 1840: Say("12h: all on; Left on an action = back; damage info on + a hit: the hit line"); Key(VK_F1); Key(VK_UP);
            Key(VK_RETURN); Key(VK_LEFT); Key(VK_DOWN); Key(VK_RETURN); break;
        case 1841: Key(VK_F1); break;
        case 1843: Ctl(19); break;
        case 1845: Snapshot("shot72.bmp"); Ctl(18); break;
        case 1846: Say("12i: damage info off; the defaults back (all off + name + HP, Right opens the folder); Esc Esc: Main");
            Key(VK_F1); Key(VK_RETURN); Key(VK_UP); Key(VK_RIGHT); Key(VK_DOWN); Key(VK_RETURN); Key(VK_DOWN); Key(VK_RETURN);
            Key(VK_DOWN); Key(VK_DOWN); Key(VK_RETURN); Key(VK_ESCAPE); Key(VK_ESCAPE); break;
        case 1848: Snapshot("shot73.bmp"); Ctl(18); break;
        case 1850: Key(VK_F1); break;
        case 1851: Say("12i: F1 opens at Main (Monsters)"); Key(VK_F1); break;
        case 1853: Key(VK_F1); break;
        case 1856: Say("final: F10 off"); SysKey(VK_F10); Ctl(18); break;
        case 1860: Say("final cursor state: released"); Ctl(6); break;
        }
        g_dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(60, 90, 130), 1.0f, 0);
        g_dev->BeginScene();
        if (frame >= 1150) DrawOccluder();
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

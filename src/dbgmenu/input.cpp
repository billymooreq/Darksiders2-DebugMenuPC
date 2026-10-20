// Input: menu keys come from WM_KEYDOWN of the game window (subclassed WndProc).
// While the menu is open the game gets no key presses and no mouse:
//   - legacy messages (WM_KEYDOWN/WM_CHAR/mouse) are swallowed in our WndProc;
//   - Raw Input (the game reads it in its WndProc via GetRawInputData, 0x00913C78) is filtered in the
//     IAT hook: presses become a no-op key (VKey 0xFF, break), mouse motion/wheel/button-downs are zeroed.
// A key the game saw going down always gets its release, and a press we swallowed never leaks a release,
// so no key stays "stuck" after the menu closes. F1/F2/F3 (our hotkeys) never reach the game.
// Flight (task 04): the same block while flying; the flight reads held keys and mouse motion from what we see
// here (s_held, raw mouse deltas), Enter / left button request the teleport.
// Task 05: focus tracking (WM_ACTIVATEAPP / WM_ACTIVATE / WM_KILLFOCUS / WM_SETFOCUS + foreground check): losing
// the window no longer ends the flight - it freezes it, every held key/button is forgotten and the cursor is
// released at once; the hover after a teleport ends at the first game control key/button pressed (Input_HoverKey).
// Task 09: T (time) and R (attach the player) while flying, from WM_KEYDOWN like Enter; the game never sees them.
// Task 10: the hotkeys come from keys.cpp (the player can change them). The debug key (F10) is always ours; the
// menu / playerFly / Show triggers keys only while the debug menu is on - while it is off the game gets them like
// any other key. While the menu Keys waits for a new key, the next key press (or mouse button) goes to it only.
#include "common.h"

volatile LONG g_menuOpen;
volatile LONG g_inputBlocked;
volatile LONG g_overlayReady;
UINT(WINAPI *o_GetRawInputData)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);

static HWND g_hwnd;
static WNDPROC g_origProc;
static bool g_unicode;
static CRITICAL_SECTION g_qLock;
static int g_queue[64];
static int g_qHead, g_qTail;

struct KeyPairing {
    BYTE gameDown[256];   // game saw the press -> it must see the release
    BYTE swallowed[256];  // we ate the press -> eat the release too
};
static KeyPairing s_legacy, s_raw;
static USHORT s_mouseGameDown, s_mouseSwallowed;  // bit per mouse button 0..4
static BYTE s_held[256];                          // physical state as we saw it (game thread only); mouse
                                                  // buttons under VK_LBUTTON / VK_RBUTTON / VK_MBUTTON / VK_XBUTTONn
static BYTE s_hoverIgnore[256];                   // held when the hover started: never releases it
static volatile LONG s_hoverPressed;              // first game control press since Input_HoverArm
static bool s_active = true;                      // window active (from activation messages)
static DWORD s_activeSince;
static volatile LONG s_mouseDx, s_mouseDy;        // relative motion while flying
static LPARAM s_lastMouseLp;                      // test mode: legacy WM_MOUSEMOVE deltas
static bool s_lastMouseValid;

bool Input_KeyHeld(int vk) { return s_held[vk & 0xFF] != 0; }
HWND Input_Hwnd() { return g_hwnd; }

bool Input_WindowActive()
{
    if (!s_active) return false;
    if (g_testMode || !g_hwnd) return true;  // the test window is never shown / foreground
    if (IsIconic(g_hwnd)) return false;
    HWND fg = GetForegroundWindow();
    return fg == g_hwnd || fg == GetAncestor(g_hwnd, GA_ROOT);
}

DWORD Input_ActiveForMs() { return GetTickCount() - s_activeSince; }

// not hero controls: F-keys, our hotkeys (task 10: may be letters), Esc, Alt / Win / Tab (Alt+Tab), Print Screen,
// media keys
static bool HoverExcluded(int vk)
{
    return vk <= 0 || vk >= 0xFF || (vk >= VK_F1 && vk <= VK_F24) || vk == VK_ESCAPE || vk == VK_MENU ||
           vk == VK_LMENU || vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN || vk == VK_APPS || vk == VK_TAB ||
           vk == VK_SNAPSHOT || vk == VK_PAUSE || (vk >= 0xA6 && vk <= 0xB7) || Keys_HotkeyNow(vk);
}

void Input_HoverArm()
{
    for (int i = 0; i < 256; i++) s_hoverIgnore[i] = s_held[i];
    InterlockedExchange(&s_hoverPressed, 0);
}

int Input_HoverKey()
{
    if (g_inputBlocked) return 0;  // menu open: the keys belong to the menu
    int vk = (int)InterlockedExchange(&s_hoverPressed, 0);
    if (vk) return vk;
    for (int i = 1; i < 256; i++)
        if (s_held[i] && !s_hoverIgnore[i] && !HoverExcluded(i)) return i;
    return 0;
}

// key/button state as we see it (game thread); a new press that reaches the game may end the hover
static void SetHeld(int vk, bool down)
{
    vk &= 0xFF;
    bool was = s_held[vk] != 0;
    s_held[vk] = down ? 1 : 0;
    if (!down) {
        s_hoverIgnore[vk] = 0;
        return;
    }
    if (!was && !g_inputBlocked && !s_hoverIgnore[vk] && !HoverExcluded(vk))
        InterlockedCompareExchange(&s_hoverPressed, vk, 0);
}

static void FocusChanged(bool active, const char *msg)
{
    static volatile LONG budget;
    bool was = s_active;
    memset(s_held, 0, sizeof(s_held));  // releases may go to another window: forget everything held
    Input_ResetFlight();
    s_active = active;
    if (active && !was) s_activeSince = GetTickCount();
    if (was != active && LogBudget(&budget, 100))
        Log("input: window %s (%s)%s", active ? "active" : "INACTIVE", msg,
            g_flying ? (active ? " - flight resumes" : " - flight frozen, keys reset") : "");
    Cursor_Update();  // release / take the cursor now, not on the next frame
}

void Input_TakeMouse(long *dx, long *dy)
{
    *dx = InterlockedExchange(&s_mouseDx, 0);
    *dy = InterlockedExchange(&s_mouseDy, 0);
}

void Input_ResetFlight()
{
    InterlockedExchange(&s_mouseDx, 0);
    InterlockedExchange(&s_mouseDy, 0);
    s_lastMouseValid = false;
}

static void AddMouse(long dx, long dy)
{
    if (!g_flying || g_menuOpen) return;
    InterlockedExchangeAdd(&s_mouseDx, dx);
    InterlockedExchangeAdd(&s_mouseDy, dy);
}

static void PushKey(int vk)
{
    EnterCriticalSection(&g_qLock);
    int next = (g_qTail + 1) % 64;
    if (next != g_qHead) {
        g_queue[g_qTail] = vk;
        g_qTail = next;
    }
    LeaveCriticalSection(&g_qLock);
}

int Input_PopKey()
{
    int vk = 0;
    EnterCriticalSection(&g_qLock);
    if (g_qHead != g_qTail) {
        vk = g_queue[g_qHead];
        g_qHead = (g_qHead + 1) % 64;
    }
    LeaveCriticalSection(&g_qLock);
    return vk;
}

void Input_SetMenuOpen(bool open)
{
    InterlockedExchange(&g_menuOpen, open);
    Input_UpdateBlock();
}

void Input_UpdateBlock()
{
    InterlockedExchange(&g_inputBlocked, (g_menuOpen || g_flying) ? 1 : 0);
}

static bool Capturing() { return g_keyCapture && g_menuOpen; }

// a hotkey press (not a repeat): run it here, the menu thread logs it
static void RunHotkey(int act)
{
    switch (act) {
    case KA_DEBUG: Debug_Toggle(Keys_KeyName(KA_DEBUG)); break;
    case KA_MENU:
        if (g_overlayReady || g_menuOpen) Input_SetMenuOpen(!g_menuOpen);
        break;
    case KA_FLY: Flight_RequestToggle(); break;  // executed by the next game frame
    case KA_TRIGGERS:
        if (g_overlayReady) Trig_Toggle();  // task 07: Show triggers
        break;
    case KA_TARGET:
        if (g_overlayReady) Mon_ToggleTarget();  // task 11: Monsters > info: target
        break;
    case KA_TRIGTARGET:
        if (g_overlayReady) Trig_ToggleTarget();  // task 13: Triggers > info: target
        break;
    }
    PushKey(KQ_ACTION | act);
}

// menu Keys waits for a key: a mouse button is offered too (to say "not allowed")
static void CaptureButton(int vk)
{
    if (Capturing()) PushKey(KQ_CAPTURE | vk);
}

static bool IsMenuKey(int vk)
{
    return vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT || vk == VK_RETURN || vk == VK_ESCAPE ||
           vk == VK_BACK;
}

// returns true = hide this event from the game
static bool KeyMake(KeyPairing &k, int vk, bool block)
{
    if (!block) {
        k.gameDown[vk] = 1;
        k.swallowed[vk] = 0;
        return false;
    }
    if (!k.gameDown[vk]) k.swallowed[vk] = 1;  // auto-repeat of a key the game holds: hide, keep pairing
    return true;
}

static bool KeyBreak(KeyPairing &k, int vk)
{
    if (k.gameDown[vk]) {
        k.gameDown[vk] = 0;
        return false;
    }
    if (k.swallowed[vk]) {
        k.swallowed[vk] = 0;
        return true;
    }
    return false;
}

// legacy window messages; true = swallow
static bool FilterMessage(UINT m, WPARAM w, LPARAM l)
{
    static volatile LONG firstKey;
    bool blocked = g_inputBlocked != 0;
    switch (m) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        int vk = (int)(w & 0xFF);
        bool repeat = (l & 0x40000000) != 0;
        if (LogBudget(&firstKey, 1)) Log("input: first WM_KEYDOWN vk=%02X (legacy keyboard messages arrive)", vk);
        SetHeld(vk, true);
        if (Capturing() && !(m == WM_SYSKEYDOWN && vk == VK_F4)) {  // task 10: menu Keys; Alt+F4 still works
            if (!repeat) PushKey(KQ_CAPTURE | vk);
            KeyMake(s_legacy, vk, true);
            return true;
        }
        int act = Keys_HotkeyNow(vk);
        if (act) {
            if (!repeat) RunHotkey(act);
            KeyMake(s_legacy, vk, true);
            return true;
        }
        bool flyKeys = g_flying && g_debugOn && !g_menuOpen && !repeat;
        if (g_menuOpen && IsMenuKey(vk) && (!repeat || vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT))
            PushKey(vk);
        else if (flyKeys && m == WM_KEYDOWN && vk == VK_RETURN)
            Flight_RequestTeleport();
        else if (flyKeys && vk == Keys_Get(KA_TIME))
            Flight_RequestTime(-1);    // task 09: time frozen / running
        else if (flyKeys && vk == Keys_Get(KA_ATTACH))
            Flight_RequestAttach(-1);  // task 09: carry the player / let him go
        // Alt+F4 must keep working
        return KeyMake(s_legacy, vk, blocked && !(m == WM_SYSKEYDOWN && vk == VK_F4));
    }
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if ((w & 0xFF) == VK_SNAPSHOT) CaptureButton(VK_SNAPSHOT);  // Print Screen has no WM_KEYDOWN
        SetHeld((int)(w & 0xFF), false);
        return KeyBreak(s_legacy, (int)(w & 0xFF));
    case WM_ACTIVATEAPP:
        FocusChanged(w != 0, w ? "WM_ACTIVATEAPP 1" : "WM_ACTIVATEAPP 0");
        return false;
    case WM_DESTROY:
        Cheats_OnUnload();  // task 15: the game's PlayerInvincible byte back before the game shuts down
        return false;
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE) FocusChanged(false, "WM_ACTIVATE inactive");
        else if (!HIWORD(w)) FocusChanged(true, "WM_ACTIVATE");
        return false;
    case WM_KILLFOCUS:
        FocusChanged(false, "WM_KILLFOCUS");
        return false;
    case WM_SETFOCUS:
        FocusChanged(true, "WM_SETFOCUS");
        return false;
    case WM_CHAR:
    case WM_SYSCHAR:
    case WM_DEADCHAR:
    case WM_SYSDEADCHAR:
    case WM_UNICHAR:
        return blocked;
    case WM_LBUTTONDOWN:
        SetHeld(VK_LBUTTON, true);
        CaptureButton(VK_LBUTTON);
        if (g_flying && g_debugOn && !g_menuOpen) Flight_RequestTeleport();
        return blocked;
    case WM_RBUTTONDOWN: SetHeld(VK_RBUTTON, true); CaptureButton(VK_RBUTTON); return blocked;
    case WM_MBUTTONDOWN: SetHeld(VK_MBUTTON, true); CaptureButton(VK_MBUTTON); return blocked;
    case WM_XBUTTONDOWN: {
        int vk = HIWORD(w) == XBUTTON2 ? VK_XBUTTON2 : VK_XBUTTON1;
        SetHeld(vk, true);
        CaptureButton(vk);
        return blocked;
    }
    case WM_LBUTTONUP: SetHeld(VK_LBUTTON, false); return false;
    case WM_RBUTTONUP: SetHeld(VK_RBUTTON, false); return false;
    case WM_MBUTTONUP: SetHeld(VK_MBUTTON, false); return false;
    case WM_XBUTTONUP: SetHeld(HIWORD(w) == XBUTTON2 ? VK_XBUTTON2 : VK_XBUTTON1, false); return false;
    case WM_MOUSEMOVE:
        if (g_testMode) {  // offline test has no raw mouse: use the cursor motion
            if (s_lastMouseValid)
                AddMouse((short)LOWORD(l) - (short)LOWORD(s_lastMouseLp), (short)HIWORD(l) - (short)HIWORD(s_lastMouseLp));
            s_lastMouseLp = l;
            s_lastMouseValid = true;
        }
        return blocked;
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDBLCLK:
    case WM_MOUSEWHEEL:
    case 0x020E:  // WM_MOUSEHWHEEL
        return blocked;
    }
    return false;
}

static LRESULT CALLBACK HookWndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (FilterMessage(m, w, l)) return 0;
    return g_unicode ? CallWindowProcW(g_origProc, h, m, w, l) : CallWindowProcA(g_origProc, h, m, w, l);
}

// ---- Raw Input (IAT hook of the exe's GetRawInputData) ----
static void FilterRaw(RAWINPUT *ri, UINT size)
{
    static volatile LONG firstKb, firstMouse;
    bool blocked = g_inputBlocked != 0;
    if (ri->header.dwType == RIM_TYPEKEYBOARD && size >= sizeof(RAWINPUTHEADER) + sizeof(RAWKEYBOARD)) {
        RAWKEYBOARD &kb = ri->data.keyboard;
        int vk = kb.VKey;
        if (LogBudget(&firstKb, 1)) Log("input: first raw keyboard event vk=%02X flags=%X", vk, kb.Flags);
        if (vk <= 0 || vk >= 0xFF) return;
        SetHeld(vk, !(kb.Flags & RI_KEY_BREAK));
        bool hide = (kb.Flags & RI_KEY_BREAK) ? KeyBreak(s_raw, vk)
                                               : KeyMake(s_raw, vk, blocked || Keys_HotkeyNow(vk) || Capturing());
        if (hide) {  // a release of an unused virtual key: the game ignores it
            kb.VKey = 0xFF;
            kb.MakeCode = 0;
            kb.Flags = RI_KEY_BREAK;
            kb.Message = WM_KEYUP;
        }
    } else if (ri->header.dwType == RIM_TYPEMOUSE && size >= sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE)) {
        RAWMOUSE &ms = ri->data.mouse;
        USHORT f = ms.usButtonFlags, out = 0;
        if (LogBudget(&firstMouse, 1)) Log("input: first raw mouse event flags=%X", f);
        if (!(ms.usFlags & MOUSE_MOVE_ABSOLUTE)) AddMouse(ms.lLastX, ms.lLastY);
        if ((f & RI_MOUSE_LEFT_BUTTON_DOWN) && g_flying && g_debugOn && !g_menuOpen) Flight_RequestTeleport();
        static const int kButtonVk[5] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
        for (int b = 0; b < 5; b++) {
            USHORT down = (USHORT)(1 << (2 * b)), up = (USHORT)(1 << (2 * b + 1)), bit = (USHORT)(1 << b);
            if (f & down) SetHeld(kButtonVk[b], true);
            if (f & up) SetHeld(kButtonVk[b], false);
            if (f & down) {
                if (!blocked) {
                    s_mouseGameDown |= bit;
                    s_mouseSwallowed &= ~bit;
                    out |= down;
                } else if (!(s_mouseGameDown & bit)) {
                    s_mouseSwallowed |= bit;
                }
            }
            if (f & up) {
                if (s_mouseGameDown & bit) {
                    s_mouseGameDown &= ~bit;
                    out |= up;
                } else if (s_mouseSwallowed & bit) {
                    s_mouseSwallowed &= ~bit;
                } else {
                    out |= up;
                }
            }
        }
        if (blocked) {
            ms.usFlags = MOUSE_MOVE_RELATIVE;
            ms.lLastX = ms.lLastY = 0;
            ms.usButtonData = 0;
        } else {
            out |= f & (RI_MOUSE_WHEEL | 0x0800 /*RI_MOUSE_HWHEEL*/);
        }
        ms.usButtonFlags = out;
    }
}

UINT WINAPI Hook_GetRawInputData(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT hdr)
{
    UINT r = o_GetRawInputData(h, cmd, data, size, hdr);
    if (cmd == RID_INPUT && data && r != (UINT)-1 && r >= sizeof(RAWINPUTHEADER)) {
        __try {
            FilterRaw((RAWINPUT *)data, r);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            static volatile LONG budget;
            if (LogBudget(&budget, 5)) Log("ERROR: exception %08X in raw input filter", GetExceptionCode());
        }
    }
    return r;
}

// ---- window ----
void Input_Init()
{
    InitializeCriticalSection(&g_qLock);
}

void Input_Attach(HWND hwnd)
{
    if (!hwnd || hwnd == g_hwnd) return;
    g_unicode = IsWindowUnicode(hwnd) != 0;
    WNDPROC prev = (WNDPROC)(g_unicode ? SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)HookWndProc)
                                       : SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)HookWndProc));
    if (!prev) {
        Log("ERROR: WndProc subclass of %08X failed (err %lu) -> no menu keys", (unsigned)(UINT_PTR)hwnd, GetLastError());
        return;
    }
    g_origProc = prev;
    g_hwnd = hwnd;
    s_activeSince = GetTickCount();
    Log("WndProc subclassed: hwnd=%08X unicode=%d orig=%08X thread=%lu", (unsigned)(UINT_PTR)hwnd, g_unicode,
        (unsigned)(UINT_PTR)prev, GetWindowThreadProcessId(hwnd, NULL));
}

// Someone may subclass the window after us; we stay in the chain only if they call the previous proc.
void Input_PerFrame()
{
    static volatile LONG budget;
    if (!g_hwnd) return;
    LONG_PTR cur = g_unicode ? GetWindowLongPtrW(g_hwnd, GWLP_WNDPROC) : GetWindowLongPtrA(g_hwnd, GWLP_WNDPROC);
    if (cur && cur != (LONG_PTR)HookWndProc && LogBudget(&budget, 1))
        Log("WARNING: WndProc of %08X replaced after us by %08X (menu keys work only if it chains to us)",
            (unsigned)(UINT_PTR)g_hwnd, (unsigned)cur);
}

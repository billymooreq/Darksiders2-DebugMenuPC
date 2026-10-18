// Input: menu keys come from WM_KEYDOWN of the game window (subclassed WndProc).
// While the menu is open the game gets no key presses and no mouse:
//   - legacy messages (WM_KEYDOWN/WM_CHAR/mouse) are swallowed in our WndProc;
//   - Raw Input (the game reads it in its WndProc via GetRawInputData, 0x00913C78) is filtered in the
//     IAT hook: presses become a no-op key (VKey 0xFF, break), mouse motion/wheel/button-downs are zeroed.
// A key the game saw going down always gets its release, and a press we swallowed never leaks a release,
// so no key stays "stuck" after the menu closes. F1/F2 (our hotkeys) never reach the game.
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
    InterlockedExchange(&g_inputBlocked, open);  // task 04: also while flying
}

static bool IsHotkey(int vk) { return vk == VK_F1 || vk == VK_F2; }
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
        if (IsHotkey(vk)) {
            if (!repeat) {
                if (vk == VK_F1 && (g_overlayReady || g_menuOpen)) Input_SetMenuOpen(!g_menuOpen);
                PushKey(vk);
            }
            KeyMake(s_legacy, vk, true);
            return true;
        }
        if (g_menuOpen && IsMenuKey(vk) && (!repeat || vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT))
            PushKey(vk);
        // Alt+F4 must keep working
        return KeyMake(s_legacy, vk, blocked && !(m == WM_SYSKEYDOWN && vk == VK_F4));
    }
    case WM_KEYUP:
    case WM_SYSKEYUP:
        return KeyBreak(s_legacy, (int)(w & 0xFF));
    case WM_CHAR:
    case WM_SYSCHAR:
    case WM_DEADCHAR:
    case WM_SYSDEADCHAR:
    case WM_UNICHAR:
        return blocked;
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK:
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
        bool hide = (kb.Flags & RI_KEY_BREAK) ? KeyBreak(s_raw, vk) : KeyMake(s_raw, vk, blocked || IsHotkey(vk));
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
        for (int b = 0; b < 5; b++) {
            USHORT down = (USHORT)(1 << (2 * b)), up = (USHORT)(1 << (2 * b + 1)), bit = (USHORT)(1 << b);
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

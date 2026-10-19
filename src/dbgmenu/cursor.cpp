// System cursor while the menu is open or playerFly runs (task 05).
// The game itself never clips the cursor (its only ClipCursor call, 0x00403976, releases it on WM_ACTIVATEAPP(0));
// it keeps it in the window by SetCursorPos to the centre (0x0091BEA9) and hides it with ShowCursor loops
// (0x00D4D07A show / 0x00D4D087 hide). While paused / blocked that logic stops, so in v0.3 the cursor was visible
// and could reach a second monitor, where the teleport click minimized the game.
// While captured (menu open or flying, and the window has the focus):
//   - the cursor is hidden (ShowCursor display counter kept below 0) and clipped to a 4x4 px box in the centre
//     of the client area; the free camera reads Raw Input deltas only, so the cursor position does not matter;
//   - the exe's own ShowCursor / ClipCursor / SetCursorPos calls (IAT hooks) are recorded instead of applied.
// On release (menu closed and flight over, or focus lost) the state before us comes back: the display counter
// becomes what the game's own calls made it, the clip is the game's last request (or the one we found), the
// cursor goes back to where it was (only if we still have the focus).
// Test mode (DBGMENU_TEST=1): the system functions are simulated, the desktop cursor is never touched.
#include "common.h"

typedef int(WINAPI *ShowCursor_t)(BOOL);
typedef BOOL(WINAPI *ClipCursor_t)(const RECT *);
typedef BOOL(WINAPI *SetCursorPos_t)(int, int);
static ShowCursor_t o_ShowCursor;
static ClipCursor_t o_ClipCursor;
static SetCursorPos_t o_SetCursorPos;

// ---- simulated system state (test mode) ----
static struct {
    int count;  // display counter
    bool clipped;
    RECT clip;
    POINT pos;
} s_sim = {0, false, {0, 0, 0, 0}, {50, 60}};

static int SysShowCursor(BOOL show)
{
    if (g_testMode) return s_sim.count += show ? 1 : -1;
    return o_ShowCursor ? o_ShowCursor(show) : ShowCursor(show);
}
static BOOL SysClipCursor(const RECT *r)
{
    if (g_testMode) {
        s_sim.clipped = r != NULL;
        if (r) s_sim.clip = *r;
        return TRUE;
    }
    return o_ClipCursor ? o_ClipCursor(r) : ClipCursor(r);
}
static BOOL SysSetCursorPos(int x, int y)
{
    if (g_testMode) {
        s_sim.pos.x = x;
        s_sim.pos.y = y;
        return TRUE;
    }
    return o_SetCursorPos ? o_SetCursorPos(x, y) : SetCursorPos(x, y);
}
static bool SysGetClip(RECT *r, bool *full)  // full = no clip (the whole virtual screen)
{
    if (g_testMode) {
        *full = !s_sim.clipped;
        if (s_sim.clipped) *r = s_sim.clip;
        return true;
    }
    if (!GetClipCursor(r)) return false;
    RECT v = {GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
    v.right = v.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    v.bottom = v.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    *full = r->left <= v.left && r->top <= v.top && r->right >= v.right && r->bottom >= v.bottom;
    return true;
}
static bool SysGetPos(POINT *p)
{
    if (g_testMode) {
        *p = s_sim.pos;
        return true;
    }
    return GetCursorPos(p) != 0;
}

// ---- capture state (game thread) ----
static bool s_captured;
static DWORD s_thread;
static int s_virtCount;           // the display counter as the game believes it is
static int s_ourHides;            // ShowCursor(FALSE) calls we made
static RECT s_savedClip;
static bool s_savedClipFull, s_savedClipOk, s_savedPosOk;
static POINT s_savedPos;
static bool s_gameClipSet, s_gameClipNull, s_gamePosSet;  // the game asked for something while captured
static RECT s_gameClip;
static POINT s_gamePos;
static LONG s_gameShowCalls, s_gameClipCalls, s_gamePosCalls;
static RECT s_box;

static bool MineHere() { return s_captured && GetCurrentThreadId() == s_thread; }

static int WINAPI Hook_ShowCursor(BOOL show)
{
    if (!MineHere()) return SysShowCursor(show);
    s_gameShowCalls++;
    return s_virtCount += show ? 1 : -1;
}

static BOOL WINAPI Hook_ClipCursor(const RECT *r)
{
    if (!MineHere()) return SysClipCursor(r);
    s_gameClipCalls++;
    s_gameClipSet = true;
    s_gameClipNull = r == NULL;
    if (r) s_gameClip = *r;
    return TRUE;
}

static BOOL WINAPI Hook_SetCursorPos(int x, int y)
{
    if (!MineHere()) return SysSetCursorPos(x, y);
    s_gamePosCalls++;
    s_gamePosSet = true;
    s_gamePos.x = x;
    s_gamePos.y = y;
    return TRUE;
}

void Cursor_InstallHooks(HMODULE exe)
{
    static const struct {
        const char *name;
        void *hook;
        void **orig;
    } k[] = {{"ShowCursor", (void *)Hook_ShowCursor, (void **)&o_ShowCursor},
             {"ClipCursor", (void *)Hook_ClipCursor, (void **)&o_ClipCursor},
             {"SetCursorPos", (void *)Hook_SetCursorPos, (void **)&o_SetCursorPos}};
    for (int i = 0; i < 3; i++) {
        void **slot = FindImportSlot(exe, "user32.dll", k[i].name);
        bool ok = slot && PatchSlot(slot, k[i].hook, k[i].orig);
        Log("IAT %-12s slot=%08X : %s", k[i].name, (unsigned)(UINT_PTR)slot,
            ok ? "hooked" : "NOT hooked (the game's own calls are not recorded while captured)");
    }
}

static bool CentreBox(RECT *box)
{
    HWND h = Input_Hwnd();
    RECT rc;
    if (!h || !GetClientRect(h, &rc)) return false;
    POINT c = {(rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2};
    if (!ClientToScreen(h, &c)) return false;
    box->left = c.x - 2;
    box->top = c.y - 2;
    box->right = c.x + 2;
    box->bottom = c.y + 2;
    return true;
}

// every captured frame: still hidden, still clipped (Windows or another program may have changed it)
static void Enforce()
{
    int c = SysShowCursor(FALSE);
    if (c < -1) SysShowCursor(TRUE);
    else s_ourHides++;  // someone showed it: keep our extra hide (undone on release)
    RECT box, cur;
    bool full;
    if (!CentreBox(&box)) return;
    if (!SysGetClip(&cur, &full) || full || memcmp(&cur, &box, sizeof(box)) != 0) {
        SysClipCursor(&box);
        s_box = box;
    }
}

static void Capture()
{
    static volatile LONG budget;
    s_savedClipOk = SysGetClip(&s_savedClip, &s_savedClipFull);
    s_savedPosOk = SysGetPos(&s_savedPos);
    int c = SysShowCursor(FALSE), n = 1;
    while (c >= 0 && n < 64) {
        c = SysShowCursor(FALSE);
        n++;
    }
    s_ourHides = n;
    s_virtCount = c + n;
    s_gameClipSet = s_gamePosSet = false;
    s_gameShowCalls = s_gameClipCalls = s_gamePosCalls = 0;
    s_thread = GetCurrentThreadId();
    s_captured = true;
    Enforce();
    if (LogBudget(&budget, 200))
        Log("cursor: captured (menu=%ld fly=%ld) | display counter %d -> %d | clip before %s -> box (%ld %ld %ld %ld) | "
            "pos (%ld %ld)",
            g_menuOpen, g_flying, s_virtCount, c, s_savedClipOk && !s_savedClipFull ? "a rect" : "none", s_box.left,
            s_box.top, s_box.right, s_box.bottom, s_savedPos.x, s_savedPos.y);
}

static void Release(const char *why, bool focused)
{
    static volatile LONG budget;
    s_captured = false;
    int c = SysShowCursor(TRUE), guard = 0;
    while (c < s_virtCount && guard++ < 256) c = SysShowCursor(TRUE);
    while (c > s_virtCount && guard++ < 256) c = SysShowCursor(FALSE);
    if (s_gameClipSet) SysClipCursor(s_gameClipNull ? NULL : &s_gameClip);
    else if (s_savedClipOk && !s_savedClipFull) SysClipCursor(&s_savedClip);
    else SysClipCursor(NULL);
    POINT p = s_gamePosSet ? s_gamePos : s_savedPos;
    bool moved = focused && (s_gamePosSet || s_savedPosOk);
    if (moved) SysSetCursorPos(p.x, p.y);
    if (LogBudget(&budget, 200))
        Log("cursor: released (%s) | display counter %d (game calls: ShowCursor %ld, ClipCursor %ld, SetCursorPos %ld) | "
            "clip %s | pos %s(%ld %ld)",
            why, c, s_gameShowCalls, s_gameClipCalls, s_gamePosCalls,
            s_gameClipSet ? (s_gameClipNull ? "game's: none" : "game's rect")
                          : (s_savedClipOk && !s_savedClipFull ? "restored rect" : "none"),
            moved ? "" : "not moved ", p.x, p.y);
}

void Cursor_Update()
{
    __try {
        bool active = Input_WindowActive();
        bool want = (g_menuOpen || g_flying) && active && Input_Hwnd() != NULL;
        if (want && !s_captured) Capture();
        else if (!want && s_captured) Release(active ? "menu closed / flight over" : "focus lost", active);
        else if (s_captured) Enforce();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static volatile LONG budget;
        if (LogBudget(&budget, 5)) Log("cursor: ERROR exception %08X", GetExceptionCode());
    }
}

void Cursor_OnUnload()
{
    if (!s_captured) return;
    s_captured = false;
    SysClipCursor(NULL);  // never leave the desktop clipped; the counter belongs to a thread that is going away
    Log("cursor: released (unload)");
}

void Cursor_TestLog(const char *when)
{
    if (!g_testMode) return;
    Log("test: cursor %s: captured=%d counter=%d (%s) clip=%s(%ld %ld %ld %ld) pos=(%ld %ld)", when, s_captured,
        s_sim.count, s_sim.count < 0 ? "hidden" : "visible", s_sim.clipped ? "" : "none ", s_sim.clip.left,
        s_sim.clip.top, s_sim.clip.right, s_sim.clip.bottom, s_sim.pos.x, s_sim.pos.y);
}

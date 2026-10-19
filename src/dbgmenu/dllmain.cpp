// dinput8.dll proxy: forwards DirectInput to the system DLL, writes the log and installs the hooks.
#include "common.h"
#include <stdio.h>
#include <stdarg.h>
#include <intrin.h>

#define EXPECTED_TIMESTAMP 0x50241AFAu  // Darksiders2.exe PE TimeDateStamp (2012-08-09)
#define EXPECTED_IMAGESIZE 0x0147D000u

HMODULE g_self;
bool g_gameMatch;
bool g_testMode;
static HMODULE g_real;
static CRITICAL_SECTION g_logLock;
static char g_logPath[MAX_PATH];
static UINT_PTR g_imgBegin, g_imgEnd, g_textBegin, g_textEnd;

typedef HRESULT(WINAPI *DirectInput8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID *, void *);
typedef HRESULT(WINAPI *DllCanUnloadNow_t)(void);
typedef HRESULT(WINAPI *DllGetClassObject_t)(REFCLSID, REFIID, LPVOID *);
typedef HRESULT(WINAPI *DllRegisterServer_t)(void);
typedef LPCVOID(WINAPI *GetdfDIJoystick_t)(void);

static DirectInput8Create_t p_DirectInput8Create;
static DllCanUnloadNow_t p_DllCanUnloadNow;
static DllGetClassObject_t p_DllGetClassObject;
static DllRegisterServer_t p_DllRegisterServer;
static DllRegisterServer_t p_DllUnregisterServer;
static GetdfDIJoystick_t p_GetdfDIJoystick;

void Log(const char *fmt, ...)
{
    char line[1024];
    SYSTEMTIME t;
    va_list ap;
    int n;
    FILE *f;

    GetLocalTime(&t);
    n = _snprintf_s(line, sizeof(line), _TRUNCATE, "%02u:%02u:%02u.%03u [%5lu] ", t.wHour, t.wMinute,
                    t.wSecond, t.wMilliseconds, GetCurrentThreadId());
    va_start(ap, fmt);
    _vsnprintf_s(line + n, sizeof(line) - n, _TRUNCATE, fmt, ap);
    va_end(ap);

    EnterCriticalSection(&g_logLock);
    if (fopen_s(&f, g_logPath, "a") == 0 && f) {
        fputs(line, f);
        fputc('\n', f);
        fclose(f);  // close every line: nothing lost if the game crashes
    }
    LeaveCriticalSection(&g_logLock);
}

bool LogBudget(volatile LONG *counter, LONG max)
{
    return InterlockedIncrement(counter) <= max;
}

bool InImage(const void *p) { return (UINT_PTR)p >= g_imgBegin && (UINT_PTR)p < g_imgEnd; }
bool InText(const void *p) { return (UINT_PTR)p >= g_textBegin && (UINT_PTR)p < g_textEnd; }

// Log goes to <game dir>\..\_claudework\logs\dbgmenu.log (previous run -> dbgmenu.prev.log); fallback: next to the DLL.
static void init_log_path(void)
{
    char dir[MAX_PATH], prev[MAX_PATH], *p;
    GetModuleFileNameA(g_self, dir, MAX_PATH);
    p = strrchr(dir, '\\');
    if (p) *p = 0;
    _snprintf_s(g_logPath, MAX_PATH, _TRUNCATE, "%s\\..\\_claudework\\logs", dir);
    if (GetFileAttributesA(g_logPath) == INVALID_FILE_ATTRIBUTES)
        _snprintf_s(g_logPath, MAX_PATH, _TRUNCATE, "%s", dir);
    _snprintf_s(prev, MAX_PATH, _TRUNCATE, "%s\\dbgmenu.prev.log", g_logPath);
    strcat_s(g_logPath, MAX_PATH, "\\dbgmenu.log");
    MoveFileExA(g_logPath, prev, MOVEFILE_REPLACE_EXISTING);
}

static void check_exe(void)
{
    char path[MAX_PATH];
    HMODULE exe = GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)exe;
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)((BYTE *)exe + dos->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    DWORD ts = nt->FileHeader.TimeDateStamp, sz = nt->OptionalHeader.SizeOfImage;

    g_imgBegin = (UINT_PTR)exe;
    g_imgEnd = g_imgBegin + sz;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) {
            g_textBegin = g_imgBegin + sec[i].VirtualAddress;
            g_textEnd = g_textBegin + sec[i].Misc.VirtualSize;
            break;
        }
    GetModuleFileNameA(NULL, path, MAX_PATH);
    g_gameMatch = (UINT_PTR)exe == 0x00400000 && ts == EXPECTED_TIMESTAMP && sz == EXPECTED_IMAGESIZE;
    Log("exe: %s", path);
    Log("exe base=%08X TimeDateStamp=%08X SizeOfImage=%08X text=%08X-%08X", (unsigned)(UINT_PTR)exe, ts, sz,
        (unsigned)g_textBegin, (unsigned)g_textEnd);
    Log("exe identity: %s", g_gameMatch ? "MATCH (Darksiders II PC 2012)" : "UNKNOWN BUILD");
}

static BOOL load_real(void)
{
    char sys[MAX_PATH];
    GetSystemDirectoryA(sys, MAX_PATH);  // WOW64 redirects System32 -> SysWOW64 for this 32-bit process
    strcat_s(sys, MAX_PATH, "\\dinput8.dll");
    g_real = LoadLibraryA(sys);
    if (!g_real) {
        Log("ERROR: cannot load real %s (err %lu)", sys, GetLastError());
        return FALSE;
    }
    p_DirectInput8Create = (DirectInput8Create_t)GetProcAddress(g_real, "DirectInput8Create");
    p_DllCanUnloadNow = (DllCanUnloadNow_t)GetProcAddress(g_real, "DllCanUnloadNow");
    p_DllGetClassObject = (DllGetClassObject_t)GetProcAddress(g_real, "DllGetClassObject");
    p_DllRegisterServer = (DllRegisterServer_t)GetProcAddress(g_real, "DllRegisterServer");
    p_DllUnregisterServer = (DllRegisterServer_t)GetProcAddress(g_real, "DllUnregisterServer");
    p_GetdfDIJoystick = (GetdfDIJoystick_t)GetProcAddress(g_real, "GetdfDIJoystick");
    Log("real dinput8: %s base=%08X", sys, (unsigned)(UINT_PTR)g_real);
    return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        HMODULE pinned;
        char env[8] = "";
        g_self = h;
        DisableThreadLibraryCalls(h);
        // pin: hooks point into this DLL, it must never be unloaded
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (LPCSTR)DllMain, &pinned);
        InitializeCriticalSection(&g_logLock);
        init_log_path();
        Log("==== %s loaded, self base=%08X, pid=%lu ====", DBGMENU_VERSION, (unsigned)(UINT_PTR)h,
            GetCurrentProcessId());
        check_exe();
        load_real();  // LoadLibrary of a system DLL under loader lock is the usual proxy practice
        g_testMode = GetEnvironmentVariableA("DBGMENU_TEST", env, sizeof(env)) && env[0] == '1';
        Input_Init();
        if (g_gameMatch || g_testMode) {
            if (g_testMode) Log("TEST MODE (DBGMENU_TEST=1): hooks installed without the game");
            Hooks_Install();
        } else {
            Log("unknown exe: hooks NOT installed, proxy only");
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        Flight_OnUnload(reserved != NULL);
        Cursor_OnUnload();
        Log("==== unload (process exit=%d) ====", reserved != NULL);
    }
    return TRUE;
}

// ---- forwarded exports (names via dinput8.def) ----
extern "C" HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE hinst, DWORD ver, REFIID riid, LPVOID *out, void *outer)
{
    HRESULT hr;
    if (!p_DirectInput8Create) return E_FAIL;
    hr = p_DirectInput8Create(hinst, ver, riid, out, outer);
    Log("DirectInput8Create(ver=%08X) -> hr=%08X obj=%08X (caller %08X)", ver, hr,
        (unsigned)(UINT_PTR)(out ? *out : 0), (unsigned)(UINT_PTR)_ReturnAddress());
    return hr;
}
extern "C" HRESULT WINAPI Proxy_DllCanUnloadNow(void) { return p_DllCanUnloadNow ? p_DllCanUnloadNow() : S_FALSE; }
extern "C" HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID c, REFIID r, LPVOID *o)
{
    return p_DllGetClassObject ? p_DllGetClassObject(c, r, o) : E_FAIL;
}
extern "C" HRESULT WINAPI Proxy_DllRegisterServer(void) { return p_DllRegisterServer ? p_DllRegisterServer() : E_FAIL; }
extern "C" HRESULT WINAPI Proxy_DllUnregisterServer(void) { return p_DllUnregisterServer ? p_DllUnregisterServer() : E_FAIL; }
extern "C" LPCVOID WINAPI Proxy_GetdfDIJoystick(void) { return p_GetdfDIJoystick ? p_GetdfDIJoystick() : NULL; }

// Offline test harness only (does nothing in the game): 1..4, 7 fake game state, 5 = exception in the flight frame,
// 6 = log the simulated cursor state, 8..11, 13 triggers (projection check, empty / broken / normal fake list, exception).
extern "C" void __cdecl DbgMenuTest(int cmd)
{
    if (!g_testMode || g_gameMatch) return;
    if (cmd == 5) Flight_TestThrow();
    else if (cmd == 6) Cursor_TestLog("check");
    else if ((cmd >= 8 && cmd <= 11) || cmd == 13 || cmd == 14) Trig_TestControl(cmd);
    else Game_TestControl(cmd);
}

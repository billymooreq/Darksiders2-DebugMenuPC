// Minimal dinput8.dll proxy for Darksiders II (PC 2012, x86).
// Only logs (load, exe identity, module bases) and forwards all exports to the system dinput8.dll.
// No hooks, no memory writes into the game.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <intrin.h>

#define DS2PROXY_VERSION "proxy_dinput8 v0.1 (task 01)"
#define EXPECTED_TIMESTAMP 0x50241AFAu  // Darksiders2.exe PE TimeDateStamp (2012-08-09)
#define EXPECTED_IMAGESIZE 0x0147D000u

static HMODULE g_self;
static HMODULE g_real;
static CRITICAL_SECTION g_logLock;
static char g_logPath[MAX_PATH];

typedef HRESULT (WINAPI *DirectInput8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID *, void *);
typedef HRESULT (WINAPI *DllCanUnloadNow_t)(void);
typedef HRESULT (WINAPI *DllGetClassObject_t)(REFCLSID, REFIID, LPVOID *);
typedef HRESULT (WINAPI *DllRegisterServer_t)(void);
typedef LPCVOID (WINAPI *GetdfDIJoystick_t)(void);

static DirectInput8Create_t p_DirectInput8Create;
static DllCanUnloadNow_t p_DllCanUnloadNow;
static DllGetClassObject_t p_DllGetClassObject;
static DllRegisterServer_t p_DllRegisterServer;
static DllRegisterServer_t p_DllUnregisterServer;
static GetdfDIJoystick_t p_GetdfDIJoystick;

static void logf(const char *fmt, ...)
{
    char line[1024];
    SYSTEMTIME t;
    va_list ap;
    int n;
    FILE *f;

    GetLocalTime(&t);
    n = _snprintf_s(line, sizeof(line), _TRUNCATE, "%02u:%02u:%02u.%03u [%5lu] ",
                    t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentThreadId());
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

// Log goes to <game dir>\..\_claudework\logs\proxy_dinput8.log; falls back to the game dir.
static void init_log_path(void)
{
    char dir[MAX_PATH], *p;
    GetModuleFileNameA(g_self, dir, MAX_PATH);
    p = strrchr(dir, '\\');
    if (p) *p = 0;
    _snprintf_s(g_logPath, MAX_PATH, _TRUNCATE, "%s\\..\\_claudework\\logs", dir);
    if (GetFileAttributesA(g_logPath) == INVALID_FILE_ATTRIBUTES)
        _snprintf_s(g_logPath, MAX_PATH, _TRUNCATE, "%s", dir);
    strcat_s(g_logPath, MAX_PATH, "\\proxy_dinput8.log");
}

static void log_module(const char *name)
{
    HMODULE h = GetModuleHandleA(name);
    char path[MAX_PATH] = "";
    if (h) GetModuleFileNameA(h, path, MAX_PATH);
    logf("  module %-16s %s base=%08X %s", name, h ? "loaded " : "absent ", (unsigned)(UINT_PTR)h, path);
}

static void log_exe_identity(void)
{
    char path[MAX_PATH];
    HMODULE exe = GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)exe;
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)((BYTE *)exe + dos->e_lfanew);
    DWORD ts = nt->FileHeader.TimeDateStamp, sz = nt->OptionalHeader.SizeOfImage;

    GetModuleFileNameA(NULL, path, MAX_PATH);
    logf("exe: %s", path);
    logf("exe base=%08X (preferred %08X) TimeDateStamp=%08X SizeOfImage=%08X CheckSum=%08X entry=%08X",
         (unsigned)(UINT_PTR)exe, nt->OptionalHeader.ImageBase, ts, sz, nt->OptionalHeader.CheckSum,
         (unsigned)(UINT_PTR)exe + nt->OptionalHeader.AddressOfEntryPoint);
    logf("exe identity: %s", (ts == EXPECTED_TIMESTAMP && sz == EXPECTED_IMAGESIZE)
                                 ? "MATCH (Darksiders II PC 2012, SHA-1 1402d089...)" : "UNKNOWN BUILD");
}

static void log_modules(const char *when)
{
    logf("modules (%s):", when);
    log_module("d3d9.dll");
    log_module("d3d11.dll");
    log_module("dxgi.dll");
    log_module("d3dx9_43.dll");
    log_module("xinput1_3.dll");
    log_module("steam_api.dll");
    log_module("binkw32.dll");
}

// Read-only: after a while, note which graphics DLLs the game actually loaded (DX9 vs DX11).
static DWORD WINAPI watcher(LPVOID arg)
{
    (void)arg;
    Sleep(15000);
    log_modules("+15 s");
    Sleep(45000);
    log_modules("+60 s");
    return 0;
}

static BOOL load_real(void)
{
    char sys[MAX_PATH];
    GetSystemDirectoryA(sys, MAX_PATH);  // WOW64 redirects System32 -> SysWOW64 for this 32-bit process
    strcat_s(sys, MAX_PATH, "\\dinput8.dll");
    g_real = LoadLibraryA(sys);
    if (!g_real) {
        logf("ERROR: cannot load real %s (err %lu)", sys, GetLastError());
        return FALSE;
    }
    p_DirectInput8Create = (DirectInput8Create_t)GetProcAddress(g_real, "DirectInput8Create");
    p_DllCanUnloadNow = (DllCanUnloadNow_t)GetProcAddress(g_real, "DllCanUnloadNow");
    p_DllGetClassObject = (DllGetClassObject_t)GetProcAddress(g_real, "DllGetClassObject");
    p_DllRegisterServer = (DllRegisterServer_t)GetProcAddress(g_real, "DllRegisterServer");
    p_DllUnregisterServer = (DllRegisterServer_t)GetProcAddress(g_real, "DllUnregisterServer");
    p_GetdfDIJoystick = (GetdfDIJoystick_t)GetProcAddress(g_real, "GetdfDIJoystick");
    logf("real dinput8: %s base=%08X DirectInput8Create=%08X", sys, (unsigned)(UINT_PTR)g_real,
         (unsigned)(UINT_PTR)p_DirectInput8Create);
    return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        HANDLE th;
        HMODULE pinned;
        g_self = h;
        DisableThreadLibraryCalls(h);
        // pin: the watcher thread must never outlive our code (FreeLibrary becomes a no-op)
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (LPCSTR)DllMain, &pinned);
        InitializeCriticalSection(&g_logLock);
        init_log_path();
        logf("==== %s loaded, self base=%08X, pid=%lu ====", DS2PROXY_VERSION, (unsigned)(UINT_PTR)h,
             GetCurrentProcessId());
        log_exe_identity();
        load_real();  // LoadLibrary of a system DLL under loader lock is the usual proxy practice
        log_modules("attach");
        th = CreateThread(NULL, 0, watcher, NULL, 0, NULL);
        if (th) CloseHandle(th);
    } else if (reason == DLL_PROCESS_DETACH) {
        logf("==== unload (process exit=%d) ====", reserved != NULL);
    }
    return TRUE;
}

// ---- forwarded exports (names via proxy.def) ----
HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE hinst, DWORD ver, REFIID riid, LPVOID *out, void * outer)
{
    HRESULT hr;
    if (!p_DirectInput8Create) return E_FAIL;
    hr = p_DirectInput8Create(hinst, ver, riid, out, outer);
    logf("DirectInput8Create(ver=%08X) -> hr=%08X obj=%08X (caller %08X)", ver, hr,
         (unsigned)(UINT_PTR)(out ? *out : 0), (unsigned)(UINT_PTR)_ReturnAddress());
    return hr;
}
HRESULT WINAPI Proxy_DllCanUnloadNow(void) { return p_DllCanUnloadNow ? p_DllCanUnloadNow() : S_FALSE; }
HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID c, REFIID r, LPVOID *o)
{
    return p_DllGetClassObject ? p_DllGetClassObject(c, r, o) : E_FAIL;
}
HRESULT WINAPI Proxy_DllRegisterServer(void) { return p_DllRegisterServer ? p_DllRegisterServer() : E_FAIL; }
HRESULT WINAPI Proxy_DllUnregisterServer(void) { return p_DllUnregisterServer ? p_DllUnregisterServer() : E_FAIL; }
LPCVOID WINAPI Proxy_GetdfDIJoystick(void) { return p_GetdfDIJoystick ? p_GetdfDIJoystick() : NULL; }

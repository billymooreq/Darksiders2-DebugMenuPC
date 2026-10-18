// Offline smoke test for the proxy (no game): loads .\dinput8.dll, calls DirectInput8Create, releases.
// Build: msvc32.bat cl /nologo test_load.c   Run from a folder that contains the built dinput8.dll.
#include <windows.h>
#include <stdio.h>

typedef HRESULT (WINAPI *DI8Create_t)(HINSTANCE, DWORD, const GUID *, void **, void *);
static const GUID IID_IDirectInput8A = {0xBF798030, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};

int main(void)
{
    HMODULE h;
    DI8Create_t f;
    void *obj = NULL;
    HRESULT hr;
    char path[MAX_PATH] = "";

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("start\n");
    h = LoadLibraryA(".\\dinput8.dll");
    if (!h) { printf("LoadLibrary failed %lu\n", GetLastError()); return 1; }
    GetModuleFileNameA(h, path, MAX_PATH);
    printf("loaded %p %s\n", (void *)h, path);
    f = (DI8Create_t)GetProcAddress(h, "DirectInput8Create");
    printf("DirectInput8Create = %p\n", (void *)f);
    hr = f(GetModuleHandleA(NULL), 0x0800, &IID_IDirectInput8A, &obj, NULL);
    printf("hr=%08lX obj=%p\n", hr, obj);
    if (obj) ((ULONG(WINAPI *)(void *))(*(void ***)obj)[2])(obj);
    FreeLibrary(h);
    if (__argc > 1) { printf("waiting 16 s (watcher thread after FreeLibrary)...\n"); Sleep(16000); }
    printf("done\n");
    return 0;
}

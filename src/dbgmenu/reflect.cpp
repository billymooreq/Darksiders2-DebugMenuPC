// Task 13: the game's reflection (gfc Object / Class / Property, notes/triggers_pc.md §7) - every property of a
// reflected object by name, type and value, the way the game's editor and serializer see them. Used by the trigger
// info (all fields of a trigger) and the map lines of showCameraPosition (region names).
//   class descriptor (vt 0x0116228C, scripted classes 0x01166A08): +8 parent, +0x10 Name, +0x1C {data, count} of
//     property handles (addProperty 0x0048A740 -> push_back 0x005757C4).
//   property (a static object, vtable per property): +4 refcount, +8 Name (its name);
//     vt[1] (+4) type = a constant (push N; pop eax; ret); vt[4] (+0x10) element type of a container;
//     vt[0xC] (+0x30) makes a value holder: 55 8B EC 51 83 65 FC 00 68 <getter> - the getter (thiscall, ECX = obj)
//       is a field read: lea / mov eax|ax|al|fld [ecx+d] (+ shr / and 1 for bit fields), or mov eax, [ecx];
//       jmp [eax+slot] (virtual, e.g. Position = vt[0x38] 0x008EF6B2: copies 4 dwords from +0x90);
//     arrays: vt[0xD] (+0x34) count: 55 8B EC 8B 45 08 8B 80 <d> 5D C2 04 00 -> {data, count, cap} at d-4;
//       element size: Name 8, int / float / object 4, struct (type 11) from the reserve vt[0xE] (+0x38) (shl ecx, n).
//   types: 2 int, 3 float, 4 bool, 5 std::string, 7 object, 9 array, 10 map, 11 struct of dwords, 13 64 bit, 15 Name.
// Only Name::c_str 0x00497245 is called; every getter is parsed byte by byte, never called. A getter of another
// shape -> the property is skipped and logged once (first bytes), so the next version can learn it.
#include "common.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

#define SPRINTF(buf, ...) _snprintf_s(buf, sizeof(buf), _TRUNCATE, __VA_ARGS__)
#define STRCPY(buf, src) strncpy_s(buf, sizeof(buf), src, _TRUNCATE)

#define PC_VT_CLASS        0x0116228Cu
#define PC_CLS_GETNAME     0x0054B0DDu   // class vt[8] (both class vtables)
#define PC_CLS_ISKINDOF    0x004898FDu   // class vt[0x10]
#define PC_NAME_CSTR       0x00497245u   // const char *__thiscall Name::c_str(Name *)
#define PC_OBJ_GETNAME     0x005A327Bu   // WorldObject vt[0x2C]: lea eax, [ecx+0x50]
#define OFS_CLS_PARENT     0x08
#define OFS_CLS_NAME       0x10
#define OFS_CLS_PROPS      0x1C
#define OFS_PROP_NAME      0x08
#define OFS_OBJ_NAME       0x50
#define VT_GETCLASS        0x04
#define VT_GETNAME         0x2C
#define PVT_TYPE           0x04
#define PVT_ELEM           0x10
#define PVT_HOLDER         0x30
#define PVT_COUNT          0x34
#define PVT_RESERVE        0x38
#define FAKE_NAME_TAG      0x7E58u

enum { AK_NONE, AK_ADDR, AK_LOAD, AK_FLD, AK_VIRT, AK_LOAD64 };
struct Acc {
    BYTE kind, size, shift, bit;  // AK_LOAD: size 1/2/4, shr shift, bit = and 1
    int off;                      // field offset (AK_VIRT: the vtable slot)
    int bytes;                    // AK_ADDR of a struct: its size (copy-out getter: 4 * movsd)
};
struct PropEnt {
    void *prop;
    bool ok;
    int type, elem, elemSize, count;  // count: offset of the array count (arrays)
    Acc acc;
    char name[48];
};
static PropEnt s_props[4096];
static volatile LONG s_unknownBudget, s_exBudget;

static bool Fake() { return g_testMode && !g_gameMatch; }
static bool CodeOk(const void *p) { return Fake() ? p != NULL : InText(p); }
static bool DataOk(const void *p) { return Fake() ? p != NULL : InImage(p); }

static int CatchRefl(const char *where, EXCEPTION_POINTERS *ep)
{
    if (LogBudget(&s_exBudget, 30))
        Log("reflect: exception %08X at %08X in %s", ep->ExceptionRecord->ExceptionCode,
            (unsigned)(UINT_PTR)ep->ExceptionRecord->ExceptionAddress, where);
    return EXCEPTION_EXECUTE_HANDLER;
}

// ======================= Name -> text (cached; the game's lookup takes a lock) =======================
typedef const char *(__fastcall *CStr_t)(const void *name, void *edx);
static const char *__fastcall FakeCStr(const void *name, void *)
{
    DWORD a = ((const DWORD *)name)[0], b = ((const DWORD *)name)[1];
    return (b == FAKE_NAME_TAG && a) ? (const char *)(UINT_PTR)a : "";
}
void Refl_FakeName(void *name, const char *text)
{
    ((DWORD *)name)[0] = (DWORD)(UINT_PTR)text;
    ((DWORD *)name)[1] = text ? FAKE_NAME_TAG : 0;
}

struct NameEnt { DWORD a, b; bool used; char s[64]; };
static NameEnt s_names[4096];
const char *Refl_NameText(const void *p)
{
    DWORD a, b;
    __try {
        a = ((const DWORD *)p)[0];
        b = ((const DWORD *)p)[1];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "";
    }
    if (!a && !b) return "";
    unsigned h = (a ^ (b * 0x9E3779B1u)) & 4095;
    for (int i = 0; i < 8; i++) {
        NameEnt &e = s_names[(h + i) & 4095];
        if (e.used && e.a == a && e.b == b) return e.s;
        if (!e.used) break;
    }
    const char *s = NULL;
    DWORD tmp[2] = {a, b};
    __try {
        s = (Fake() ? FakeCStr : (CStr_t)(UINT_PTR)PC_NAME_CSTR)(tmp, NULL);
    } __except (CatchRefl("Name::c_str 0x00497245", GetExceptionInformation())) {
        s = NULL;
    }
    NameEnt *slot = &s_names[h];  // the table is full around h: overwrite the first one
    for (int i = 0; i < 8; i++)
        if (!s_names[(h + i) & 4095].used) {
            slot = &s_names[(h + i) & 4095];
            break;
        }
    slot->a = a, slot->b = b, slot->used = true;
    slot->s[0] = 0;
    __try {
        if (s) {
            int n = 0;
            while (s[n] && n < (int)sizeof(slot->s) - 1) {
                char c = s[n];
                slot->s[n++] = (c >= 32 && c < 127) ? c : '?';
            }
            slot->s[n] = 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        slot->s[0] = 0;
    }
    return slot->s;
}

// ======================= classes =======================
static bool ClassDescOk(void *cls)
{
    __try {
        void **vt = *(void ***)cls;
        if (!DataOk(vt)) return false;
        return vt[8 / 4] == (void *)(UINT_PTR)PC_CLS_GETNAME && vt[0x10 / 4] == (void *)(UINT_PTR)PC_CLS_ISKINDOF;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// getClass (vt[1]) is called only when it is "mov eax, [abs]" / "mov eax, [ecx+d]" / "lea" chains + ret
static bool PlainGetter(const BYTE *b)
{
    int o = 0;
    for (int n = 0; n < 4; n++) {
        if (b[o] == 0xC3) return n > 0;
        if (b[o] == 0xA1) { o += 5; continue; }
        if (b[o] == 0x8B || b[o] == 0x8D) {
            BYTE m = b[o + 1];
            if (m == 0x41 || m == 0x40) { o += 3; continue; }
            if (m == 0x81 || m == 0x80) { o += 6; continue; }
            if (m == 0x01 || m == 0x00) { o += 2; continue; }
        }
        return false;
    }
    return b[o] == 0xC3;
}

typedef void *(__fastcall *GetPtr_t)(void *self, void *edx);
void *Refl_ClassOf(void *obj)
{
    if (!obj) return NULL;
    __try {
        void **vt = *(void ***)obj;
        if (!DataOk(vt)) return NULL;
        void *fn = vt[VT_GETCLASS / 4];
        if (!CodeOk(fn) || (!Fake() && !PlainGetter((const BYTE *)fn))) return NULL;
        void *cls = ((GetPtr_t)fn)(obj, NULL);
        return (cls && ClassDescOk(cls)) ? cls : NULL;
    } __except (CatchRefl("getClass", GetExceptionInformation())) {
        return NULL;
    }
}

const char *Refl_ClassName(void *cls) { return cls ? Refl_NameText((BYTE *)cls + OFS_CLS_NAME) : ""; }

void *Refl_ClassParent(void *cls)
{
    __try {
        void *p = *(void **)((BYTE *)cls + OFS_CLS_PARENT);
        return (p && ClassDescOk(p)) ? p : NULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
}

bool Refl_IsKindOf(void *obj, const char *className)
{
    void *c = Refl_ClassOf(obj);
    for (int d = 0; c && d < 40; d++, c = Refl_ClassParent(c))
        if (!strcmp(Refl_ClassName(c), className)) return true;
    return false;
}

// "a > b > c" (most derived first), at most `levels`
void Refl_ClassChain(void *obj, char *buf, int size, int levels)
{
    buf[0] = 0;
    void *c = Refl_ClassOf(obj);
    for (int d = 0; c && d < levels; d++, c = Refl_ClassParent(c)) {
        if (d) strncat_s(buf, size, " > ", _TRUNCATE);
        strncat_s(buf, size, Refl_ClassName(c), _TRUNCATE);
    }
}

const char *Refl_ObjName(void *obj)
{
    __try {
        void *fn = (*(void ***)obj)[VT_GETNAME / 4];
        const BYTE *b = (const BYTE *)fn;
        if (!CodeOk(fn)) return "";
        // lea eax, [ecx+0x50]; ret (0x005A327B) - the WorldObject Name
        if (fn != (void *)(UINT_PTR)PC_OBJ_GETNAME && !(b[0] == 0x8D && b[1] == 0x41 && b[2] == OFS_OBJ_NAME && b[3] == 0xC3))
            return "";
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "";
    }
    return Refl_NameText((BYTE *)obj + OFS_OBJ_NAME);
}

// ======================= property accessors (parsed, never called) =======================
static int ConstRet(const BYTE *c)
{
    if (c[0] == 0x6A && c[2] == 0x58 && c[3] == 0xC3) return (signed char)c[1];
    if (c[0] == 0x33 && c[1] == 0xC0 && c[2] == 0xC3) return 0;
    if (c[0] == 0xB0 && c[2] == 0xC3) return c[1];
    if (c[0] == 0xB8 && c[5] == 0xC3) return *(const int *)(c + 1);
    return -1;
}

// [ecx + disp8 / disp32] after the opcode: m = modrm; returns the length of modrm + disp, 0 = other
static int EcxDisp(const BYTE *b, int *off)
{
    if (b[0] == 0x41) { *off = (signed char)b[1]; return 2; }
    if (b[0] == 0x81) { *off = *(const int *)(b + 1); return 5; }
    if (b[0] == 0x01) { *off = 0; return 1; }
    return 0;
}

static bool ParseGetter(const BYTE *b, Acc *a)
{
    memset(a, 0, sizeof(*a));
    int o = 0, n;
    // mov eax, [ecx]; jmp [eax+slot] - a virtual getter
    if (b[0] == 0x8B && b[1] == 0x01 && b[2] == 0xFF && (b[3] == 0x60 || b[3] == 0xA0)) {
        a->kind = AK_VIRT;
        a->off = b[3] == 0x60 ? b[4] : *(const int *)(b + 4);
        return true;
    }
    // copy-out: 55 8B EC 8B 45 08 56 57 8D B1 <d> 8B F8 A5..A5 5F 5E 5D C2 04 00 (getPos 0x008EF6B2 ...)
    if (!memcmp(b, "\x55\x8B\xEC\x8B\x45\x08\x56\x57\x8D", 9) && (b[9] == 0xB1 || b[9] == 0x71)) {
        int d, l = b[9] == 0xB1 ? 4 : 1;
        d = l == 4 ? *(const int *)(b + 10) : (signed char)b[10];
        o = 10 + l;
        if (b[o] != 0x8B || b[o + 1] != 0xF8) return false;
        o += 2;
        int k = 0;
        while (b[o] == 0xA5 && k < 16) o++, k++;
        if (!k || memcmp(b + o, "\x5F\x5E\x5D\xC2\x04\x00", 6)) return false;
        a->kind = AK_ADDR;
        a->off = d;
        a->bytes = k * 4;
        return true;
    }
    if (b[0] == 0x8D && (n = EcxDisp(b + 1, &a->off)) && b[1 + n] == 0xC3) {  // lea eax, [ecx+d]
        a->kind = AK_ADDR;
        return true;
    }
    if (b[0] == 0xD9 && (n = EcxDisp(b + 1, &a->off)) && b[1 + n] == 0xC3) {  // fld dword [ecx+d]
        a->kind = AK_FLD;
        a->size = 4;
        return true;
    }
    // 64 bit: mov eax, [ecx+d]; mov edx, [ecx+d+4]; ret
    if (b[0] == 0x8B && b[1] == 0x41 && b[3] == 0x8B && b[4] == 0x51 && b[5] == (BYTE)(b[2] + 4) && b[6] == 0xC3) {
        a->kind = AK_LOAD64;
        a->off = (signed char)b[2];
        a->size = 8;
        return true;
    }
    if (b[0] == 0x8B) a->size = 4, o = 1;                                  // mov eax, [ecx+d]
    else if (b[0] == 0x8A) a->size = 1, o = 1;                             // mov al, [ecx+d]
    else if (b[0] == 0x66 && b[1] == 0x8B) a->size = 2, o = 2;             // mov ax, [ecx+d]
    else if (b[0] == 0x0F && b[1] == 0xB6) a->size = 1, o = 2;             // movzx eax, byte [ecx+d]
    else if (b[0] == 0x0F && b[1] == 0xB7) a->size = 2, o = 2;             // movzx eax, word [ecx+d]
    else return false;
    if (!(n = EcxDisp(b + o, &a->off))) return false;
    o += n;
    a->kind = AK_LOAD;
    if ((b[o] == 0xD1 || b[o] == 0xD0) && b[o + 1] == 0xE8) a->shift = 1, o += 2;                   // shr 1
    else if ((b[o] == 0xC1 || b[o] == 0xC0) && b[o + 1] == 0xE8) a->shift = b[o + 2] & 31, o += 3;  // shr n
    if (b[o] == 0x24 && b[o + 1] == 0x01) a->bit = 1, o += 2;                                      // and al, 1
    else if (b[o] == 0x83 && b[o + 1] == 0xE0 && b[o + 2] == 0x01) a->bit = 1, o += 3;             // and eax, 1
    return b[o] == 0xC3;
}

// the value holder vt[0xC]: 55 8B EC 51 83 65 FC 00 68 <getter>
static void *HolderGetter(void *fn)
{
    const BYTE *b = (const BYTE *)fn;
    if (memcmp(b, "\x55\x8B\xEC\x51\x83\x65\xFC\x00\x68", 9)) return NULL;
    return *(void *const *)(b + 9);
}

// array count vt[0xD]: 55 8B EC 8B 45 08 8B 80 <d32> | 8B 40 <d8> 5D C2 04 00
static int CountOffset(const BYTE *b)
{
    if (memcmp(b, "\x55\x8B\xEC\x8B\x45\x08\x8B", 7)) return -1;
    if (b[7] == 0x80 && !memcmp(b + 12, "\x5D\xC2\x04\x00", 4)) return *(const int *)(b + 8);
    if (b[7] == 0x40 && !memcmp(b + 9, "\x5D\xC2\x04\x00", 4)) return (signed char)b[8];
    return -1;
}

// element size of a struct array: reserve vt[0xE] = 55 8B EC 8B 45 08 FF 75 0C 05 <d32> 50 E8 <rel> -> the callee
// multiplies by shl ecx|ebx, n / imul
static int ReserveElemSize(const BYTE *b)
{
    if (memcmp(b, "\x55\x8B\xEC\x8B\x45\x08\xFF\x75\x0C\x05", 10) || b[14] != 0x50 || b[15] != 0xE8) return 0;
    const BYTE *c = b + 20 + *(const int *)(b + 16);
    if (!CodeOk(c)) return 0;
    for (int i = 0; i < 0x60; i++) {
        if (c[i] == 0xC1 && (c[i + 1] == 0xE1 || c[i + 1] == 0xE3) && c[i + 2] >= 1 && c[i + 2] <= 6) return 1 << c[i + 2];
        if (c[i] == 0x6B && (c[i + 1] == 0xC9 || c[i + 1] == 0xDB) && c[i + 2] > 0 && c[i + 2] <= 64) return c[i + 2];
        if (c[i] == 0xC3 || c[i] == 0xC2) break;
    }
    return 0;
}

static void LogUnknown(PropEnt &e, const char *what, const void *fn)
{
    if (!LogBudget(&s_unknownBudget, 80)) return;
    const BYTE *b = (const BYTE *)fn;
    char hex[64] = "";
    __try {
        for (int i = 0; b && i < 14; i++) {
            char t[4];
            sprintf_s(t, "%02X ", b[i]);
            strcat_s(hex, t);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    Log("reflect: property %s (type %d, elem %d, vt %08X): %s %08X not understood: %s- skipped", e.name, e.type,
        e.elem, (unsigned)(UINT_PTR)(e.prop ? *(void **)e.prop : 0), what, (unsigned)(UINT_PTR)fn, hex);
}

static PropEnt *PropOf(void *prop)
{
    unsigned h = ((unsigned)(UINT_PTR)prop >> 3) & 4095;
    for (int i = 0; i < 16; i++) {
        PropEnt &e = s_props[(h + i) & 4095];
        if (e.prop == prop) return &e;
        if (!e.prop) break;
    }
    PropEnt *slot = &s_props[h];
    for (int i = 0; i < 16; i++)
        if (!s_props[(h + i) & 4095].prop) {
            slot = &s_props[(h + i) & 4095];
            break;
        }
    PropEnt &e = *slot;
    memset(&e, 0, sizeof(e));
    e.prop = prop;
    e.type = e.elem = -1;
    e.count = -1;
    __try {
        void **vt = *(void ***)prop;
        if (!DataOk(vt)) return &e;
        STRCPY(e.name, Refl_NameText((BYTE *)prop + OFS_PROP_NAME));
        if (!e.name[0]) return &e;
        if (!CodeOk(vt[PVT_TYPE / 4]) || !CodeOk(vt[PVT_ELEM / 4])) return &e;
        e.type = ConstRet((const BYTE *)vt[PVT_TYPE / 4]);
        e.elem = ConstRet((const BYTE *)vt[PVT_ELEM / 4]);
        if (e.type == 9) {  // array
            void *cf = vt[PVT_COUNT / 4];
            if (!CodeOk(cf) || (e.count = CountOffset((const BYTE *)cf)) < 4 || e.count > 0x4000) {
                LogUnknown(e, "array count vt[0xD]", cf);
                e.count = -1;
                return &e;
            }
            switch (e.elem) {
            case 15: e.elemSize = 8; break;
            case 2: case 3: case 7: e.elemSize = 4; break;
            case 4: e.elemSize = 1; break;
            case 5: e.elemSize = 28; break;
            case 11: {
                void *rf = vt[PVT_RESERVE / 4];
                e.elemSize = CodeOk(rf) ? ReserveElemSize((const BYTE *)rf) : 0;
                break;
            }
            default: e.elemSize = 0; break;
            }
            e.ok = true;
            return &e;
        }
        void *hf = vt[PVT_HOLDER / 4];
        void *g = CodeOk(hf) ? HolderGetter(hf) : NULL;
        if (!g || !CodeOk(g)) {
            if (e.type == 2 || e.type == 3 || e.type == 4 || e.type == 5 || e.type == 7 || e.type == 11 ||
                e.type == 13 || e.type == 15)
                LogUnknown(e, "value holder vt[0xC]", hf);
            return &e;
        }
        if (!ParseGetter((const BYTE *)g, &e.acc) || (e.acc.kind != AK_VIRT && (e.acc.off < 0 || e.acc.off > 0x4000))) {
            LogUnknown(e, "getter", g);
            e.acc.kind = AK_NONE;
            return &e;
        }
        if (e.type == 11 && e.acc.kind == AK_ADDR && !e.acc.bytes) e.acc.bytes = 12;  // a vector (x y z)
        e.ok = true;
    } __except (CatchRefl("property", GetExceptionInformation())) {
        e.ok = false;
    }
    return &e;
}

// ======================= values -> text =======================
static void Fmt(char *out, int size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(out, size, _TRUNCATE, fmt, ap);
    va_end(ap);
}

static void Cat(char *out, int size, const char *s) { strncat_s(out, size, s, _TRUNCATE); }

// a dword of a struct: an int when it looks like one (region ids, -1), else a float
static void DwordText(DWORD v, char *out, int size)
{
    int i = (int)v;
    float f = *(float *)&v;
    if (i >= -100000 && i <= 100000) Fmt(out, size, "%d", i);
    else if (_finite(f)) Fmt(out, size, fabsf(f) >= 100 ? "%.0f" : "%.2f", f);
    else Fmt(out, size, "%08X", v);
}

static void StdString(const BYTE *p, char *out, int size)
{
    unsigned len = *(const unsigned *)(p + 0x10), cap = *(const unsigned *)(p + 0x14);
    const char *s = cap >= 16 ? *(const char *const *)p : (const char *)p;
    if (len > 400 || cap < len) {
        Fmt(out, size, "?");
        return;
    }
    int n = 0;
    for (unsigned i = 0; i < len && n < size - 1; i++) out[n++] = (s[i] >= 32 && s[i] < 127) ? s[i] : '?';
    out[n] = 0;
}

static void ObjText(void *o, char *out, int size)
{
    if (!o) {
        Fmt(out, size, "none");
        return;
    }
    void *c = Refl_ClassOf(o);
    const char *n = c ? Refl_ObjName(o) : "";
    if (c) Fmt(out, size, "%s%s%s%s", Refl_ClassName(c), n[0] ? " '" : "", n, n[0] ? "'" : "");
    else Fmt(out, size, "object %08X", (unsigned)(UINT_PTR)o);
}

// one value of type t at p (a field or an array element of `bytes`)
static void ValueText(int t, const BYTE *p, int bytes, char *out, int size)
{
    switch (t) {
    case 2: Fmt(out, size, "%d", *(const int *)p); break;
    case 3: Fmt(out, size, "%g", *(const float *)p); break;
    case 4: Fmt(out, size, "%s", *p ? "true" : "false"); break;
    case 5: StdString(p, out, size); break;
    case 7: ObjText(*(void *const *)p, out, size); break;
    case 13: Fmt(out, size, "%08X%08X", ((const DWORD *)p)[1], ((const DWORD *)p)[0]); break;
    case 15: {
        const char *s = Refl_NameText(p);
        Fmt(out, size, "%s", s[0] ? s : "\"\"");
        break;
    }
    case 11: {
        out[0] = 0;
        int n = bytes / 4;
        if (n > 6) n = 6;
        Cat(out, size, "(");
        for (int i = 0; i < n; i++) {
            char t1[24];
            DwordText(((const DWORD *)p)[i], t1, sizeof(t1));
            if (i) Cat(out, size, " ");
            Cat(out, size, t1);
        }
        Cat(out, size, ")");
        break;
    }
    default: Fmt(out, size, "(type %d)", t); break;
    }
}

// the address of the field (AK_ADDR / virtual copy-out getter), or NULL (then AK_LOAD / AK_FLD read at off)
static const BYTE *FieldAddr(void *obj, const Acc &a, Acc *res)
{
    *res = a;
    if (a.kind == AK_VIRT) {
        void *fn = (*(void ***)obj)[a.off / 4];
        if (!CodeOk(fn) || !ParseGetter((const BYTE *)fn, res) || res->kind == AK_VIRT) {
            res->kind = AK_NONE;
            return NULL;
        }
        if (res->kind == AK_ADDR && !res->bytes) res->bytes = 12;
    }
    return (BYTE *)obj + res->off;
}

static bool PropText(void *obj, PropEnt &e, char *out, int size)
{
    out[0] = 0;
    if (!e.ok) return false;
    __try {
        if (e.type == 9) {
            const BYTE *arr = (const BYTE *)obj + e.count - 4;
            const BYTE *data = *(const BYTE *const *)arr;
            int n = *(const int *)(arr + 4);
            if (n < 0 || n > 100000) return false;
            Fmt(out, size, "[%d]", n);
            if (!n || !data || !e.elemSize) return true;
            Cat(out, size, " ");
            for (int i = 0; i < n && i < 12; i++) {
                char one[96];
                ValueText(e.elem, data + i * e.elemSize, e.elemSize, one, sizeof(one));
                if (i) Cat(out, size, ", ");
                Cat(out, size, one);
                if ((int)strlen(out) >= size - 2) break;
            }
            if (n > 12) Cat(out, size, ", ...");
            return true;
        }
        Acc a;
        const BYTE *p = FieldAddr(obj, e.acc, &a);
        if (!p || a.kind == AK_NONE) return false;
        if (a.kind == AK_ADDR) {
            int bytes = a.bytes ? a.bytes : 4;
            if (e.type == 11 && bytes == 16) {  // a copied vector4 (getPos): x y z, w = 0 / 1 is not shown
                DWORD w = ((const DWORD *)p)[3];
                if (w == 0 || w == 0x3F800000) bytes = 12;
            }
            ValueText(e.type, p, bytes, out, size);
            return true;
        }
        if (a.kind == AK_FLD) {
            Fmt(out, size, "%g", *(const float *)p);
            return true;
        }
        if (a.kind == AK_LOAD64) {
            ValueText(13, p, 8, out, size);
            return true;
        }
        DWORD v = a.size == 1 ? *p : a.size == 2 ? *(const WORD *)p : *(const DWORD *)p;
        v >>= a.shift;
        if (a.bit) v &= 1;
        if (e.type == 4) Fmt(out, size, "%s", v ? "true" : "false");
        else if (e.type == 3 && a.size == 4) Fmt(out, size, "%g", *(float *)&v);
        else if (e.type == 7) ObjText((void *)(UINT_PTR)v, out, size);
        else if (e.type == 15 || e.type == 5 || e.type == 11) return false;  // a value where an address was expected
        else Fmt(out, size, "%d", a.size == 4 ? (int)v : (int)v);
        return true;
    } __except (CatchRefl(e.name, GetExceptionInformation())) {
        out[0] = 0;
        return false;
    }
}

// ======================= public =======================
// every property of the object's class chain (most derived first; a name already seen is skipped)
int Refl_Visit(void *obj, ReflVisit fn, void *ctx)
{
    int n = 0;
    const char *seen[256];
    int nSeen = 0;
    void *c = Refl_ClassOf(obj);
    for (int d = 0; c && d < 40; d++, c = Refl_ClassParent(c)) {
        void **data;
        int count;
        __try {
            data = *(void ***)((BYTE *)c + OFS_CLS_PROPS);
            count = *(int *)((BYTE *)c + OFS_CLS_PROPS + 4);
        } __except (CatchRefl("class properties", GetExceptionInformation())) {
            break;
        }
        if (!data || count <= 0 || count > 512) continue;
        const char *cls = Refl_ClassName(c);
        for (int i = 0; i < count; i++) {
            void *prop;
            __try {
                prop = data[i];
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                break;
            }
            if (!prop) continue;
            PropEnt *e = PropOf(prop);
            if (!e->name[0]) continue;
            bool dup = false;
            for (int k = 0; k < nSeen && !dup; k++) dup = !strcmp(seen[k], e->name);
            if (dup) continue;
            if (nSeen < 256) seen[nSeen++] = e->name;
            char val[400];
            bool ok = PropText(obj, *e, val, sizeof(val));
            fn(ctx, cls, e->name, e->type, ok ? val : NULL);
            n++;
        }
    }
    return n;
}

struct FindCtx { const char *prop; char *out; int size; int type; bool found; };
static void FindOne(void *ctx, const char *, const char *prop, int type, const char *val)
{
    FindCtx *f = (FindCtx *)ctx;
    if (f->found || strcmp(prop, f->prop) || !val) return;
    strncpy_s(f->out, f->size, val, _TRUNCATE);
    f->type = type;
    f->found = true;
}
bool Refl_Get(void *obj, const char *prop, char *out, int size)
{
    FindCtx f = {prop, out, size, -1, false};
    out[0] = 0;
    Refl_Visit(obj, FindOne, &f);
    return f.found;
}

// the dwords of an array property (struct elements: LoadOnEnter = {region id, layer}); returns the element count
struct ArrCtx { const char *prop; DWORD *out; int max; int *elemDwords; int n; void *obj; };
int Refl_GetArrayDwords(void *obj, const char *prop, DWORD *out, int maxDwords, int *elemDwords)
{
    *elemDwords = 0;
    void *c = Refl_ClassOf(obj);
    for (int d = 0; c && d < 40; d++, c = Refl_ClassParent(c)) {
        __try {
            void **data = *(void ***)((BYTE *)c + OFS_CLS_PROPS);
            int count = *(int *)((BYTE *)c + OFS_CLS_PROPS + 4);
            if (!data || count <= 0 || count > 512) continue;
            for (int i = 0; i < count; i++) {
                PropEnt *e = data[i] ? PropOf(data[i]) : NULL;
                if (!e || !e->ok || e->type != 9 || strcmp(e->name, prop)) continue;
                if (e->elemSize < 4 || (e->elemSize & 3)) return 0;
                const BYTE *arr = (const BYTE *)obj + e->count - 4;
                const DWORD *dd = *(const DWORD *const *)arr;
                int n = *(const int *)(arr + 4);
                if (n < 0 || n > 100000 || (n && !dd)) return 0;
                int per = e->elemSize / 4, k = 0;
                *elemDwords = per;
                for (int j = 0; j < n && k + per <= maxDwords; j++)
                    for (int q = 0; q < per; q++) out[k++] = dd[j * per + q];
                return n;
            }
        } __except (CatchRefl("array property", GetExceptionInformation())) {
            return 0;
        }
    }
    return 0;
}

// ======================= offline test: fake classes and properties =======================
// A fake property is {vt, ref, Name name}; its vtable needs real code bytes in the slots we parse, so the getters,
// value holders, counts and type constants below are naked functions with exactly the game's instruction shapes.
#define NAKED __declspec(naked)
NAKED static void FT_Int() { __asm { push 2 } __asm { pop eax } __asm { ret } }
NAKED static void FT_Float() { __asm { push 3 } __asm { pop eax } __asm { ret } }
NAKED static void FT_Bool() { __asm { push 4 } __asm { pop eax } __asm { ret } }
NAKED static void FT_Array() { __asm { push 9 } __asm { pop eax } __asm { ret } }
NAKED static void FT_Vec() { __asm { push 11 } __asm { pop eax } __asm { ret } }
NAKED static void FT_Name() { __asm { push 15 } __asm { pop eax } __asm { ret } }
NAKED static void FT_Zero() { __asm { xor eax, eax } __asm { ret } }
// getters (thiscall, ECX = obj)
NAKED static void FG_Name50() { __asm { lea eax, [ecx + 0x50] } __asm { ret } }
NAKED static void FG_Byte11C() { __asm { mov al, byte ptr [ecx + 0x11C] } __asm { ret } }
NAKED static void FG_Byte11D() { __asm { mov al, byte ptr [ecx + 0x11D] } __asm { ret } }
NAKED static void FG_Byte11F() { __asm { mov al, byte ptr [ecx + 0x11F] } __asm { ret } }
NAKED static void FG_Bit140() { __asm { mov eax, dword ptr [ecx + 0x140] } __asm { shr eax, 1 } __asm { and al, 1 } __asm { ret } }
NAKED static void FG_Int14() { __asm { mov eax, dword ptr [ecx + 0x14] } __asm { ret } }
NAKED static void FG_Name48() { __asm { lea eax, [ecx + 0x48] } __asm { ret } }
NAKED static void FG_Name58() { __asm { lea eax, [ecx + 0x58] } __asm { ret } }
NAKED static void FG_Name130() { __asm { lea eax, [ecx + 0x130] } __asm { ret } }
NAKED static void FG_Float150() { __asm { fld dword ptr [ecx + 0x150] } __asm { ret } }
NAKED static void FG_VirtPos() { __asm { mov eax, [ecx] } __asm { jmp dword ptr [eax + 0x38] } }
NAKED static void FG_Odd() { __asm { mov eax, [ecx + 0x10] } __asm { add eax, 4 } __asm { ret } }  // not understood
#define HOLDER(n, g) NAKED static void n() { __asm { push ebp } __asm { mov ebp, esp } __asm { push ecx } \
    __asm { and dword ptr [ebp - 4], 0 } __asm { push offset g } __asm { pop ecx } __asm { leave } __asm { ret 8 } }
HOLDER(FH_Name50, FG_Name50)
HOLDER(FH_Byte11C, FG_Byte11C)
HOLDER(FH_Byte11D, FG_Byte11D)
HOLDER(FH_Byte11F, FG_Byte11F)
HOLDER(FH_Bit140, FG_Bit140)
HOLDER(FH_Int14, FG_Int14)
HOLDER(FH_Name130, FG_Name130)
HOLDER(FH_Name48, FG_Name48)
HOLDER(FH_Name58, FG_Name58)
HOLDER(FH_Float150, FG_Float150)
HOLDER(FH_VirtPos, FG_VirtPos)
HOLDER(FH_Odd, FG_Odd)
// array counts: 55 8B EC 8B 45 08 8B 80 <d32> 5D C2 04 00
#define COUNT(n, d) NAKED static void n() { __asm { push ebp } __asm { mov ebp, esp } __asm { mov eax, [ebp + 8] } \
    __asm { mov eax, dword ptr [eax + d] } __asm { pop ebp } __asm { ret 4 } }
COUNT(FC_E4, 0xE4)
COUNT(FC_FC, 0xFC)
COUNT(FC_108, 0x108)
COUNT(FC_168, 0x168)
// struct array reserve: 55 8B EC 8B 45 08 FF 75 0C 05 <d32> 50 E8 <callee: shl ecx, 3>
NAKED static void FR_Inner() { __asm { mov ecx, [esp + 8] } __asm { shl ecx, 3 } __asm { ret } }
NAKED static void FR_164()
{
    __asm { push ebp } __asm { mov ebp, esp } __asm { mov eax, [ebp + 8] } __asm { push dword ptr [ebp + 0xC] }
    __asm { add eax, 0x164 } __asm { push eax } __asm { call FR_Inner } __asm { pop ecx } __asm { pop ecx }
    __asm { pop ebp } __asm { ret 8 }
}
// the object's vt[0x38] getPos (copy-out of +0x90, like 0x008EF6B2)
NAKED void Refl_FakeGetPos()
{
    __asm { push ebp } __asm { mov ebp, esp } __asm { mov eax, [ebp + 8] } __asm { push esi } __asm { push edi }
    __asm { lea esi, [ecx + 0x90] } __asm { mov edi, eax } __asm { movsd } __asm { movsd } __asm { movsd }
    __asm { movsd } __asm { pop edi } __asm { pop esi } __asm { pop ebp } __asm { ret 4 }
}

enum { FP_NAME, FP_ONESHOT, FP_ENABLE, FP_REQWORLD, FP_FLAG140, FP_OBJECTID, FP_POSITION, FP_FILTER, FP_NAMEFILTER,
       FP_ONENTER, FP_WAYPOINT, FP_MAXDIST, FP_LOADONENTER, FP_ODD,
       FP_WORLDFILE, FP_REGION, FP_STARTPOINT, FP_ID, FP_COUNT };  // task 14: WorldLoadModule, module ID
static const struct { const char *name; void (*type)(); void (*elem)(); void (*holder)(); void (*count)();
                      void (*reserve)(); } kFakeProps[FP_COUNT] = {
    {"Name", FT_Name, FT_Zero, FH_Name50, NULL, NULL},
    {"OneShot", FT_Bool, FT_Zero, FH_Byte11C, NULL, NULL},
    {"Enable", FT_Bool, FT_Zero, FH_Byte11D, NULL, NULL},
    {"RequireInWorld", FT_Bool, FT_Zero, FH_Byte11F, NULL, NULL},
    {"IncludeGrabbables", FT_Bool, FT_Zero, FH_Bit140, NULL, NULL},
    {"ObjectID", FT_Int, FT_Zero, FH_Int14, NULL, NULL},
    {"Position", FT_Vec, FT_Zero, FH_VirtPos, NULL, NULL},
    {"FilterList", FT_Array, FT_Name, NULL, FC_E4, NULL},
    {"NameFilter", FT_Array, FT_Name, NULL, FC_FC, NULL},
    {"OnEnterList", FT_Array, FT_Name, NULL, FC_108, NULL},
    {"Waypoint", FT_Name, FT_Zero, FH_Name130, NULL, NULL},
    {"MaxDistance", FT_Float, FT_Zero, FH_Float150, NULL, NULL},
    {"LoadOnEnter", FT_Array, FT_Vec, NULL, FC_168, FR_164},
    {"Mystery", FT_Int, FT_Zero, FH_Odd, NULL, NULL},
    {"WorldFile", FT_Name, FT_Zero, FH_Name48, NULL, NULL},
    {"Region", FT_Name, FT_Zero, FH_Name50, NULL, NULL},
    {"StartPoint", FT_Name, FT_Zero, FH_Name58, NULL, NULL},
    {"ID", FT_Int, FT_Zero, FH_Int14, NULL, NULL},
};
static void *s_fpVt[FP_COUNT][0x70 / 4];
static __declspec(align(8)) BYTE s_fp[FP_COUNT][0x18];
static void *s_fcVt[0x40 / 4];

// a fake class descriptor (0x30 bytes): its properties are the FP_* listed (array kept by the caller)
void Refl_FakeClass(void *cls, const char *name, void *parent, void **props, int numProps)
{
    BYTE *c = (BYTE *)cls;
    for (int i = 0; i < 0x40 / 4; i++) s_fcVt[i] = (void *)(UINT_PTR)(0x7E100000 + i);
    s_fcVt[8 / 4] = (void *)(UINT_PTR)PC_CLS_GETNAME;
    s_fcVt[0x10 / 4] = (void *)(UINT_PTR)PC_CLS_ISKINDOF;
    *(void **)c = s_fcVt;
    *(void **)(c + OFS_CLS_PARENT) = parent;
    Refl_FakeName(c + OFS_CLS_NAME, name);
    *(void ***)(c + OFS_CLS_PROPS) = props;
    *(int *)(c + OFS_CLS_PROPS + 4) = numProps;
}

// the fake property FP_x (index into kFakeProps); names: Name OneShot Enable RequireInWorld IncludeGrabbables
// ObjectID Position FilterList NameFilter OnEnterList Waypoint MaxDistance LoadOnEnter Mystery
// WorldFile (+0x48) Region (+0x50) StartPoint (+0x58) ID (+0x14)
void *Refl_FakeProp(int i)
{
    if (i < 0 || i >= FP_COUNT) return NULL;
    void **vt = s_fpVt[i];
    for (int k = 0; k < 0x70 / 4; k++) vt[k] = (void *)FT_Zero;
    vt[PVT_TYPE / 4] = (void *)kFakeProps[i].type;
    vt[PVT_ELEM / 4] = (void *)kFakeProps[i].elem;
    if (kFakeProps[i].holder) vt[PVT_HOLDER / 4] = (void *)kFakeProps[i].holder;
    if (kFakeProps[i].count) vt[PVT_COUNT / 4] = (void *)kFakeProps[i].count;
    if (kFakeProps[i].reserve) vt[PVT_RESERVE / 4] = (void *)kFakeProps[i].reserve;
    *(void ***)s_fp[i] = vt;
    Refl_FakeName(s_fp[i] + OFS_PROP_NAME, kFakeProps[i].name);
    return s_fp[i];
}

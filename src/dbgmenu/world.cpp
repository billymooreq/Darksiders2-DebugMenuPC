// Task 13: the current map in the showCameraPosition panel (notes/triggers_pc.md §8): the loaded world, its
// regions (loaded / active, like the Xbox showRegions "Loaded Regions:" / "Active region: %s" 0x82435958) and the
// region of the player. Also region id -> name for the trigger info (LoadOnEnter).
//   world = loader game+0xBC (+0x10 == 4) -> +0xC (the same world the triggers walk). World (vt 0x012AAE30,
//     ctor 0x00A494CB): +8 Name - the world name (an empty Name 0x00B819B8 in the ctor, the dtor frees it with
//     0x0049717E) [ГИП-сильная]; +0x20 default region index; +0x28 {data, count +0x2C} region entries
//     (WorldRegionData: +8 ID (0x00A465D1 looks ids up there), reflection property Name; +0x18 the loaded region,
//     NULL = not loaded (0x00A4645C -> 0x0053775E; Xbox prints the loaded ones the same way)); +0x128 the active
//     region index (0x00A462FC: -1 -> +0x20), picked by the camera position every frame (0x004CECC1).
// Read in the overlay frame (game thread), refreshed twice a second; changes go to the log.
#include "common.h"
#include <stdio.h>
#include <string.h>
#include "imgui.h"

#define SPRINTF(buf, ...) _snprintf_s(buf, sizeof(buf), _TRUNCATE, __VA_ARGS__)
#define STRCPY(buf, src) strncpy_s(buf, sizeof(buf), src, _TRUNCATE)

#define OFS_GAME_LOADER    0xBC
#define OFS_LOADER_STATE   0x10
#define OFS_LOADER_WORLD   0x0C
#define OFS_WORLD_NAME     0x08
#define OFS_WORLD_DEFREG   0x20
#define OFS_WORLD_REGIONS  0x28
#define OFS_WORLD_ACTIVE   0x128
#define OFS_REG_ID         0x08
#define OFS_REG_LOADED     0x18

static const int kMaxRegions = 256;
struct Region { int id; bool loaded; char name[48]; };
static struct {
    void *world;
    char name[64];
    int numRegions, active, def, loaded;
    Region reg[kMaxRegions];
    bool ok;
    char playerRegion[96];
    DWORD tick;
} s_w;
static char s_lastName[64], s_lastLoaded[512];
static void *s_lastWorld;
static volatile LONG s_logBudget, s_exBudget;
static bool s_entryLogged;

static bool Fake() { return g_testMode && !g_gameMatch; }

static int CatchW(const char *where, EXCEPTION_POINTERS *ep)
{
    if (LogBudget(&s_exBudget, 20))
        Log("map: exception %08X at %08X in %s", ep->ExceptionRecord->ExceptionCode,
            (unsigned)(UINT_PTR)ep->ExceptionRecord->ExceptionAddress, where);
    return EXCEPTION_EXECUTE_HANDLER;
}

// ======================= offline test: a fake world =======================
static __declspec(align(8)) BYTE s_fWorld[0x140], s_fEntry[5][0x60], s_fLoadedReg[0x20], s_fRegCls[0x30];
static void *s_fEntries[5], *s_fRegProps[1];
static int s_fakeMap;  // 0 = test_world (regions 1..3 loaded, active 2), 1 = other_world after a "map change"
static const char *const kFakeRegions[5] = {"Global_World_Region", "Z2_Hub", "Z2_Forge", "Z2_Lostvale", "Z2_Spire"};
static void *__fastcall FakeEntryClass(void *self, void *) { return s_fRegCls; }
static void *s_fEntryVt[0x40 / 4];

static void FakeBuild()
{
    s_fRegProps[0] = Refl_FakeProp(0);  // Name (+0x50)
    Refl_FakeClass(s_fRegCls, "WorldRegionData", NULL, s_fRegProps, 1);
    for (int i = 0; i < 0x40 / 4; i++) s_fEntryVt[i] = (void *)(UINT_PTR)(0x7E200000 + i);
    s_fEntryVt[1] = (void *)FakeEntryClass;
    for (int i = 0; i < 5; i++) {
        BYTE *e = s_fEntry[i];
        memset(e, 0, sizeof(s_fEntry[i]));
        *(void **)e = s_fEntryVt;
        *(int *)(e + OFS_REG_ID) = i + 1;
        Refl_FakeName(e + 0x50, kFakeRegions[i]);
        bool loaded = s_fakeMap == 0 ? (i >= 1 && i <= 3) : (i == 0 || i == 4);
        *(void **)(e + OFS_REG_LOADED) = loaded ? s_fLoadedReg : NULL;
        s_fEntries[i] = e;
    }
    memset(s_fWorld, 0, sizeof(s_fWorld));
    Refl_FakeName(s_fWorld + OFS_WORLD_NAME, s_fakeMap == 0 ? "test_world" : "other_world");
    *(int *)(s_fWorld + OFS_WORLD_DEFREG) = 0;
    *(void ***)(s_fWorld + OFS_WORLD_REGIONS) = (void **)s_fEntries;
    *(int *)(s_fWorld + OFS_WORLD_REGIONS + 4) = 5;
    *(int *)(s_fWorld + OFS_WORLD_ACTIVE) = s_fakeMap == 0 ? 2 : -1;
}

void World_TestControl(int cmd)
{
    if (!Fake()) return;
    s_fakeMap = cmd == 27 ? 1 : 0;
    s_w.tick = 0;  // read again now
    Log("test: map: fake world %s", s_fakeMap ? "other_world (map change)" : "test_world");
}

// ======================= read =======================
static void *CurrentWorld()
{
    if (Fake()) {
        FakeBuild();
        return s_fWorld;
    }
    void *game = NULL;
    if (!Game_GetGame(&game) || !game) return NULL;
    __try {
        BYTE *ld = *(BYTE **)((BYTE *)game + OFS_GAME_LOADER);
        if (!ld || *(int *)(ld + OFS_LOADER_STATE) != 4) return NULL;
        void *w = *(void **)(ld + OFS_LOADER_WORLD);
        return (w && InImage(*(void **)w)) ? w : NULL;
    } __except (CatchW("loader game+0xBC", GetExceptionInformation())) {
        return NULL;
    }
}

static void LogEntryOnce(void *e)
{
    if (s_entryLogged || !e) return;
    s_entryLogged = true;
    char chain[160];
    Refl_ClassChain(e, chain, sizeof(chain), 6);
    Log("map: region entry %08X class %s", (unsigned)(UINT_PTR)e, chain[0] ? chain : "(not a reflected object)");
}

static void ReadWorld(void *w)
{
    s_w.world = w;
    s_w.numRegions = s_w.loaded = 0;
    s_w.active = s_w.def = -1;
    s_w.name[0] = 0;
    STRCPY(s_w.name, Refl_NameText((BYTE *)w + OFS_WORLD_NAME));
    void **data;
    int count;
    __try {
        data = *(void ***)((BYTE *)w + OFS_WORLD_REGIONS);
        count = *(int *)((BYTE *)w + OFS_WORLD_REGIONS + 4);
        s_w.def = *(int *)((BYTE *)w + OFS_WORLD_DEFREG);
        s_w.active = *(int *)((BYTE *)w + OFS_WORLD_ACTIVE);
        if (s_w.active < 0) s_w.active = s_w.def;
    } __except (CatchW("world regions", GetExceptionInformation())) {
        return;
    }
    if (!data || count <= 0 || count > 4096) return;
    for (int i = 0; i < count && s_w.numRegions < kMaxRegions; i++) {
        Region &r = s_w.reg[s_w.numRegions];
        void *e;
        __try {
            e = data[i];
            if (!e) continue;
            r.id = *(int *)((BYTE *)e + OFS_REG_ID);
            r.loaded = *(void **)((BYTE *)e + OFS_REG_LOADED) != NULL;
        } __except (CatchW("region entry", GetExceptionInformation())) {
            break;
        }
        LogEntryOnce(e);
        if (!Refl_Get(e, "Name", r.name, sizeof(r.name)) || !r.name[0]) SPRINTF(r.name, "region %d", r.id);
        if (r.loaded) s_w.loaded++;
        s_w.numRegions++;
    }
}

// the player's RegionID / LayerID (reflection of WorldObject), if the class has them
static void ReadPlayer()
{
    s_w.playerRegion[0] = 0;
    if (Fake()) return;
    void *game = NULL, *actor = NULL;
    if (!Game_GetGame(&game) || !Game_GetActor(game, &actor) || !actor) return;
    char reg[32], lay[32];
    bool r = Refl_Get(actor, "RegionID", reg, sizeof(reg)), l = Refl_Get(actor, "LayerID", lay, sizeof(lay));
    if (!r && !l) return;
    const char *rn = r ? World_RegionName(atoi(reg)) : NULL;
    SPRINTF(s_w.playerRegion, "player object: region %s%s%s%s layer %s", r ? reg : "?", rn ? " (" : "", rn ? rn : "",
            rn ? ")" : "", l ? lay : "?");
}

static void LoadedList(char *buf, int size, int maxNames)
{
    buf[0] = 0;
    int n = 0;
    for (int i = 0; i < s_w.numRegions; i++) {
        if (!s_w.reg[i].loaded) continue;
        if (n == maxNames) {
            char more[24];
            SPRINTF(more, ", ... +%d", s_w.loaded - n);
            strncat_s(buf, size, more, _TRUNCATE);
            break;
        }
        if (n) strncat_s(buf, size, ", ", _TRUNCATE);
        strncat_s(buf, size, s_w.reg[i].name, _TRUNCATE);
        n++;
    }
    if (!n) strncat_s(buf, size, "none", _TRUNCATE);
}

static void LogChanges()
{
    char all[512];
    LoadedList(all, sizeof(all), 40);
    if (s_w.world != s_lastWorld || strcmp(s_w.name, s_lastName)) {
        if (LogBudget(&s_logBudget, 200))
            Log("map: %s (world %08X, %d regions, active %s, default %d) | loaded %d: %s", s_w.name[0] ? s_w.name : "(no name)",
                (unsigned)(UINT_PTR)s_w.world, s_w.numRegions,
                s_w.active >= 0 && s_w.active < s_w.numRegions ? s_w.reg[s_w.active].name : "-", s_w.def, s_w.loaded, all);
        s_lastWorld = s_w.world;
        STRCPY(s_lastName, s_w.name);
        STRCPY(s_lastLoaded, all);
        return;
    }
    if (strcmp(all, s_lastLoaded)) {
        if (LogBudget(&s_logBudget, 200)) Log("map: loaded regions now %d: %s", s_w.loaded, all);
        STRCPY(s_lastLoaded, all);
    }
}

static void Refresh()
{
    if (s_w.tick && GetTickCount() - s_w.tick < 500) return;
    s_w.tick = GetTickCount() | 1;
    void *w = CurrentWorld();
    s_w.ok = w != NULL;
    if (!w) {
        s_w.world = NULL;
        if (s_lastWorld && LogBudget(&s_logBudget, 200)) Log("map: no world (loading)");
        s_lastWorld = NULL;
        s_lastName[0] = 0;
        return;
    }
    __try {
        ReadWorld(w);
        ReadPlayer();
    } __except (CatchW("world", GetExceptionInformation())) {
        s_w.ok = false;
        return;
    }
    LogChanges();
}

// region id -> name (NULL = unknown); uses the last reading
const char *World_RegionName(int id)
{
    for (int i = 0; i < s_w.numRegions; i++)
        if (s_w.reg[i].id == id) return s_w.reg[i].name;
    return NULL;
}

void World_Refresh() { Refresh(); }

// the map lines of the showCameraPosition panel
void World_Panel()
{
    Refresh();
    ImGui::Separator();
    if (!s_w.ok) {
        ImGui::TextDisabled("map    - (no world: loading)");
        return;
    }
    ImGui::Text("map    %s", s_w.name[0] ? s_w.name : "(no name)");
    const char *act = s_w.active >= 0 && s_w.active < s_w.numRegions ? s_w.reg[s_w.active].name : "-";
    ImGui::Text("region %s (active)", act);
    char list[512];
    LoadedList(list, sizeof(list), 6);
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f);
    ImGui::TextDisabled("loaded %d / %d: %s", s_w.loaded, s_w.numRegions, list);
    if (s_w.playerRegion[0]) ImGui::TextDisabled("%s", s_w.playerRegion);
    ImGui::PopTextWrapPos();
}

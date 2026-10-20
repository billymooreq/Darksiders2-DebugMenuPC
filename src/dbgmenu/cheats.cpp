// Cheats (task 15, notes/cheats_pc.md): the Xbox playerInvincible / killMonsters and a one-hit kill.
//   Xbox playerInvincible (General, ctor 0x825B32B0, toggle of the bool 0x837B42FD, config Game/PlayerInvincible): read
//     by Player::applyDamage 0x827CC610 (no HP taken) and Character::updateStats 0x828B4C70 (no negative regen). The
//     PC exe kept both checks and the flag: byte 0x01562FED (right after the Player class global 0x01562FE8, like the
//     Xbox 0x837B42F8 / FD), the config var 0x014D4010 points at it (+0x14 = 0x014D4024), read at 0x00722362
//     (Player::applyDamage 0x0072223C) and 0x00790318 (Character::updateStats 0x007902A4). We only set the byte.
//   Xbox killMonsters (AI, ctor 0x825BE180, execute 0x825BEB28 -> 0x825BE1F0 per monster): a damage event of type 0x1F,
//     100000 through the normal damage path, then the script event "killAllMonsters" to a monster that did not die.
//     The PC KillRegion (0x007E5D41) does exactly that for monsters: event ctor 0x0086AC0A, init 0x0086AE68, dispatch
//     0x0086BCF4, dtor 0x0062B97A, then sendEvent 0x004CEC21(obj, "killAllMonsters"). We build the same event with
//     the player as the attacker (Monster::applyDamage sets GivesXP +0xB84 bit 0x800 only for the player's damage).
//   One-hit kill (not on the Xbox; its playerIsUber 0x01562FEC gives "at least 20 % of the HP", 0x007225DB): hook of
//     Monster::applyDamage 0x007B310F (Monster vt[0x16C], every monster class) - a hit of the player (or of a monster
//     on his side) on an enemy gets the damage = the enemy's HP, so the game's own code kills it. Bosses in their fight
//     (registerBoss: +0xB84 bit 0x400) and the game's "keep alive" (the filter 0x00787F64 leaves HP 1) are left alone.
#include "common.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <float.h>

bool g_cheatInvincible, g_cheatOneHit, g_cheatHookOk;

#define PC_INVINCIBLE   0x01562FEDu  // byte, PlayerInvincible (Xbox 0x837B42FD)
#define PC_INV_PTR      0x014D4024u  // config var Game/PlayerInvincible 0x014D4010 +0x14: -> the byte
#define PC_IS_ENEMY     0x008D7985u  // bool __cdecl isEnemy(Character *, Character *) (script isEnemy 0x00769C59)
#define PC_EV_CTOR      0x0086AC0Au  // DamageEvent *__fastcall ctor(ev)
#define PC_EV_INIT      0x0086AE68u  // __thiscall init(ev, type, kind, reaction, amount, +0x28, +0x2C, attacker +0x14C,
                                     //   source +0x150, target +0x154, +0x18), ret 0x28
#define PC_EV_SEND      0x0086BCF4u  // __thiscall dispatch(ev): attacker vt[0x164], target vt[0x168] / vt[0x16C] ...
#define PC_EV_DTOR      0x0062B97Au  // __fastcall dtor(ev)
#define PC_NAME_CTOR    0x00497100u  // Name::Name(const char *, int), thiscall, ret 8
#define PC_SEND_EVENT   0x004CEC21u  // __cdecl sendEvent(obj, Name *) - the script event
#define PC_RELEASE      0x00401017u  // release(ECX = obj)
#define OFS_CHR_STATS   0x330        // stats: +0 HP (float)
#define OFS_MON_FLAGS   0xB84        // Monster bit field: 0x400 registerBoss (0x007B8E1D), 0x800 GivesXP
#define MONF_BOSS       0x400u
#define OFS_DMG_AMOUNT  0x20         // DamageEvent: +0x19 type, +0x1A kind, +0x1C reaction, +0x20 amount (float)
#define OFS_DMG_FLAGS   0x148        //   +0x148 bit 0: no damage (the filter zeroed it)
#define OFS_DMG_ATTACK  0x14C        //   +0x14C attacker (a character or NULL), +0x154 target
static const int kKillType = 0x1F, kKillKind = 0, kKillReaction = 100000;  // the Xbox killMonsters event
static const float kKillAmount = 100000.0f;
static const DWORD kSummaryMs = 10000;

typedef void *(__fastcall *EvCtor_t)(void *ev);
typedef void(__fastcall *EvInit_t)(void *ev, void *edx, int type, int kind, int reaction, float amount, float f28,
                                   float f2c, void *attacker, void *source, void *target, int b18);
typedef void(__fastcall *EvSend_t)(void *ev);
typedef void(__fastcall *EvDtor_t)(void *ev);
typedef void *(__fastcall *NameCtor_t)(void *name, void *edx, const char *s, int b);
typedef void(__cdecl *SendEvent_t)(void *obj, void *name);
typedef BYTE(__cdecl *IsEnemy_t)(void *a, void *b);
typedef void(__fastcall *Release_t)(void *obj);

static bool s_invOn;               // the byte is set by us
static BYTE s_invPrev;             // its value before
static bool s_invChecked, s_invOk, s_killChecked, s_killOk, s_enemyChecked, s_enemyOk;
static bool s_oneHitWas;
static volatile LONG s_killReq;
static volatile LONG s_exBudget, s_hitBudget, s_bossBudget, s_aliveBudget, s_notEnemyBudget, s_allyBudget, s_killBudget;
static LONG s_hits, s_hitsQuiet, s_lethal, s_bosses, s_keptAlive, s_notEnemy;  // one-hit counters since it went on
static DWORD s_summaryTick;
static __declspec(align(8)) BYTE s_kamName[8];  // Name "killAllMonsters" (built once, never freed - like the game's)
static bool s_kamNameOk;

static bool Fake() { return g_testMode && !g_gameMatch; }
static BYTE s_fakeInv;             // stands for 0x01562FED

#define SPRINTF(buf, ...) _snprintf_s(buf, sizeof(buf), _TRUNCATE, __VA_ARGS__)

static int CatchCheat(const char *where, EXCEPTION_POINTERS *ep)
{
    if (LogBudget(&s_exBudget, 40))
        Log("cheats: exception %08X at %08X in %s", ep->ExceptionRecord->ExceptionCode,
            (unsigned)(UINT_PTR)ep->ExceptionRecord->ExceptionAddress, where);
    return EXCEPTION_EXECUTE_HANDLER;
}

static bool CodeIs(UINT_PTR addr, const BYTE *b, size_t n, const char *what)
{
    bool ok = false;
    __try {
        ok = memcmp((void *)addr, b, n) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    if (!ok) Log("cheats: unexpected bytes at %08X (%s)", (unsigned)addr, what);
    return ok;
}

// ======================= the game (real or fake) =======================
static bool Hp(void *chr, float *hp)
{
    __try {
        BYTE *st = *(BYTE **)((BYTE *)chr + OFS_CHR_STATS);
        if (!st) return false;
        *hp = *(float *)st;
        return _finite(*hp) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void *Player()
{
    if (Fake()) return Mon_FakeObj(6);
    void *game = NULL, *actor = NULL;
    if (!Game_GetGame(&game) || !game || !Game_GetActor(game, &actor) || !actor) return NULL;
    return actor;
}

// test mode: the player (Death) and Behind_01 (a monster on his side: "a summoned creature") against the other monsters
static bool FakeIsEnemy(void *a, void *b)
{
    float ha, hb;
    if (!Hp(a, &ha) || !Hp(b, &hb) || ha <= 0 || hb <= 0 || a == b) return false;
    void *pl = Mon_FakeObj(6), *ally = Mon_FakeObj(4);
    int sa = (a == pl || a == ally) ? 1 : Mon_Kind(a) == MK_MONSTER ? 2 : 0;
    int sb = (b == pl || b == ally) ? 1 : Mon_Kind(b) == MK_MONSTER ? 2 : 0;
    return sa && sb && sa != sb;
}

static bool IsEnemy(void *a, void *b)
{
    if (Fake()) return FakeIsEnemy(a, b);
    if (!s_enemyChecked) {
        static const BYTE kPro[] = {0x6A, 0x00, 0xB8, 0x36, 0xEF, 0x05, 0x01, 0xE8};
        s_enemyChecked = true;
        s_enemyOk = CodeIs(PC_IS_ENEMY, kPro, sizeof(kPro), "isEnemy");
    }
    if (!s_enemyOk) return false;
    __try {
        return ((IsEnemy_t)(UINT_PTR)PC_IS_ENEMY)(a, b) != 0;
    } __except (CatchCheat("isEnemy 0x008D7985", GetExceptionInformation())) {
        return false;
    }
}

static BYTE *InvByte() { return Fake() ? &s_fakeInv : (BYTE *)(UINT_PTR)PC_INVINCIBLE; }

static bool InvCodeOk()
{
    if (Fake()) return true;
    if (s_invChecked) return s_invOk;
    s_invChecked = true;
    static const BYTE kRead[] = {0xA1, 0x24, 0x40, 0x4D, 0x01, 0x80, 0x38, 0x00};  // mov eax, [0x014D4024]; cmp byte [eax], 0
    s_invOk = CodeIs(0x00722362u, kRead, sizeof(kRead), "Player::applyDamage invincible check") &&
              CodeIs(0x00790318u, kRead, sizeof(kRead), "Character::updateStats invincible check");
    __try {
        UINT_PTR p = *(UINT_PTR *)PC_INV_PTR;
        BYTE v = *InvByte();
        if (p != PC_INVINCIBLE || v > 1) {
            Log("cheats: PlayerInvincible: pointer %08X (expected %08X), value %u -> invincible not available",
                (unsigned)p, PC_INVINCIBLE, v);
            s_invOk = false;
        }
    } __except (CatchCheat("PlayerInvincible check", GetExceptionInformation())) {
        s_invOk = false;
    }
    return s_invOk;
}

static bool KillCodeOk()
{
    if (Fake()) return true;
    if (s_killChecked) return s_killOk;
    s_killChecked = true;
    static const BYTE kCtor[] = {0x6A, 0x08, 0xB8, 0x88, 0x4E, 0x05, 0x01, 0xE8};
    static const BYTE kInit[] = {0x53, 0x8B, 0xDC, 0x51, 0x51, 0x83, 0xE4, 0xF0};
    static const BYTE kSend[] = {0x6A, 0x10, 0x68, 0xF8, 0x04, 0x00, 0x00, 0xB8};
    static const BYTE kDtor[] = {0x6A, 0x04, 0xB8, 0x7E, 0x78, 0x02, 0x01, 0xE8};
    static const BYTE kName[] = {0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x56};
    static const BYTE kSendEv[] = {0x55, 0x8B, 0xEC, 0x51, 0x53, 0x8B, 0x5D, 0x0C};
    s_killOk = CodeIs(PC_EV_CTOR, kCtor, sizeof(kCtor), "damage event ctor") &
               CodeIs(PC_EV_INIT, kInit, sizeof(kInit), "damage event init") &
               CodeIs(PC_EV_SEND, kSend, sizeof(kSend), "damage dispatch") &
               CodeIs(PC_EV_DTOR, kDtor, sizeof(kDtor), "damage event dtor") &
               CodeIs(PC_NAME_CTOR, kName, sizeof(kName), "Name ctor") &
               CodeIs(PC_SEND_EVENT, kSendEv, sizeof(kSendEv), "sendEvent");
    return s_killOk;
}

// ======================= invincible =======================
static void InvSet(bool on, const char *why)
{
    __try {
        BYTE *b = InvByte();
        BYTE before = *b;
        if (on) {
            s_invPrev = before;
            *b = 1;
            s_invOn = true;
        } else {
            *b = s_invPrev;
            s_invOn = false;
        }
        Log("cheats: invincible %s (%s): PlayerInvincible %08X %u -> %u", on ? "ON" : "OFF", why,
            (unsigned)(UINT_PTR)b, before, *b);
    } __except (CatchCheat("PlayerInvincible byte", GetExceptionInformation())) {
        s_invOn = false;
        g_cheatInvincible = false;
    }
}

// game thread: the byte follows the menu item (also after level loads: the game never writes it after the start)
static void ApplyInvincible(const char *why)
{
    if (g_cheatInvincible && !s_invOn) {
        if (!InvCodeOk()) {
            g_cheatInvincible = false;
            Menu_Status("invincible: not available (see the log)");
            return;
        }
        InvSet(true, why);
    } else if (!g_cheatInvincible && s_invOn) {
        InvSet(false, why);
    } else if (s_invOn) {
        __try {
            BYTE *b = InvByte();
            if (*b != 1) {
                static volatile LONG once;
                if (LogBudget(&once, 5)) Log("cheats: invincible: PlayerInvincible was %u again -> 1", *b);
                *b = 1;
            }
        } __except (CatchCheat("PlayerInvincible byte", GetExceptionInformation())) {
        }
    }
}

// ======================= one-hit kill (hook of Monster::applyDamage, game thread) =======================
static void LogOnce(volatile LONG *budget, LONG max, void *obj, const char *fmt)
{
    if (!LogBudget(budget, max)) return;
    char lb[96];
    Log(fmt, Mon_Label(obj, lb, sizeof(lb)));
}

void Cheats_MonsterDamage(void *mon, void *dmg, CheatHit *h)
{
    memset(h, 0, sizeof(*h));
    if (!g_cheatOneHit || !g_debugOn || !mon || !dmg) return;
    __try {
        BYTE *d = (BYTE *)dmg;
        float amount = *(float *)(d + OFS_DMG_AMOUNT), hp;
        if (!(amount > 0) || (d[OFS_DMG_FLAGS] & 1)) return;  // no damage, or the filter blocked it
        if (!Hp(mon, &hp) || !(hp > 0)) return;               // dead already: the game's own branch
        void *att = *(void **)(d + OFS_DMG_ATTACK);
        if (!att || att == mon) return;
        void *pl = Player();
        if (!pl || mon == pl) return;
        const char *by = NULL;
        int ak = att == pl ? MK_PLAYER : Mon_Kind(att);
        if (ak == MK_PLAYER) by = "the player";
        else if (ak != MK_NONE && !IsEnemy(pl, att)) {     // a character on the player's side
            if (ak == MK_MONSTER) by = "a summoned creature";
            else LogOnce(&s_allyBudget, 10, att, "cheats: one-hit: a hit of the ally %s (NPC) - not changed");
        }
        if (!by) return;
        if (!IsEnemy(pl, mon)) {
            s_notEnemy++;
            LogOnce(&s_notEnemyBudget, 10, mon, "cheats: one-hit: %s is not an enemy - normal damage");
            return;
        }
        if (*(DWORD *)((BYTE *)mon + OFS_MON_FLAGS) & MONF_BOSS) {
            s_bosses++;
            LogOnce(&s_bossBudget, 10, mon, "cheats: one-hit: %s is a boss in its fight (registerBoss) - normal damage");
            return;
        }
        if (amount < hp && fabsf(amount - (hp - 1.0f)) < 0.01f) {  // the filter 0x00787F64 keeps it alive (HP 1)
            s_keptAlive++;
            LogOnce(&s_aliveBudget, 10, mon, "cheats: one-hit: the game keeps %s alive (HP 1) - not changed");
            return;
        }
        if (amount >= hp) {
            s_lethal++;
            return;
        }
        h->boosted = true;
        h->hp = hp;
        h->dmg = amount;
        h->attacker = att;
        h->by = by;
        *(float *)(d + OFS_DMG_AMOUNT) = hp;  // Monster::applyDamage: damage >= HP -> HP 0 -> its death (vt[0x174])
    } __except (CatchCheat("one-hit kill", GetExceptionInformation())) {
        h->boosted = false;
    }
}

void Cheats_MonsterDamageDone(void *mon, const CheatHit *h)
{
    float after = -1;
    Hp(mon, &after);
    LONG n = InterlockedIncrement(&s_hits);
    if (!LogBudget(&s_hitBudget, 40)) {
        s_hitsQuiet++;
        return;
    }
    char lb[96], ab[96];
    if (h->attacker && strcmp(h->by, "the player") != 0)
        Log("cheats: one-hit #%ld %s: HP %.1f, hit %.1f -> %.1f (by %s %s) -> HP %.1f", n, Mon_Label(mon, lb, sizeof(lb)),
            h->hp, h->dmg, h->hp, h->by, Mon_Label(h->attacker, ab, sizeof(ab)), after);
    else
        Log("cheats: one-hit #%ld %s: HP %.1f, hit %.1f -> %.1f (by %s) -> HP %.1f", n, Mon_Label(mon, lb, sizeof(lb)),
            h->hp, h->dmg, h->hp, h->by, after);
}

static void OneHitSummary(bool force)
{
    DWORD now = GetTickCount();
    if (!force && now - s_summaryTick < kSummaryMs) return;
    s_summaryTick = now;
    if (!s_hitsQuiet && !force) return;
    Log("cheats: one-hit: %ld hits made lethal (%ld not logged one by one), %ld lethal anyway, %ld boss hits, %ld kept "
        "alive by the game, %ld on non-enemies", s_hits, s_hitsQuiet, s_lethal, s_bosses, s_keptAlive, s_notEnemy);
    s_hitsQuiet = 0;
}

static void OneHitFollow(const char *why)
{
    if (g_cheatOneHit && !g_cheatHookOk && !Fake()) {
        g_cheatOneHit = false;
        Log("cheats: one-hit kill refused: no hook of Monster::applyDamage 0x007B310F");
        Menu_Status("one-hit kill: not available (see the log)");
    }
    if (g_cheatOneHit == s_oneHitWas) return;
    s_oneHitWas = g_cheatOneHit;
    if (g_cheatOneHit) {
        s_hits = s_hitsQuiet = s_lethal = s_bosses = s_keptAlive = s_notEnemy = 0;
        s_hitBudget = 0;
        s_summaryTick = GetTickCount();
        Log("cheats: one-hit kill ON (%s)", why);
    } else {
        Log("cheats: one-hit kill OFF (%s)", why);
        OneHitSummary(true);
    }
}

// ======================= kill all monsters (before a game frame) =======================
// test mode: what the game's dispatch does for this event (the boss Skel_Far is kept alive by "its fight" like the
// filter 0x00787F64 does) and Monster::applyDamage (HP down, GivesXP for the player) with our hook around it
static void FakeHit(void *att, void *mon, float amount)
{
    __declspec(align(16)) BYTE ev[0x160];
    memset(ev, 0, sizeof(ev));
    float hp = 0;
    if (!Hp(mon, &hp)) return;
    if (mon == Mon_FakeObj(3) && amount > hp) amount = hp - 1;  // the boss: its fight keeps it alive (HP 1)
    *(float *)(ev + OFS_DMG_AMOUNT) = amount;
    *(void **)(ev + OFS_DMG_ATTACK) = att;
    *(void **)(ev + 0x154) = mon;
    CheatHit h;
    Cheats_MonsterDamage(mon, ev, &h);
    float a = *(float *)(ev + OFS_DMG_AMOUNT);
    if (hp > 0) {
        float *p = *(float **)((BYTE *)mon + OFS_CHR_STATS);
        *p = a >= hp ? 0 : hp - a;
        if (att == Mon_FakeObj(6)) *(DWORD *)((BYTE *)mon + OFS_MON_FLAGS) |= 0x800;
    }
    if (h.boosted) Cheats_MonsterDamageDone(mon, &h);
}

// one monster like the Xbox 0x825BE1F0: the lethal event, then "killAllMonsters" if it is still alive; false = exception
static bool KillOne(void *mon, void *pl, bool *eventSent)
{
    *eventSent = false;
    if (Fake()) {
        FakeHit(pl, mon, kKillAmount);
        float hp = 0;
        if (Hp(mon, &hp) && hp > 0) {
            *eventSent = true;
            char lb[96];
            Log("test: (fake) script event killAllMonsters -> %s: the fake script does nothing", Mon_Label(mon, lb, sizeof(lb)));
        }
        return true;
    }
    __declspec(align(16)) BYTE ev[0x400];  // the game's own events live on the stack (KillRegion: 0x1B0 bytes)
    const char *stage = "kill all: damage event";
    __try {
        ((EvCtor_t)(UINT_PTR)PC_EV_CTOR)(ev);
        ((EvInit_t)(UINT_PTR)PC_EV_INIT)(ev, NULL, kKillType, kKillKind, kKillReaction, kKillAmount, 0.0f, 0.0f, pl, NULL,
                                          mon, 0);
        InterlockedIncrement((LONG *)(ev + 4));  // like the KillRegion: the dispatch may take and drop references
        ((EvSend_t)(UINT_PTR)PC_EV_SEND)(ev);
        InterlockedDecrement((LONG *)(ev + 4));
        ((EvDtor_t)(UINT_PTR)PC_EV_DTOR)(ev);
        stage = "kill all: script event killAllMonsters";
        float hp = 0;
        if (Hp(mon, &hp) && hp > 0) {
            if (!s_kamNameOk) {
                ((NameCtor_t)(UINT_PTR)PC_NAME_CTOR)(s_kamName, NULL, "killAllMonsters", 0);
                s_kamNameOk = true;
            }
            ((SendEvent_t)(UINT_PTR)PC_SEND_EVENT)(mon, s_kamName);
            *eventSent = true;
        }
    } __except (CatchCheat(stage, GetExceptionInformation())) {
        return false;
    }
    return true;
}

static void Release(void *obj)
{
    if (Fake()) {
        InterlockedDecrement((LONG *)((BYTE *)obj + 4));
        return;
    }
    __try {
        ((Release_t)(UINT_PTR)PC_RELEASE)(obj);
    } __except (CatchCheat("kill all: release", GetExceptionInformation())) {
    }
}

static void KillAll()
{
    double t0 = (double)GetTickCount();
    void *pl = Player();
    float php = 0;
    if (!pl || !Hp(pl, &php)) {
        Log("cheats: kill all monsters: no player now (loading?)");
        Menu_Status("kill all monsters: no player now (loading?)");
        return;
    }
    if (php <= 0) {
        Log("cheats: kill all monsters: the player is dead (HP %.1f)", php);
        Menu_Status("kill all monsters: the player is dead");
        return;
    }
    if (!KillCodeOk()) {
        Menu_Status("kill all monsters: not available (see the log)");
        return;
    }
    static void *chars[2048], *mons[2048];
    int nc = Mon_Characters(chars, 2048), nm = 0, monsters = 0, dead = 0, allies = 0;
    for (int i = 0; i < nc; i++) {
        if (chars[i] == pl || Mon_Kind(chars[i]) != MK_MONSTER) continue;
        monsters++;
        float hp = 0;
        if (!Hp(chars[i], &hp) || hp <= 0) {
            dead++;
            continue;
        }
        if (!IsEnemy(pl, chars[i])) {
            allies++;
            continue;
        }
        InterlockedIncrement((LONG *)((BYTE *)chars[i] + 4));  // kept alive while we go through the list
        mons[nm++] = chars[i];
    }
    int enemies = nm, killed = 0, spawners = 0, survived = 0, events = 0, bosses = 0, byEvent = 0, failed = 0;
    for (int i = 0; i < nm; i++) {
        void *m = mons[i];
        float hp0 = 0, hp1 = 0;
        Hp(m, &hp0);
        bool boss = false, sent = false;
        __try {
            boss = (*(DWORD *)((BYTE *)m + OFS_MON_FLAGS) & MONF_BOSS) != 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        bosses += boss;
        bool ok = KillOne(m, pl, &sent);
        events += sent;
        bool alive = Hp(m, &hp1) && hp1 > 0;
        char lb[96];
        if (!ok) {
            failed++;
            Log("cheats: kill all: stopped at %s (exception)", Mon_Label(m, lb, sizeof(lb)));
            for (int j = i; j < nm; j++) Release(mons[j]);
            nm = i;
            break;
        }
        if (alive) survived++;
        else {
            killed++;
            byEvent += sent;
            if (Mon_ClassHas(m, "spawn")) spawners++;
        }
        if (LogBudget(&s_killBudget, 60))
            Log("cheats: kill %s: HP %.1f -> %.1f%s%s%s", Mon_Label(m, lb, sizeof(lb)), hp0, hp1, boss ? " (boss)" : "",
                sent ? " (script event killAllMonsters sent)" : "", alive ? " - SURVIVED" : "");
        Release(m);
    }
    s_killBudget = 0;
    Log("cheats: kill all monsters: characters %d, monsters %d (dead already %d, on the player's side %d), enemies %d -> "
        "killed %d (spawners %d, by the script event %d), survived %d, bosses %d, script events %d%s, %.0f ms", nc,
        monsters, dead, allies, enemies, killed, spawners, byEvent, survived, bosses, events,
        failed ? ", STOPPED by an exception" : "", (double)GetTickCount() - t0);
    char msg[128];
    if (failed) SPRINTF(msg, "kill all monsters: stopped (see the log); killed %d", killed);
    else if (!enemies) SPRINTF(msg, "kill all monsters: no living enemies in the loaded level");
    else if (survived) SPRINTF(msg, "killed %d monsters, %d spawners (%d survived)", killed, spawners, survived);
    else SPRINTF(msg, "killed %d monsters, %d spawners", killed, spawners);
    Menu_Status("%s", msg);
}

// ======================= frame / menu / F10 =======================
void Cheats_Frame()
{
    if (!g_gameMatch && !g_testMode) return;
    ApplyInvincible("menu");
    OneHitFollow("menu");
    if (s_oneHitWas) OneHitSummary(false);
    if (InterlockedExchange(&s_killReq, 0) && g_debugOn) KillAll();
}

void Cheats_RequestKillAll()
{
    InterlockedExchange(&s_killReq, 1);
    Log("cheats: kill all monsters requested (next game frame)");
}

void Cheats_MenuChanged(bool invincible)
{
    if (invincible) {
        ApplyInvincible("menu");
        Menu_Status("invincible %s", g_cheatInvincible ? "on" : "off");
    } else {
        OneHitFollow("menu");
        Menu_Status("one-hit kill %s", g_cheatOneHit ? "on" : "off");
    }
}

void Cheats_OnDebugOff()
{
    g_cheatInvincible = g_cheatOneHit = false;
    InterlockedExchange(&s_killReq, 0);
    ApplyInvincible("debug off");
    OneHitFollow("debug off");
}

void Cheats_OnUnload()
{
    g_cheatInvincible = false;
    if (s_invOn) InvSet(false, "unload / window closed");
}

// ======================= offline test (DBGMENU_TEST=1) =======================
// fake characters of monsters.cpp: 0 Skel_A01 (HP 180), 1 Ghoul_07 (90), 2 Ghoul_08 (40), 3 Skel_Far (250, a boss in
// its fight), 4 Behind_01 (250, on the player's side: "a summoned creature"), 5 Vulgrim (NPC), 6 Death (the player)
void Cheats_TestControl(int cmd)
{
    if (!Fake()) return;
    void *pl = Mon_FakeObj(6);
    char lb[96];
    switch (cmd) {
    case 32:  // the player hits Skel_A01 for 20
        *(DWORD *)((BYTE *)Mon_FakeObj(3) + OFS_MON_FLAGS) |= MONF_BOSS;  // registerBoss
        Log("test: the player hits Skel_A01 for 20");
        FakeHit(pl, Mon_FakeObj(0), 20);
        break;
    case 33:  // the summoned creature hits Ghoul_07; the player hits it (not an enemy)
        Log("test: Behind_01 (the player's side) hits Ghoul_07 for 5; the player hits Behind_01 for 5");
        FakeHit(Mon_FakeObj(4), Mon_FakeObj(1), 5);
        FakeHit(pl, Mon_FakeObj(4), 5);
        break;
    case 34:  // the boss and the game's keep-alive, an enemy hitting an enemy
        Log("test: the player hits the boss Skel_Far for 30; Ghoul_08 hits Skel_A01 for 3");
        FakeHit(pl, Mon_FakeObj(3), 30);
        FakeHit(Mon_FakeObj(2), Mon_FakeObj(0), 3);
        break;
    case 35: {  // a keep-alive hit (the filter leaves HP 1): not changed
        float hp = 0;
        Hp(Mon_FakeObj(2), &hp);
        Log("test: the game's keep-alive: the player hits Ghoul_08 for HP - 1 = %.1f", hp - 1);
        __declspec(align(16)) BYTE ev[0x160];
        memset(ev, 0, sizeof(ev));
        *(float *)(ev + OFS_DMG_AMOUNT) = hp - 1;
        *(void **)(ev + OFS_DMG_ATTACK) = pl;
        CheatHit h;
        Cheats_MonsterDamage(Mon_FakeObj(2), ev, &h);
        Log("test: keep-alive hit: boosted %d, damage %.1f", h.boosted, *(float *)(ev + OFS_DMG_AMOUNT));
        break;
    }
    case 36: Mon_FakeHeal(); Log("test: every fake character has its start HP again"); break;
    case 37:  // a hit right now, whatever the menu says (one-hit off: normal)
        Log("test: the player hits Ghoul_07 for 10");
        FakeHit(pl, Mon_FakeObj(1), 10);
        break;
    case 38: Cheats_RequestKillAll(); break;  // like the menu item, outside the menu
    case 39: {
        char line[400] = {0};
        int n = 0;
        for (int i = 0; i < 7; i++) {
            float hp = -1;
            Hp(Mon_FakeObj(i), &hp);
            n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, "%s%s %.0f%s", i ? ", " : "",
                             Mon_Label(Mon_FakeObj(i), lb, sizeof(lb)), hp,
                             (*(DWORD *)((BYTE *)Mon_FakeObj(i) + OFS_MON_FLAGS) & 0x800) ? " xp" : "");
        }
        Log("test: cheats check: invincible %d (byte %u, ours %d), one-hit %d, debug %ld | HP: %s", g_cheatInvincible,
            s_fakeInv, s_invOn, g_cheatOneHit, g_debugOn, line);
        break;
    }
    }
}

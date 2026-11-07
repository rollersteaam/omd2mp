// devcheat.c - see devcheat.h. Everything here runs on the game's main thread (from
// devcon.c's per-frame hook), inside its __try/__except.
//
// Game facts (OrcsMustDie2.exe, stamp 0x51DD91A9), found from the script bindings
// Player_GetCash / Player_GetHealth and their callees:
//   character entity  +0x310 health component, +0x318 team component,
//                     +0x39C flags (bit 0 = player character), +0x39E net id (owner slot << 10)
//   health component  vtable 0x8688E8 (only class), +0x40 owner entity, +0x4C alive,
//                     (+0x4C is ALIVE: set to 1 at 0x49ACF1, cleared at death 0x49B849;
//                     the death handler 0x49C04D returns early when it is 0)
//                     +0x50 team, +0x54 health, +0x58 max health, +0x7C cash
//   SetHealth  0x49AE30  void __stdcall (comp, float hp, attacker, 0x8EA8BC, -1, flag)
//              clamps to [0, max]; at 0 it runs the death handler 0x49C000. The game's
//              own out-of-bounds kill (0x4E02F7) calls it as (comp, 0, 0, 0x8EA8BC, -1, 0).
//   AddCash    0x49B510  eax = health component, edi = amount; fires the GoldChanged UI event
//   local player       [0xBA4458], else [0xBA4454]
//   all entities       vision90.dll VisElementManager_cl<VisBaseEntity_cl*>: elementTable
//                      (+8 = data) and g_iElementCount, walked the same way Vision does.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "devcheat.h"
#include "devproto.h"

#define SET_HEALTH       0x0049AE30u
#define ADD_CASH         0x0049B510u
#define HEALTH_VTABLE    0x008688E8u
#define DAMAGE_DEFAULT   0x008EA8BCu
#define LOCAL_PLAYER_A   0x00BA4458u
#define LOCAL_PLAYER_B   0x00BA4454u
#define SESSION_GLOBAL   0x00B9D108u
#define SESSION_LOCAL    0x6C

#define ENT_HEALTH       0x310
#define ENT_FLAGS        0x39C
#define ENT_NET_ID       0x39E
#define ENT_MIN_SIZE     0x3A0
#define ENT_STATS        0x2D4   // SetHealth reads [[owner+0x2D4]+0x34] first
#define HC_OWNER         0x40
#define HC_ALIVE         0x4C
#define HC_TEAM          0x50
#define HC_HEALTH        0x54
#define HC_CASH          0x7C
#define HC_MIN_SIZE      0x80

#define MAX_VICTIMS      4096

static const uint8_t SET_HEALTH_PROLOGUE[7] = { 0x51, 0x53, 0x8B, 0x5C, 0x24, 0x0C, 0x55 };
static const uint8_t ADD_CASH_PROLOGUE[8] = { 0x56, 0x8B, 0xF0, 0x8B, 0x4E, 0x7C, 0x03, 0xCF };

typedef void(__stdcall *SetHealthFn)(void *comp, float hp, void *attacker, void *dmg, int a5, int a6);

static SetHealthFn g_set_health = (SetHealthFn)SET_HEALTH;   // trampoline once hooked
static int g_hooked;
static volatile LONG g_god;

static int readable(const void *p, size_t n) {
    MEMORY_BASIC_INFORMATION mi;
    const uint8_t *a = p, *end = a + n;
    if (!p) return 0;
    while (a < end) {
        if (!VirtualQuery(a, &mi, sizeof mi) || mi.State != MEM_COMMIT) return 0;
        if (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
        a = (const uint8_t *)mi.BaseAddress + mi.RegionSize;
    }
    return 1;
}

// The entity's health component, or NULL unless it is exactly what we expect.
static uint8_t *health_of(uint8_t *ent) {
    if (!readable(ent, ENT_MIN_SIZE)) return NULL;
    uint8_t *c = *(uint8_t **)(ent + ENT_HEALTH);
    if (!readable(c, HC_MIN_SIZE)) return NULL;
    if (*(uint32_t *)c != HEALTH_VTABLE || *(uint8_t **)(c + HC_OWNER) != ent) return NULL;
    return c;
}

static uint8_t *local_player(void) {
    uint8_t *p = *(uint8_t **)LOCAL_PLAYER_A;
    return p ? p : *(uint8_t **)LOCAL_PLAYER_B;
}

static int set_health_ok(void) {
    return g_hooked || !memcmp((void *)SET_HEALTH, SET_HEALTH_PROLOGUE, sizeof SET_HEALTH_PROLOGUE);
}

// ------------------------------------------------------------------ gold
const char *devcheat_gold(int amount, int *cash_after) {
    if (memcmp((void *)ADD_CASH, ADD_CASH_PROLOGUE, sizeof ADD_CASH_PROLOGUE))
        return "AddCash (0x49b510) does not match this game build";
    uint8_t *c = health_of(local_player());
    if (!c) return "no local player yet: load into a level first";
    void *fn = (void *)ADD_CASH;
    __asm {
        mov eax, c
        mov edi, amount
        call fn
    }
    *cash_after = *(int *)(c + HC_CASH);
    return NULL;
}

// ------------------------------------------------------------------ kill all
static int vision_entities(uint8_t ***data, unsigned *count) {
    static uint8_t *table;
    static unsigned *n;
    if (!table) {
        HMODULE v = GetModuleHandleA("vision90.dll");
        if (!v) return 0;
        table = (uint8_t *)GetProcAddress(v, "?elementTable@?$VisElementManager_cl@PAVVisBaseEntity_cl@@@@0V?$DynArray_cl@PAVVisBaseEntity_cl@@@@A");
        n = (unsigned *)GetProcAddress(v, "?g_iElementCount@?$VisElementManager_cl@PAVVisBaseEntity_cl@@@@0IA");
        if (!table || !n) { table = NULL; return 0; }
    }
    unsigned cap = *(unsigned *)(table + 4);
    *data = *(uint8_t ***)(table + 8);
    *count = *n < cap ? *n : cap;
    return *count == 0 || readable(*data, *count * sizeof **data);
}

const char *devcheat_killall(DevKillStats *st) {
    static uint8_t *victims[MAX_VICTIMS];
    uint8_t **ents;
    unsigned count;
    int nv = 0;
    memset(st, 0, sizeof *st);
    if (!set_health_ok()) return "SetHealth (0x49ae30) does not match this game build";
    uint8_t *me = health_of(local_player());
    if (!me) return "no local player yet: load into a level first";
    if (!vision_entities(&ents, &count)) return "vision90.dll entity table not found";
    uint8_t *session = *(uint8_t **)SESSION_GLOBAL;
    DevKillCand k;
    k.player_team = *(int *)(me + HC_TEAM);
    k.local_slot = session ? *(uint8_t *)(session + SESSION_LOCAL) : 0;
    for (unsigned i = 0; i < count; i++) {
        uint8_t *e = ents[i], *c;
        if (!e) continue;
        st->entities++;
        if (!(c = health_of(e))) continue;
        st->characters++;
        k.is_player = e[ENT_FLAGS] & 1;
        k.dead = c[HC_ALIVE] == 0;    // also covers half-spawned entities
        k.health = *(float *)(c + HC_HEALTH);
        k.team = *(int *)(c + HC_TEAM);
        k.net_id = *(uint16_t *)(e + ENT_NET_ID);
        int v = dev_kill_verdict(&k);
        if (v != DK_KILL) st->counts[v]++;
        else if (nv < MAX_VICTIMS) victims[nv++] = c;
        else st->overflow++;
    }
    // Kill after the scan: a death may change the entity table.
    for (int i = 0; i < nv; i++) {
        uint8_t *c = victims[i];
        uint8_t *e = *(uint8_t **)(c + HC_OWNER), *stats;
        if (*(uint32_t *)c != HEALTH_VTABLE || !c[HC_ALIVE] || !readable(e, ENT_MIN_SIZE)) continue;
        stats = *(uint8_t **)(e + ENT_STATS);
        if (!readable(stats, 0x38)) { st->faults++; continue; }
        __try {
            g_set_health(c, 0.0f, NULL, (void *)DAMAGE_DEFAULT, -1, 0);
            st->counts[DK_KILL]++;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            st->faults++;                   // one bad entity must not stop the rest
        }
    }
    return NULL;
}

// ------------------------------------------------------------------ god mode
static void __stdcall set_health_hook(uint8_t *comp, float hp, void *attacker, void *dmg, int a5, int a6) {
    if (g_god && comp) {
        uint8_t *e = *(uint8_t **)(comp + HC_OWNER);
        if (e) hp = dev_god_health(1, e[ENT_FLAGS] & 1, *(float *)(comp + HC_HEALTH), hp);
    }
    g_set_health(comp, hp, attacker, dmg, a5, a6);
}

// jmp set_health_hook at 0x49AE30; the trampoline runs the 7 stolen bytes (no relative
// operands in them) and jumps back to 0x49AE37.
static int install_hook(void) {
    uint8_t *t = VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return 0;
    memcpy(t, SET_HEALTH_PROLOGUE, 7);
    t[7] = 0xE9;
    *(int32_t *)(t + 8) = (int32_t)(SET_HEALTH + 7 - ((uint32_t)(uintptr_t)t + 12));
    uint8_t site[7] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90 };
    *(int32_t *)(site + 1) = (int32_t)((uint32_t)(uintptr_t)set_health_hook - (SET_HEALTH + 5));
    DWORD old;
    if (!VirtualProtect((void *)SET_HEALTH, sizeof site, PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(t, 0, MEM_RELEASE);
        return 0;
    }
    g_set_health = (SetHealthFn)t;          // before the jump goes live
    memcpy((void *)SET_HEALTH, site, sizeof site);
    VirtualProtect((void *)SET_HEALTH, sizeof site, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)SET_HEALTH, sizeof site);
    g_hooked = 1;
    return 1;
}

const char *devcheat_god(int on) {
    if (on && !g_hooked) {
        if (!set_health_ok()) return "SetHealth (0x49ae30) does not match this game build";
        if (!install_hook()) return "could not hook SetHealth";
    }
    InterlockedExchange(&g_god, on != 0);
    return NULL;
}

int devcheat_god_enabled(void) { return g_god != 0; }

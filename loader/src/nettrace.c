// nettrace.c - see nettrace.h.
//
// Must be installed after mock_start: mock_start refuses to hook a vtable entry that is not
// the game's original, while this module saves and calls whatever entry it finds.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "nettrace.h"

#define RAKPEER_GLOBAL   0x00B9D0B8u
#define IS_HOST_GLOBAL   0x00B9D0ADu
#define SESSION_GLOBAL   0x00B9D108u
#define SLOT_STATUS_GLOBAL 0x00B9D10Cu  // "invalid slot" sentinel / session status
#define ACCEPT_GLOBAL    0x00BBB99Cu    // [ptr]+8 != 0: Session::OnConnect (0x4B23F0) accepts
#define RAKPEER_VTABLE   0x0083375Cu

#define V_NUMCONN   9
#define V_SEND_BS   19
#define V_SEND_RAW  20
#define V_RECEIVE   23

#define OLD_BASE    0x38
#define SLOT_SIZE   0x18
#define MAX_SLOTS   16

#define MSG_TIMESTAMP 0x1B

#pragma pack(push, 4)
typedef struct RakGuid { uint64_t g; uint16_t idx; uint16_t pad[3]; } RakGuid;
typedef struct SysAddr { uint8_t sa[16]; uint16_t debug_port; uint16_t idx; } SysAddr;
typedef struct AOG { RakGuid guid; SysAddr addr; uint32_t pad; } AOG;
typedef struct Packet {
    SysAddr addr; uint32_t pad; RakGuid guid;
    uint32_t length, bit_size; uint8_t *data; uint8_t delete_data, local;
} Packet;
typedef struct BitStream { uint32_t bits_used, bits_alloc, read_off; uint8_t *data; } BitStream;
#pragma pack(pop)
typedef char check_aog[sizeof(AOG) == 0x28 ? 1 : -1];
typedef char check_pkt[offsetof(Packet, guid) == 0x18 && offsetof(Packet, length) == 0x28 &&
                       offsetof(Packet, data) == 0x30 ? 1 : -1];

typedef uint16_t(__fastcall *NumConnFn)(void *self, void *edx);
typedef uint32_t(__fastcall *SendBsFn)(void *self, void *edx, const BitStream *bs, int prio, int rel,
                                       char chan, AOG target, char broadcast, uint32_t receipt);
typedef uint32_t(__fastcall *SendRawFn)(void *self, void *edx, const char *data, int len, int prio, int rel,
                                        char chan, AOG target, char broadcast, uint32_t receipt);
typedef Packet *(__fastcall *ReceiveFn)(void *self, void *edx);

static NetTraceConfig g_cfg;
static FILE *g_f;
static CRITICAL_SECTION g_cs;
static uint32_t g_t0, g_last_snap, g_last_sum;
static NumConnFn o_numconn;
static SendBsFn o_send_bs;
static SendRawFn o_send_raw;
static ReceiveFn o_receive;
static uint32_t g_count[2][256];        // [0]=sent [1]=received, by message ID
static uint32_t g_bytes[2][256];
static uint32_t g_sum_count[2][256];    // at the last summary

// ------------------------------------------------------------------ output
static void out(const char *fmt, ...) {
    va_list ap;
    if (!g_f) return;
    EnterCriticalSection(&g_cs);
    fprintf(g_f, "%8.3f ", (GetTickCount() - g_t0) / 1000.0);
    va_start(ap, fmt);
    vfprintf(g_f, fmt, ap);
    va_end(ap);
    fputc('\n', g_f);
    fflush(g_f);
    LeaveCriticalSection(&g_cs);
}

static const char *msg_name(int id) {
    switch (id) {
    case 0x00: return "CONNECTED_PING";
    case 0x01: return "UNCONNECTED_PING";
    case 0x03: return "CONNECTED_PONG";
    case 0x04: return "DETECT_LOST_CONNECTIONS";
    case 0x09: return "CONNECTION_REQUEST";
    case 0x10: return "CONNECTION_REQUEST_ACCEPTED";
    case 0x11: return "CONNECTION_ATTEMPT_FAILED";
    case 0x12: return "ALREADY_CONNECTED";
    case 0x13: return "NEW_INCOMING_CONNECTION";
    case 0x14: return "NO_FREE_INCOMING_CONNECTIONS";
    case 0x15: return "DISCONNECTION_NOTIFICATION";
    case 0x16: return "CONNECTION_LOST";
    case 0x17: return "CONNECTION_BANNED";
    case 0x18: return "INVALID_PASSWORD";
    case 0x19: return "INCOMPATIBLE_PROTOCOL_VERSION";
    case 0x1A: return "IP_RECENTLY_CONNECTED";
    case 0x1B: return "TIMESTAMP";
    case 0x83: return "game:SESSION_DATA";
    case 0x84: return "game:SESSION_0x84";
    case 0x86: return "game:SLOT_STATES";
    case 0x87: return "game:NETVARS_ALL";
    case 0x88: return "game:NETVARS_SET";
    case 0x89: return "game:LEVEL_ACK";
    case 0x8c: return "game:0x8c";
    case 0x8e: return "game:PROFILE";
    }
    return id >= 0x80 ? "game" : "raknet";
}

static int msg_id(const uint8_t *d, uint32_t len) {
    if (!d || len < 1) return -1;
    if (d[0] == MSG_TIMESTAMP) return len > 9 ? d[9] : -1;
    return d[0];
}

static int wants_full_dump(int id) {
    return id == 0x83 || id == 0x84 || id == 0x86 || id == 0x89 || id == 0x8c || (id >= 0x09 && id <= 0x1A);
}

static void trace(int dir, const uint8_t *d, uint32_t len, const RakGuid *peer, int broadcast) {
    int id = msg_id(d, len);
    uint32_t n, i, show;
    char hex[3 * 160 + 1];
    if (id < 0) return;
    EnterCriticalSection(&g_cs);
    n = ++g_count[dir][id];
    g_bytes[dir][id] += len;
    LeaveCriticalSection(&g_cs);
    if (n > (uint32_t)g_cfg.full && (g_cfg.every <= 0 || n % (uint32_t)g_cfg.every)) return;
    show = len < (wants_full_dump(id) ? 160u : 32u) ? len : (wants_full_dump(id) ? 160u : 32u);
    for (i = 0; i < show; i++) sprintf(hex + 3 * i, "%02x ", d[i]);
    hex[3 * show] = 0;
    out("%s %-28s id=%#04x #%-5u len=%-5u %s%016llx | %s%s",
        dir ? "RECV" : "SEND", msg_name(id), id, n, len,
        dir ? "from " : broadcast ? "bcast-except " : "to ",
        peer ? (unsigned long long)peer->g : 0ull, hex, show < len ? "..." : "");
}

// ------------------------------------------------------------------ session snapshot
typedef struct Snap {
    uint8_t valid, is_host, accepting;
    uint16_t conns;
    uint32_t session, status;
    int32_t local_slot, player_count;
    uint8_t dirty;
    uint8_t old_slots[2][SLOT_SIZE];
    uint8_t new_slots[MAX_SLOTS][SLOT_SIZE];
} Snap;
static Snap g_prev;

static void take(Snap *s, void *self) {
    memset(s, 0, sizeof *s);
    __try {
        uint8_t *ses = *(uint8_t **)(uintptr_t)SESSION_GLOBAL;
        uint8_t *acc = *(uint8_t **)(uintptr_t)ACCEPT_GLOBAL;
        int i;
        s->is_host = *(volatile uint8_t *)(uintptr_t)IS_HOST_GLOBAL;
        s->accepting = acc ? acc[8] : 0xFF;
        s->status = *(volatile uint32_t *)(uintptr_t)SLOT_STATUS_GLOBAL;
        s->conns = self && o_numconn ? o_numconn(self, NULL) : 0xFFFF;
        s->session = (uint32_t)(uintptr_t)ses;
        if (ses) {
            s->dirty = ses[0x68];
            s->local_slot = *(int32_t *)(ses + 0x6C);
            s->player_count = *(int32_t *)(ses + 0x70);
            memcpy(s->old_slots, ses + OLD_BASE, sizeof s->old_slots);
            for (i = 0; i < g_cfg.slot_count && i < MAX_SLOTS; i++)
                memcpy(s->new_slots[i], ses + g_cfg.slot_new_base + i * SLOT_SIZE, SLOT_SIZE);
        }
        s->valid = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        s->valid = 0;
    }
}

static void log_slot(const char *arr, int i, const uint8_t *sl) {
    uint64_t g;
    uint16_t idx;
    uint32_t state;
    memcpy(&g, sl, 8);
    memcpy(&idx, sl + 8, 2);
    memcpy(&state, sl + 0x10, 4);
    if (g == ~0ull && !state && !sl[0x14] && !sl[0x15] && !sl[0x16] && !sl[0x17]) {
        out("    %s[%2d] empty", arr, i);
        return;
    }
    out("    %s[%2d] guid=%016llx sysidx=%u state=%u flags=%02x %02x %02x %02x", arr, i,
        (unsigned long long)g, idx, state, sl[0x14], sl[0x15], sl[0x16], sl[0x17]);
}

static void snapshot(void *self, int force) {
    Snap s;
    int i;
    take(&s, self);
    if (!force && !memcmp(&s, &g_prev, sizeof s)) return;
    g_prev = s;
    if (!s.valid) { out("SESSION snapshot: read faulted"); return; }
    out("SESSION %s conns=%u accepting=%d status@b9d10c=%u session=%#x localSlot=%d playerCount=%d dirty=%u",
        s.is_host ? "HOST" : "CLIENT", s.conns, s.accepting == 0xFF ? -1 : s.accepting, s.status,
        s.session, s.local_slot, s.player_count, s.dirty);
    if (!s.session) return;
    for (i = 0; i < 2; i++) log_slot("old", i, s.old_slots[i]);
    for (i = 0; i < g_cfg.slot_count && i < MAX_SLOTS; i++) log_slot("new", i, s.new_slots[i]);
}

static void summary(void) {
    int dir, id;
    char line[1024];
    for (dir = 0; dir < 2; dir++) {
        int len = 0;
        line[0] = 0;
        for (id = 0; id < 256; id++) {
            uint32_t d = g_count[dir][id] - g_sum_count[dir][id];
            if (!d) continue;
            if (len < (int)sizeof line - 32)
                len += sprintf(line + len, " %02x:%u", id, d);
            g_sum_count[dir][id] = g_count[dir][id];
        }
        if (len) out("COUNT %s last %ds:%s", dir ? "recv" : "sent", g_cfg.summary_s, line);
    }
}

// ------------------------------------------------------------------ hooks
static uint32_t __fastcall h_send_bs(void *self, void *edx, const BitStream *bs, int prio, int rel,
                                     char chan, AOG t, char bc, uint32_t receipt) {
    uint32_t r;
    if (bs && bs->data) trace(0, bs->data, (bs->bits_used + 7) / 8, &t.guid, bc);
    r = o_send_bs(self, edx, bs, prio, rel, chan, t, bc, receipt);
    if (!r && bs && bs->data) out("  ^ Send returned 0 (not sent)");
    return r;
}

static uint32_t __fastcall h_send_raw(void *self, void *edx, const char *data, int len, int prio, int rel,
                                      char chan, AOG t, char bc, uint32_t receipt) {
    uint32_t r;
    if (data && len > 0) trace(0, (const uint8_t *)data, (uint32_t)len, &t.guid, bc);
    r = o_send_raw(self, edx, data, len, prio, rel, chan, t, bc, receipt);
    if (!r && data && len > 0) out("  ^ Send returned 0 (not sent)");
    return r;
}

static Packet *__fastcall h_receive(void *self, void *edx) {
    Packet *p = o_receive(self, edx);
    uint32_t now = GetTickCount();
    if (p && p->data) trace(1, p->data, p->length, &p->guid, 0);
    // Snapshot after the packet is handled would be ideal; the next drain sees its effect.
    if (now - g_last_snap >= (uint32_t)g_cfg.snapshot_ms || (p && p->data && msg_id(p->data, p->length) < 0x80)) {
        g_last_snap = now;
        snapshot(self, 0);
    }
    if (g_cfg.summary_s > 0 && now - g_last_sum >= (uint32_t)g_cfg.summary_s * 1000u) {
        g_last_sum = now;
        summary();
    }
    return p;
}

// ------------------------------------------------------------------ install
int nettrace_start(const NetTraceConfig *cfg) {
    uint32_t *vt = (uint32_t *)(uintptr_t)RAKPEER_VTABLE;
    DWORD old;
    g_cfg = *cfg;
    if (g_cfg.full < 0) g_cfg.full = 0;
    if (g_cfg.snapshot_ms < 10) g_cfg.snapshot_ms = 10;
    if (g_cfg.slot_count > MAX_SLOTS) g_cfg.slot_count = MAX_SLOTS;
    InitializeCriticalSection(&g_cs);
    g_f = fopen(g_cfg.path, "w");
    if (!g_f) { g_cfg.log("nettrace: cannot open %s; trace off", g_cfg.path); return 0; }
    g_t0 = g_last_snap = g_last_sum = GetTickCount();
    if (!VirtualProtect(vt, 4 * (V_RECEIVE + 1), PAGE_READWRITE, &old)) {
        g_cfg.log("nettrace: VirtualProtect failed; trace off");
        return 0;
    }
    o_numconn = (NumConnFn)(uintptr_t)vt[V_NUMCONN];
    o_send_bs = (SendBsFn)(uintptr_t)vt[V_SEND_BS];
    o_send_raw = (SendRawFn)(uintptr_t)vt[V_SEND_RAW];
    o_receive = (ReceiveFn)(uintptr_t)vt[V_RECEIVE];
    vt[V_SEND_BS] = (uint32_t)(uintptr_t)h_send_bs;
    vt[V_SEND_RAW] = (uint32_t)(uintptr_t)h_send_raw;
    vt[V_RECEIVE] = (uint32_t)(uintptr_t)h_receive;
    VirtualProtect(vt, 4 * (V_RECEIVE + 1), old, &old);
    out("nettrace on: full=%d every=%d summary_s=%d snapshot_ms=%d slots=%d @%#x "
        "(chained send_bs=%#x send_raw=%#x receive=%#x)", g_cfg.full, g_cfg.every, g_cfg.summary_s,
        g_cfg.snapshot_ms, g_cfg.slot_count, g_cfg.slot_new_base, (uint32_t)(uintptr_t)o_send_bs,
        (uint32_t)(uintptr_t)o_send_raw, (uint32_t)(uintptr_t)o_receive);
    g_cfg.log("nettrace: writing %s", g_cfg.path);
    return 1;
}

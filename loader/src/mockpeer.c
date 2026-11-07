// mockpeer.c - see mockpeer.h. Game side of the mock players: RakPeer vtable hooks,
// packet injection, and the `mock` dev-console command.
//
// RakNet 4 layouts in OrcsMustDie2.exe (0x51DD91A9), checked against the exported RakPeer
// methods: SystemAddress is a sockaddr_in + debugPort + systemIndex (0x14 bytes),
// RakNetGUID is {u64 g; u16 systemIndex} (0x10), AddressOrGUID is {guid; address} (0x28,
// passed by value), Packet is {address; guid @0x18; length @0x28; bitSize; data @0x30}.
// Every vtable entry is compared with the expected original before anything is written.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "mockpeer.h"
#include "mockbrain.h"

#define RAKPEER_GLOBAL   0x00B9D0B8u
#define IS_HOST_GLOBAL   0x00B9D0ADu
#define SESSION_GLOBAL   0x00B9D108u
#define ACCEPT_GLOBAL    0x00BBB99Cu    // [ptr]+8 != 0: Session::OnConnect (0x4B23F0) accepts
#define RAKPEER_VTABLE   0x0083375Cu

#define V_NUMCONN   9
#define V_SEND_BS   19
#define V_SEND_RAW  20
#define V_RECEIVE   23
#define V_CLOSE     26
#define V_STATE     27
#define V_PING_AVG  40
#define V_PING_LAST 41
#define V_PING_LOW  42
#define V_PUSHBACK  67
#define V_ALLOC     69

#define IS_CONNECTED 2
#define IS_NOT_CONNECTED 6

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
typedef void(__fastcall *CloseFn)(void *self, void *edx, AOG target, char notify, uint8_t chan, int prio);
typedef int(__fastcall *AogIntFn)(void *self, void *edx, AOG target);
typedef void(__fastcall *PushBackFn)(void *self, void *edx, Packet *p, char at_head);
typedef Packet *(__fastcall *AllocFn)(void *self, void *edx, unsigned size);

static MockConfig g_cfg;
static int g_on;
static MockBrain g_brain;
static CRITICAL_SECTION g_cs;       // brain + capture state (Send may come from any thread)
static NumConnFn o_numconn;
static SendBsFn o_send_bs;
static SendRawFn o_send_raw;
static ReceiveFn o_receive;
static CloseFn o_close;
static AogIntFn o_state, o_ping_avg, o_ping_last, o_ping_low;
static PushBackFn o_pushback;
static AllocFn o_alloc;
static uint32_t g_last_tick, g_receipt = 0x4D000000u;

// What the host sent the mocks: per mock and message id, plus a ring of recent messages.
static uint32_t g_rx[MOCK_MAX][256];
typedef struct Cap { uint32_t t; uint16_t mask; uint8_t id, n; uint32_t len; uint8_t head[24]; } Cap;
#define RING 128
static Cap g_ring[RING];
static uint32_t g_ring_n;

#define LOG(...) do { if (g_cfg.log) g_cfg.log(__VA_ARGS__); } while (0)

// ------------------------------------------------------------------ identities
static void mock_addr(SysAddr *a, int i) {
    memset(a, 0, sizeof *a);
    a->sa[0] = 2;                                   // AF_INET
    a->sa[2] = (uint8_t)((61000 + i) >> 8);         // port, network order
    a->sa[3] = (uint8_t)(61000 + i);
    a->sa[4] = 10; a->sa[5] = 77; a->sa[6] = 0; a->sa[7] = (uint8_t)(i + 1);   // 10.77.0.<i+1>
    a->debug_port = (uint16_t)(61000 + i);
    a->idx = 0xFFFF;                                // unassigned: RakPeer never indexes with it
}

// Mock index the target names, or -1. RakNet uses the GUID unless it is unassigned.
static int target_mock(const AOG *t) {
    int i = mockbrain_index(&g_brain, t->guid.g);
    if (i >= 0) return i;
    if (t->guid.g == ~0ull && t->addr.sa[0] == 2 && t->addr.sa[4] == 10 && t->addr.sa[5] == 77 && t->addr.sa[6] == 0) {
        i = t->addr.sa[7] - 1;
        if (i >= 0 && i < g_brain.count) return i;
    }
    return -1;
}

// ------------------------------------------------------------------ game state
static void *rakpeer(void) { return *(void **)(uintptr_t)RAKPEER_GLOBAL; }
static uint8_t *session(void) { return *(uint8_t **)(uintptr_t)SESSION_GLOBAL; }
static int is_host(void) { return *(volatile uint8_t *)(uintptr_t)IS_HOST_GLOBAL != 0; }
static int accepting(void) {
    uint8_t *a = *(uint8_t **)(uintptr_t)ACCEPT_GLOBAL;
    return a && a[8];
}

static MockSlotView env_slot(void *ctx, uint64_t guid) {
    MockSlotView v = { -1, -1, 0, 0, 0 };
    uint8_t *s = session();
    int i;
    (void)ctx;
    if (!s) return v;
    for (i = 0; i < g_cfg.slot_count; i++) {
        uint8_t *sl = s + g_cfg.slot_base + i * 0x18;
        if (*(uint64_t *)sl == guid) {
            v.index = i;
            v.state = *(int *)(sl + 0x10);
            v.joined = sl[0x14]; v.ack = sl[0x15]; v.playerjoin = sl[0x16];
            return v;
        }
    }
    return v;
}

// Queue a packet as if mock i had sent it; the game's net loop (0x457150) receives it.
static void env_push(void *self, int i, const uint8_t *data, int len) {
    Packet *p;
    if (!self || len <= 0) return;
    p = o_alloc(self, 0, (unsigned)len);
    if (!p || !p->data) { LOG("mock %d: AllocatePacket(%d) failed", i + 1, len); return; }
    memcpy(p->data, data, len);
    p->length = (uint32_t)len;
    p->bit_size = (uint32_t)len * 8;
    mock_addr(&p->addr, i);
    memset(&p->guid, 0, sizeof p->guid);
    p->guid.g = MOCK_GUID(i);
    p->guid.idx = 0xFFFF;
    o_pushback(self, 0, p, 0);
}

static void make_env(MockEnv *e, void *self) {
    uint8_t *s = session();
    e->ctx = self;
    e->now = GetTickCount();
    e->hosting = self && is_host() && s;
    e->accepting = e->hosting && accepting();
    e->session = (uintptr_t)s;
    e->slot = env_slot;
    e->push = env_push;
    e->log = g_cfg.log;
}

// ------------------------------------------------------------------ capture
static void capture(uint16_t mask, const uint8_t *data, uint32_t len) {
    int id = mockbrain_msg_id(data, (int)len), i;
    Cap *c;
    if (!mask || id < 0) return;
    EnterCriticalSection(&g_cs);
    for (i = 0; i < g_brain.count; i++)
        if (mask & (1u << i)) {
            if (!g_rx[i][id]) LOG("mock %d: first message 0x%02x from host (%u bytes)", i + 1, id, len);
            g_rx[i][id]++;
        }
    mockbrain_host_msg(&g_brain, mask, data, (int)len);
    c = &g_ring[g_ring_n++ % RING];
    c->t = GetTickCount();
    c->mask = mask;
    c->id = (uint8_t)id;
    c->len = len;
    c->n = (uint8_t)(len < sizeof c->head ? len : sizeof c->head);
    memcpy(c->head, data, c->n);
    LeaveCriticalSection(&g_cs);
}

// Which mocks receive a send to `t`: one mock (unicast), or all connected mocks except `t`.
static uint16_t recipients(const AOG *t, char broadcast, int *to_mock) {
    int k = target_mock(t), i;
    uint16_t m = 0;
    *to_mock = k >= 0;
    if (!broadcast) return (k >= 0 && mockbrain_is_connected(&g_brain, k)) ? (uint16_t)(1u << k) : 0;
    for (i = 0; i < g_brain.count; i++)
        if (i != k && mockbrain_is_connected(&g_brain, i)) m |= (uint16_t)(1u << i);
    return m;
}

// ------------------------------------------------------------------ hooks
static uint16_t __fastcall h_numconn(void *self, void *edx) {
    return (uint16_t)(o_numconn(self, edx) + mockbrain_connected(&g_brain));
}

static uint32_t __fastcall h_send_bs(void *self, void *edx, const BitStream *bs, int prio, int rel,
                                     char chan, AOG t, char bc, uint32_t receipt) {
    int to_mock;
    uint16_t m = recipients(&t, bc, &to_mock);
    if (m && bs && bs->data) capture(m, bs->data, (bs->bits_used + 7) / 8);
    if (to_mock && !bc) return m ? ++g_receipt : 0;
    return o_send_bs(self, edx, bs, prio, rel, chan, t, bc, receipt);
}

static uint32_t __fastcall h_send_raw(void *self, void *edx, const char *data, int len, int prio, int rel,
                                      char chan, AOG t, char bc, uint32_t receipt) {
    int to_mock;
    uint16_t m = recipients(&t, bc, &to_mock);
    if (m && data && len > 0) capture(m, (const uint8_t *)data, (uint32_t)len);
    if (to_mock && !bc) return m ? ++g_receipt : 0;
    return o_send_raw(self, edx, data, len, prio, rel, chan, t, bc, receipt);
}

static Packet *__fastcall h_receive(void *self, void *edx) {
    uint32_t now = GetTickCount();
    if (now - g_last_tick >= 15) {          // the game drains Receive() in a loop; think once
        MockEnv e;
        g_last_tick = now;
        make_env(&e, self);
        EnterCriticalSection(&g_cs);
        mockbrain_tick(&g_brain, &e);
        LeaveCriticalSection(&g_cs);
    }
    return o_receive(self, edx);
}

static void __fastcall h_close(void *self, void *edx, AOG t, char notify, uint8_t chan, int prio) {
    int k = target_mock(&t);
    if (k >= 0) {
        LOG("mock %d: host closed the connection (notify=%d)", k + 1, notify);
        EnterCriticalSection(&g_cs);
        mockbrain_closed(&g_brain, k, GetTickCount());
        LeaveCriticalSection(&g_cs);
        return;
    }
    o_close(self, edx, t, notify, chan, prio);
}

static int __fastcall h_state(void *self, void *edx, AOG t) {
    int k = target_mock(&t);
    if (k >= 0) return mockbrain_is_connected(&g_brain, k) ? IS_CONNECTED : IS_NOT_CONNECTED;
    return o_state(self, edx, t);
}

#define PING_HOOK(name, orig)                                                   \
    static int __fastcall name(void *self, void *edx, AOG t) {                  \
        int k = target_mock(&t);                                                \
        if (k >= 0) return mockbrain_is_connected(&g_brain, k) ? 30 + 10 * k : -1; \
        return orig(self, edx, t);                                              \
    }
PING_HOOK(h_ping_avg, o_ping_avg)
PING_HOOK(h_ping_last, o_ping_last)
PING_HOOK(h_ping_low, o_ping_low)

// ------------------------------------------------------------------ install
static const struct { int slot; uint32_t orig; void *hook; void **save; } HOOKS[] = {
    { V_NUMCONN,   0x005FA9E0u, (void *)h_numconn,   (void **)&o_numconn },
    { V_SEND_BS,   0x005FE230u, (void *)h_send_bs,   (void **)&o_send_bs },
    { V_SEND_RAW,  0x005FE110u, (void *)h_send_raw,  (void **)&o_send_raw },
    { V_RECEIVE,   0x005FCFE0u, (void *)h_receive,   (void **)&o_receive },
    { V_CLOSE,     0x005FEE60u, (void *)h_close,     (void **)&o_close },
    { V_STATE,     0x005FACC0u, (void *)h_state,     (void **)&o_state },
    { V_PING_AVG,  0x005FD300u, (void *)h_ping_avg,  (void **)&o_ping_avg },
    { V_PING_LAST, 0x005FD3A0u, (void *)h_ping_last, (void **)&o_ping_last },
    { V_PING_LOW,  0x005FD430u, (void *)h_ping_low,  (void **)&o_ping_low },
    { V_PUSHBACK,  0x005FB920u, NULL,                (void **)&o_pushback },    // called, not hooked
    { V_ALLOC,     0x005FB9F0u, NULL,                (void **)&o_alloc },
};
#define NHOOKS (sizeof HOOKS / sizeof HOOKS[0])

int mock_start(const MockConfig *cfg) {
    uint32_t *vt = (uint32_t *)(uintptr_t)RAKPEER_VTABLE;
    DWORD old;
    size_t i;
    g_cfg = *cfg;
    if (g_cfg.party_size < 2 || g_cfg.party_size - 1 > MOCK_MAX || g_cfg.party_size > g_cfg.slot_count) {
        LOG("mpdebug: party_size=%d must be 2..%d (slots in the session); mocks off",
            g_cfg.party_size, g_cfg.slot_count < MOCK_MAX + 1 ? g_cfg.slot_count : MOCK_MAX + 1);
        return 0;
    }
    for (i = 0; i < NHOOKS; i++)
        if (vt[HOOKS[i].slot] != HOOKS[i].orig) {
            LOG("mpdebug: RakPeer vtable[%d] holds %#x, expected %#x; mocks off",
                HOOKS[i].slot, vt[HOOKS[i].slot], HOOKS[i].orig);
            return 0;
        }
    InitializeCriticalSection(&g_cs);
    mockbrain_init(&g_brain, g_cfg.party_size - 1, (uint32_t)g_cfg.join_delay_ms, (uint32_t)g_cfg.settle_ms);
    g_brain.ready_mode = (MockReadyMode)g_cfg.ready_mode;
    g_brain.mirror_profile = g_cfg.mirror_profile;
    if (!VirtualProtect(vt, 4 * (V_ALLOC + 1), PAGE_READWRITE, &old)) { LOG("mpdebug: VirtualProtect failed"); return 0; }
    for (i = 0; i < NHOOKS; i++) {
        *HOOKS[i].save = (void *)(uintptr_t)vt[HOOKS[i].slot];
        if (HOOKS[i].hook) vt[HOOKS[i].slot] = (uint32_t)(uintptr_t)HOOKS[i].hook;
    }
    VirtualProtect(vt, 4 * (V_ALLOC + 1), old, &old);
    g_on = 1;
    LOG("mpdebug: %d mock player(s) armed (party_size=%d, join_delay_ms=%d, settle_ms=%d, ready=%s, "
        "mirror_profile=%d, slots %d @%#x); they join when you host a party", g_brain.count, g_cfg.party_size,
        g_cfg.join_delay_ms, g_cfg.settle_ms, mockbrain_ready_mode_name(g_brain.ready_mode),
        g_brain.mirror_profile, g_cfg.slot_count, g_cfg.slot_base);
    return 1;
}

int mock_enabled(void) { return g_on; }

void mock_log_recent(int n) {
    uint32_t k, start;
    char hex[sizeof g_ring[0].head * 2 + 1];
    if (n <= 0 || n > RING) n = RING;
    start = g_ring_n > (uint32_t)n ? g_ring_n - n : 0;
    LOG("  last %u message(s) the host sent to mocks (now %lu):", g_ring_n - start, GetTickCount());
    for (k = start; k < g_ring_n; k++) {
        const Cap *c = &g_ring[k % RING];
        int j;
        for (j = 0; j < c->n; j++) sprintf(hex + j * 2, "%02x", c->head[j]);
        hex[c->n * 2] = 0;
        LOG("    t=%u to=%#x id=0x%02x len=%u %s", c->t, c->mask, c->id, c->len, hex);
    }
}

// ------------------------------------------------------------------ `mock` command
typedef struct Out { char *b; int cap, len; } Out;
static void put(Out *o, const char *fmt, ...) {
    va_list ap;
    int n;
    if (o->len >= o->cap - 1) return;
    va_start(ap, fmt);
    n = _vsnprintf(o->b + o->len, o->cap - o->len - 1, fmt, ap);
    va_end(ap);
    if (n < 0) o->len = o->cap - 1; else o->len += n;
    o->b[o->len] = 0;
}
static void fail(Out *o, const char *msg) { o->len = 0; put(o, "{\"ok\":false,\"error\":\"%s\"}", msg); }

static void status(Out *o) {
    uint8_t *s = session();
    int i, id;
    put(o, "{\"ok\":true,\"enabled\":true,\"party_size\":%d,\"rakpeer\":%s,\"host\":%s,\"session\":\"%#x\","
           "\"accepting\":%s,\"connections_real\":%d,\"ready_mode\":\"%s\",\"host_ready\":%d,"
           "\"host_vars\":%d,\"host_vars_bad\":%u,\"host_profile_bytes\":%d,\"mirror_lost\":%u,\"mocks\":[",
        g_cfg.party_size, rakpeer() ? "true" : "false", is_host() ? "true" : "false", (unsigned)(uintptr_t)s,
        accepting() ? "true" : "false", rakpeer() ? (int)o_numconn(rakpeer(), 0) : 0,
        mockbrain_ready_mode_name(g_brain.ready_mode), g_brain.host_ready, g_brain.host_vars.n,
        g_brain.host_vars_bad, g_brain.host_profile_len, g_brain.outbox_lost);
    for (i = 0; i < g_brain.count; i++) {
        const Mock *m = &g_brain.m[i];
        MockSlotView v = env_slot(NULL, m->guid);
        int first = 1;
        put(o, "%s{\"i\":%d,\"guid\":\"%#llx\",\"state\":\"%s\",\"slot\":%d,\"slot_state\":%d,"
               "\"joined\":%d,\"ack\":%d,\"playerjoin\":%d,\"ready\":%d,\"manual\":%d,\"mirrored\":%u,"
               "\"pushed\":%u,\"why\":\"%s\",\"rx\":{",
            i ? "," : "", i + 1, (unsigned long long)m->guid, mockbrain_state_name(m->st), v.index, v.state,
            v.joined, v.ack, v.playerjoin, m->ready, m->manual, m->mirrored, m->pushed, m->why ? m->why : "");
        for (id = 0; id < 256; id++)
            if (g_rx[i][id]) { put(o, "%s\"0x%02x\":%u", first ? "" : ",", id, g_rx[i][id]); first = 0; }
        put(o, "}}");
    }
    put(o, "]}");
}

static void recent(Out *o, int n) {
    uint32_t k, start;
    int first = 1;
    if (n <= 0 || n > RING) n = 32;
    start = g_ring_n > (uint32_t)n ? g_ring_n - n : 0;
    if (g_ring_n > RING && start < g_ring_n - RING) start = g_ring_n - RING;
    put(o, "{\"ok\":true,\"total\":%u,\"messages\":[", g_ring_n);
    for (k = start; k < g_ring_n; k++) {
        const Cap *c = &g_ring[k % RING];
        int j;
        put(o, "%s{\"t\":%u,\"to\":%u,\"id\":\"0x%02x\",\"len\":%u,\"head\":\"", first ? "" : ",", c->t, c->mask, c->id, c->len);
        for (j = 0; j < c->n; j++) put(o, "%02x", c->head[j]);
        put(o, "\"}");
        first = 0;
    }
    put(o, "]}");
}

// The host's net variables as the mocks copy them. Names are hashed on the wire; we can
// only name the ones we know.
static void vars(Out *o) {
    static const char *const known[] = { "ready" };
    int i, j, k;
    put(o, "{\"ok\":true,\"bad\":%u,\"vars\":[", g_brain.host_vars_bad);
    for (i = 0; i < g_brain.host_vars.n; i++) {
        const NvEntry *v = &g_brain.host_vars.e[i];
        const char *name = "";
        for (k = 0; k < (int)(sizeof known / sizeof known[0]); k++) if (nv_hash(known[k]) == v->key) name = known[k];
        put(o, "%s{\"key\":\"%#010x\",\"name\":\"%s\",\"len\":%d,\"hex\":\"", i ? "," : "", v->key, name, v->len);
        for (j = 0; j < v->len && j < 64; j++) put(o, "%02x", v->val[j]);
        put(o, "\"}");
    }
    put(o, "]}");
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void mock_command(int argc, const char *const *argv, char *out, int cap) {
    Out o = { out, cap, 0 };
    const char *sub = argc > 1 ? argv[1] : "status";
    int i = argc > 2 ? atoi(argv[2]) - 1 : -1;
    MockEnv e;
    out[0] = 0;
    if (!g_on) { put(&o, "{\"ok\":false,\"error\":\"mock players are off (omd2mp.ini [mpdebug] enabled=1; see omd2mp.log)\"}"); return; }
    if (!strcmp(sub, "status") && argc <= 2) { status(&o); return; }
    if (!strcmp(sub, "log") && argc <= 3) { recent(&o, argc > 2 ? atoi(argv[2]) : 32); return; }
    if (!strcmp(sub, "vars") && argc == 2) { EnterCriticalSection(&g_cs); vars(&o); LeaveCriticalSection(&g_cs); return; }
    if (!strcmp(sub, "clear") && argc == 2) {
        EnterCriticalSection(&g_cs);
        memset(g_rx, 0, sizeof g_rx);
        g_ring_n = 0;
        LeaveCriticalSection(&g_cs);
        put(&o, "{\"ok\":true}");
        return;
    }
    if (argc < 3 || i < 0 || i >= g_brain.count) {
        fail(&o, "usage: mock [status] | log [n] | vars | clear | drop <i> [lost] | join <i> | ready <i> [0|1] | send <i> <hex>");
        return;
    }
    make_env(&e, rakpeer());
    EnterCriticalSection(&g_cs);
    if (!strcmp(sub, "drop") && argc <= 4) {
        int lost = argc == 4 && !strcmp(argv[3], "lost");
        if (mockbrain_drop(&g_brain, &e, i, lost, "dev console")) put(&o, "{\"ok\":true}");
        else fail(&o, "mock is not connected");
    } else if (!strcmp(sub, "join") && argc == 3) {
        if (mockbrain_rejoin(&g_brain, i, e.now)) put(&o, "{\"ok\":true,\"note\":\"joins after join_delay_ms\"}");
        else fail(&o, "mock is not dropped");
    } else if (!strcmp(sub, "ready") && argc <= 4) {
        int r = argc == 4 ? atoi(argv[3]) != 0 : 1;
        if (!e.ctx) fail(&o, "no RakPeer yet");
        else if (mockbrain_set_ready(&g_brain, &e, i, r)) put(&o, "{\"ok\":true,\"ready\":%d}", r);
        else fail(&o, "mock has no slot yet (state must be joined)");
    } else if (!strcmp(sub, "send") && argc == 4) {
        uint8_t buf[256];
        int n = 0;
        if (!e.ctx) fail(&o, "no RakPeer yet");
        else if (!mockbrain_is_connected(&g_brain, i)) fail(&o, "mock is not connected");
        else {
            {
                const char *h = argv[3];
                while (h[0] && h[1] && n < (int)sizeof buf && hexval(h[0]) >= 0 && hexval(h[1]) >= 0) {
                    buf[n++] = (uint8_t)(hexval(h[0]) * 16 + hexval(h[1]));
                    h += 2;
                }
                if (*h) { n = 0; fail(&o, "send: payload must be hex bytes, at most 256"); }
            }
            if (n) {
                env_push(e.ctx, i, buf, n);
                g_brain.m[i].pushed++;
                put(&o, "{\"ok\":true,\"pushed\":%d}", n);
            }
        }
    } else fail(&o, "usage: mock [status] | log [n] | vars | clear | drop <i> [lost] | join <i> | ready <i> [0|1] | send <i> <hex>");
    LeaveCriticalSection(&g_cs);
}

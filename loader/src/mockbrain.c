// mockbrain.c - see mockbrain.h.
#include <string.h>
#include "mockbrain.h"

#define LOGF(e, ...) do { if ((e)->log) (e)->log(__VA_ARGS__); } while (0)

static void set_state(Mock *m, MockState s, uint32_t now) { m->st = s; m->t_state = now; }

static void reset_mock(Mock *m, int i) {
    memset(m, 0, sizeof *m);
    m->guid = MOCK_GUID(i);
    m->st = MS_WAIT;
    m->slot = m->slot_state = -1;
    m->ready = -1;
}

// Forget what we told the host about ourselves (a new session wants it again). The host
// sends its 0x87 / 0x8e before it gives the mock a slot, so a slot assignment keeps those
// requests (keep_wants); only a new session drops them.
static void reset_talk(Mock *m, int keep_wants) {
    if (!keep_wants) m->want_vars = m->want_profile = m->want_load = 0;
    m->ready = -1;
    m->t_vars = 0;
}

void mockbrain_init(MockBrain *b, int count, uint32_t join_delay_ms, uint32_t settle_ms) {
    int i;
    memset(b, 0, sizeof *b);
    if (count < 0) count = 0;
    if (count > MOCK_MAX) count = MOCK_MAX;
    b->count = count;
    b->join_delay = join_delay_ms;
    b->settle = settle_ms;
    for (i = 0; i < MOCK_MAX; i++) reset_mock(&b->m[i], i);
    b->ready_mode = READY_FOLLOW;
    b->mirror_profile = 1;
    b->key_ready = nv_hash("ready");
    b->host_ready = -1;
}

static void push2(const MockEnv *e, Mock *m, int i, uint8_t id, uint8_t v);

static void pushn(const MockEnv *e, Mock *m, int i, const uint8_t *d, int len) {
    if (len <= 0) return;
    e->push(e->ctx, i, d, len);
    m->pushed++;
}

// The "ready" value a mock should have now.
static int wanted_ready(const MockBrain *b, const Mock *m) {
    if (m->manual) return m->ready < 0 ? 0 : m->ready;
    switch (b->ready_mode) {
    case READY_AUTO: return 1;
    case READY_MANUAL: return m->ready < 0 ? 0 : m->ready;
    default: return b->host_ready > 0;
    }
}

// Our whole net-variable table: the host's, with our own "ready".
static void send_vars(const MockBrain *b, const MockEnv *e, Mock *m, int i) {
    static NvTable t;               // big; the brain runs on one thread at a time
    static uint8_t out[NV_MAX_ENTRIES * (NV_MAX_VALUE + 5) + 8];
    uint8_t r = (uint8_t)wanted_ready(b, m);
    int n;
    t = b->host_vars;
    nv_set(&t, b->key_ready, &r, 1);
    n = nv_build_all(&t, out, (int)sizeof out);
    pushn(e, m, i, out, n);
    m->ready = r;
    m->want_vars = 0;
    m->t_vars = e->now ? e->now : 1;
    LOGF(e, "mock %d: sent its net variables (0x87, %d entries, ready=%d)", i + 1, t.n, r);
}

static void send_ready(const MockEnv *e, Mock *m, int i, uint32_t key, int r) {
    uint8_t v = (uint8_t)r, out[16];
    pushn(e, m, i, out, nv_build_set(key, 1, &v, 1, out, (int)sizeof out));
    m->ready = r;
    LOGF(e, "mock %d: ready=%d (0x88)", i + 1, r);
}

void mockbrain_host_msg(MockBrain *b, uint16_t mask, const uint8_t *d, int len) {
    int id = mockbrain_msg_id(d, len), i;
    if (id < 0 || !mask) return;
    if (d[0] == MSG_TIMESTAMP) { d += 9; len -= 9; }
    if (id == MSG_NETVARS_ALL) {
        NvTable *t = &b->host_vars;
        const NvEntry *r;
        if (!nv_parse_all(t, d, len)) { nv_clear(t); b->host_vars_bad++; }
        r = nv_get(t, b->key_ready);
        b->host_ready = r && r->len && r->val[0];
        for (i = 0; i < b->count; i++) if (mask & (1u << i)) b->m[i].want_vars = 1;
    } else if (id == MSG_NETVARS_SET) {
        uint32_t key;
        int has, vlen;
        uint8_t v[NV_MAX_VALUE];
        if (!nv_parse_set(d, len, &key, &has, v, &vlen)) return;
        nv_set(&b->host_vars, key, has ? v : 0, has ? vlen : 0);
        if (key == b->key_ready) { b->host_ready = has && vlen && v[0]; return; }
        // The host changed one of its own variables: mocks copy it so they keep looking
        // like the host (same character etc.).
        if (b->outbox_n >= MOCK_OUTBOX || len > MOCK_MSG_MAX) { b->outbox_lost++; return; }
        b->outbox[b->outbox_n].mask = mask;
        b->outbox[b->outbox_n].len = (uint16_t)len;
        memcpy(b->outbox[b->outbox_n].d, d, len);
        b->outbox_n++;
    } else if (id == MSG_LEVEL_LOAD) {
        // Level_Load (0x4A73E0) on the host broadcasts 0x89 {bit go, u32 x3 level info} and
        // then waits: it only starts loading (0x4A71A0 sets +0x11a8) when a client answers
        // with 0x89 {bit 1} after starting its own load. Mocks have nothing to load, so they
        // just answer. A 0x89 with the bit clear is a cancel and needs no answer.
        if (len >= 2 && (d[1] & 0x80))
            for (i = 0; i < b->count; i++) if (mask & (1u << i)) b->m[i].want_load = 1;
    } else if (id == MSG_PROFILE) {
        if (len > MOCK_PROFILE_MAX) return;
        memcpy(b->host_profile, d, len);
        b->host_profile_len = len;
        for (i = 0; i < b->count; i++) if (mask & (1u << i)) b->m[i].want_profile = 1;
    }
}

int mockbrain_set_ready(MockBrain *b, const MockEnv *e, int i, int ready) {
    Mock *m;
    if (i < 0 || i >= b->count || b->m[i].st != MS_JOINED) return 0;
    m = &b->m[i];
    m->manual = 1;
    send_ready(e, m, i, b->key_ready, ready ? 1 : 0);
    if (ready) { push2(e, m, i, MSG_STATE, SLOT_READY); m->t_ready = e->now ? e->now : 1; }
    return 1;
}

const char *mockbrain_ready_mode_name(MockReadyMode m) {
    switch (m) {
    case READY_FOLLOW: return "follow";
    case READY_AUTO: return "auto";
    case READY_MANUAL: return "manual";
    }
    return "?";
}

static void push1(const MockEnv *e, Mock *m, int i, uint8_t id) {
    e->push(e->ctx, i, &id, 1);
    m->pushed++;
}

static void push2(const MockEnv *e, Mock *m, int i, uint8_t id, uint8_t v) {
    uint8_t d[2] = { id, v };
    e->push(e->ctx, i, d, 2);
    m->pushed++;
}

void mockbrain_tick(MockBrain *b, const MockEnv *e) {
    int i, k;
    uint32_t now = e->now;
    if (e->session != b->session) {
        // A new session object. Mocks the host still counts as connected keep their RakNet
        // "connection" and wait for a slot in the new session (connecting again if none
        // comes); the rest start over.
        if (b->session) LOGF(e, "mock: session changed %#lx -> %#lx",
                             (unsigned long)b->session, (unsigned long)e->session);
        b->session = e->session;
        for (i = 0; i < b->count; i++) {
            Mock *m = &b->m[i];
            if (e->session && mockbrain_is_connected(b, i)) {
                set_state(m, MS_CONNECTING, now);
                m->sent_connect = 0;
                m->slot = m->slot_state = -1;
                reset_talk(m, 0);
            } else reset_mock(m, i);
        }
    }
    if (!e->hosting || !e->session) {
        if (b->t_hosting) {
            LOGF(e, "mock: no longer hosting; mocks back to waiting");
            for (i = 0; i < b->count; i++)
                if (b->m[i].st == MS_CONNECTING || b->m[i].st == MS_JOINED) set_state(&b->m[i], MS_WAIT, 0);
        }
        b->t_hosting = 0;
        b->outbox_n = 0;
        return;
    }
    if (!b->t_hosting) { b->t_hosting = now ? now : 1; LOGF(e, "mock: hosting; %d mock(s) will join", b->count); }

    for (i = 0; i < b->count; i++) {
        Mock *m = &b->m[i];
        MockSlotView v;
        switch (m->st) {
        case MS_WAIT: {
            uint32_t base = m->t_state ? m->t_state : b->t_hosting;
            if (e->accepting && now - base >= b->join_delay * (uint32_t)(m->t_state ? 1 : i + 1)) {
                LOGF(e, "mock %d: connecting (guid %#llx)", i + 1, (unsigned long long)m->guid);
                push1(e, m, i, MSG_NEW_INCOMING);
                set_state(m, MS_CONNECTING, now);
                m->sent_connect = 1;
            }
            break;
        }
        case MS_CONNECTING:
            v = e->slot(e->ctx, m->guid);
            if (v.index >= 0) {
                m->slot = v.index;
                m->slot_state = v.state;
                m->t_slot = now;
                LOGF(e, "mock %d: host gave slot %d (state %d); sending 0x84", i + 1, v.index, v.state);
                push1(e, m, i, MSG_LOADED);
                m->t_ack = now;
                m->t_ready = 0;
                reset_talk(m, 1);
                set_state(m, MS_JOINED, now);
            } else if (!m->sent_connect && e->accepting && now - m->t_state >= b->settle) {
                LOGF(e, "mock %d: no slot in the new session; connecting again", i + 1);
                push1(e, m, i, MSG_NEW_INCOMING);
                m->sent_connect = 1;
                m->t_state = now;
            } else if (now - m->t_state >= MOCK_CONNECT_TIMEOUT_MS) {
                m->why = "host assigned no slot within 10 s";
                LOGF(e, "mock %d: %s", i + 1, m->why);
                set_state(m, MS_DROPPED, now);
            }
            break;
        case MS_JOINED:
            v = e->slot(e->ctx, m->guid);
            if (v.index < 0) {
                m->why = "host removed the slot";
                LOGF(e, "mock %d: %s", i + 1, m->why);
                m->slot = m->slot_state = -1;
                set_state(m, MS_DROPPED, now);
                break;
            }
            if (v.state != m->slot_state || v.index != m->slot) {
                LOGF(e, "mock %d: slot %d state %d -> %d", i + 1, v.index, m->slot_state, v.state);
                m->slot_state = v.state;
                m->slot = v.index;
                m->t_slot = now;
            }
            if (!v.ack && now - m->t_ack >= b->settle) {
                push1(e, m, i, MSG_LOADED);
                m->t_ack = now;
            }
            if (v.state >= 3 && v.state != SLOT_READY && now - m->t_slot >= b->settle &&
                (!m->t_ready || now - m->t_ready >= b->settle)) {
                push2(e, m, i, MSG_STATE, SLOT_READY);
                m->t_ready = now ? now : 1;
            }
            // Answer the host's 0x87 with ours (rate-limited), or send ours anyway if the
            // host has said nothing for settle_ms after giving us a slot.
            if ((m->want_vars && (!m->t_vars || now - m->t_vars >= b->settle)) ||
                (m->ready < 0 && !m->want_vars && now - m->t_slot >= b->settle))
                send_vars(b, e, m, i);
            else if (m->ready >= 0 && wanted_ready(b, m) != m->ready)
                send_ready(e, m, i, b->key_ready, wanted_ready(b, m));
            if (m->want_profile && b->mirror_profile && b->host_profile_len) {
                pushn(e, m, i, b->host_profile, b->host_profile_len);
                LOGF(e, "mock %d: sent a copy of the host's profile (0x8e, %d bytes)", i + 1, b->host_profile_len);
            }
            m->want_profile = 0;
            if (m->want_load) {
                push2(e, m, i, MSG_LEVEL_LOAD, 0x80);
                m->want_load = 0;
                LOGF(e, "mock %d: acked the host's level load (0x89)", i + 1);
            }
            if (now - m->t_alive >= MOCK_KEEPALIVE_MS) {
                push1(e, m, i, MSG_KEEPALIVE);
                m->t_alive = now;
            }
            break;
        case MS_DROPPED:
            break;
        }
    }
    for (k = 0; k < b->outbox_n; k++)
        for (i = 0; i < b->count; i++) {
            Mock *m = &b->m[i];
            if ((b->outbox[k].mask & (1u << i)) && m->st == MS_JOINED && m->ready >= 0) {
                pushn(e, m, i, b->outbox[k].d, b->outbox[k].len);
                m->mirrored++;
            }
        }
    b->outbox_n = 0;
}

int mockbrain_index(const MockBrain *b, uint64_t guid) {
    uint64_t k = guid & 0xFFFFFFFFull;
    if ((guid & 0xFFFFFFFF00000000ull) != MOCK_GUID_TAG || k < 1 || k > (uint64_t)b->count) return -1;
    return (int)k - 1;
}

int mockbrain_is_connected(const MockBrain *b, int i) {
    return i >= 0 && i < b->count && (b->m[i].st == MS_CONNECTING || b->m[i].st == MS_JOINED);
}

int mockbrain_connected(const MockBrain *b) {
    int i, n = 0;
    for (i = 0; i < b->count; i++) n += mockbrain_is_connected(b, i);
    return n;
}

int mockbrain_drop(MockBrain *b, const MockEnv *e, int i, int lost, const char *why) {
    if (!mockbrain_is_connected(b, i)) return 0;
    push1(e, &b->m[i], i, lost ? MSG_LOST : MSG_DISCONNECT);
    b->m[i].why = why;
    set_state(&b->m[i], MS_DROPPED, e->now);
    LOGF(e, "mock %d: %s (%s)", i + 1, lost ? "connection lost" : "disconnected", why);
    return 1;
}

void mockbrain_closed(MockBrain *b, int i, uint32_t now) {
    if (!mockbrain_is_connected(b, i)) return;
    b->m[i].why = "host closed the connection";
    set_state(&b->m[i], MS_DROPPED, now);
}

int mockbrain_rejoin(MockBrain *b, int i, uint32_t now) {
    if (i < 0 || i >= b->count || b->m[i].st != MS_DROPPED) return 0;
    reset_mock(&b->m[i], i);
    b->m[i].t_state = now ? now : 1;
    return 1;
}

const char *mockbrain_state_name(MockState s) {
    switch (s) {
    case MS_WAIT: return "waiting";
    case MS_CONNECTING: return "connecting";
    case MS_JOINED: return "joined";
    case MS_DROPPED: return "dropped";
    }
    return "?";
}

int mockbrain_msg_id(const uint8_t *data, int len) {
    if (!data || len < 1) return -1;
    if (data[0] == MSG_TIMESTAMP) return len > 9 ? data[9] : -1;
    return data[0];
}

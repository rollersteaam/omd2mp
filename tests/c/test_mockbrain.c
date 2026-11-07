// test_mockbrain.c - unit tests for loader/src/mockbrain.c (mock player decisions).
// A tiny fake host stands in for the game: pushing 0x13 gives the mock a slot in state 3,
// 0x84 sets the ack flag, 0x86 <s> sets the state. Portable: no game, no Windows.
#include <stdio.h>
#include <string.h>
#include "mockbrain.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define NSLOT 4
typedef struct Host {
    uint64_t guid[NSLOT];
    int used[NSLOT], state[NSLOT];
    uint8_t ack[NSLOT];
    int pushes, last_mock, last_len;
    uint8_t last[8];
    int count_by_id[256];
    int auto_slot;          // host hands out slots on 0x13
    int ready[NSLOT];       // the mock's "ready" net variable as the host sees it (-1 unset)
    int vars_n[NSLOT];      // entries in the mock's last 0x87
    int profile_len[NSLOT]; // bytes of the mock's last 0x8e
    uint32_t other_key[NSLOT]; // last non-ready 0x88 key from the mock
    int load_ack[NSLOT];    // second byte of the mock's last 0x89 (-1 bad length)
} Host;

static MockSlotView h_slot(void *ctx, uint64_t g) {
    Host *h = ctx;
    MockSlotView v = { -1, -1, 0, 0, 0 };
    int i;
    for (i = 0; i < NSLOT; i++)
        if (h->used[i] && h->guid[i] == g) { v.index = i; v.state = h->state[i]; v.ack = h->ack[i]; v.joined = 1; }
    return v;
}

static int find(Host *h, uint64_t g) {
    int i;
    for (i = 0; i < NSLOT; i++) if (h->used[i] && h->guid[i] == g) return i;
    return -1;
}

static void h_push(void *ctx, int mock, const uint8_t *d, int len) {
    Host *h = ctx;
    uint64_t g = MOCK_GUID(mock);
    int s = find(h, g), i;
    h->pushes++;
    h->last_mock = mock;
    h->last_len = len;
    memcpy(h->last, d, len < 8 ? len : 8);
    h->count_by_id[d[0]]++;
    if (d[0] == MSG_NEW_INCOMING && h->auto_slot && s < 0)
        for (i = 1; i < NSLOT; i++) if (!h->used[i]) { h->used[i] = 1; h->guid[i] = g; h->state[i] = 3; h->ack[i] = 0; break; }
    if (d[0] == MSG_LOADED && s >= 0) h->ack[s] = 1;
    if (d[0] == MSG_STATE && s >= 0 && len == 2) h->state[s] = d[1];
    if ((d[0] == MSG_DISCONNECT || d[0] == MSG_LOST) && s >= 0) h->used[s] = 0;
    if (d[0] == MSG_NETVARS_ALL && s >= 0) {
        static NvTable t;
        const NvEntry *r;
        if (nv_parse_all(&t, d, len)) {
            h->vars_n[s] = t.n;
            r = nv_get(&t, nv_hash("ready"));
            h->ready[s] = r ? r->val[0] : -1;
        } else h->vars_n[s] = -99;
    }
    if (d[0] == MSG_NETVARS_SET && s >= 0) {
        uint32_t key;
        int has, vlen;
        uint8_t v[NV_MAX_VALUE];
        if (nv_parse_set(d, len, &key, &has, v, &vlen)) {
            if (key == nv_hash("ready")) h->ready[s] = has ? v[0] : -1;
            else h->other_key[s] = key;
        }
    }
    if (d[0] == MSG_PROFILE && s >= 0) h->profile_len[s] = len;
    if (d[0] == MSG_LEVEL_LOAD && s >= 0) h->load_ack[s] = len == 2 ? d[1] : -1;
}

// What the host's Send would show the brain: its own net variables to the mocks in `mask`.
static void host_sends_vars(MockBrain *b, uint16_t mask, int ready, uint32_t extra_key) {
    static NvTable t;
    uint8_t buf[512], r = (uint8_t)ready, x = 7;
    nv_clear(&t);
    if (ready >= 0) nv_set(&t, nv_hash("ready"), &r, 1);
    nv_set(&t, extra_key, &x, 1);
    mockbrain_host_msg(b, mask, buf, nv_build_all(&t, buf, sizeof buf));
}

static void host_sets(MockBrain *b, uint16_t mask, uint32_t key, int val) {
    uint8_t buf[32], v = (uint8_t)val;
    mockbrain_host_msg(b, mask, buf, nv_build_set(key, val >= 0, &v, val >= 0, buf, sizeof buf));
}

static MockEnv env(Host *h, uint32_t now, int hosting, uintptr_t session) {
    MockEnv e;
    memset(&e, 0, sizeof e);
    e.ctx = h; e.now = now; e.hosting = hosting; e.accepting = hosting; e.session = session;
    e.slot = h_slot; e.push = h_push;
    return e;
}

static void run(MockBrain *b, Host *h, uint32_t from, uint32_t to, int hosting, uintptr_t session) {
    uint32_t t;
    for (t = from; t <= to; t += 16) { MockEnv e = env(h, t, hosting, session); mockbrain_tick(b, &e); }
}

static void test_three_mocks_join_and_ready(void) {
    MockBrain b;
    Host h;
    int i;
    memset(&h, 0, sizeof h);
    h.auto_slot = 1;
    mockbrain_init(&b, 3, 1000, 500);
    run(&b, &h, 1000, 1500, 0, 0);                      // not hosting: nothing happens
    CHECK(h.pushes == 0 && mockbrain_connected(&b) == 0);
    run(&b, &h, 2000, 2900, 1, 0x1234);                 // hosting from t=2000; first join at 3000
    CHECK(h.pushes == 0);
    run(&b, &h, 2916, 3100, 1, 0x1234);
    CHECK(h.count_by_id[MSG_NEW_INCOMING] == 1 && mockbrain_connected(&b) == 1);
    run(&b, &h, 3116, 8000, 1, 0x1234);                 // staggered: 3000, 4000, 5000
    CHECK(h.count_by_id[MSG_NEW_INCOMING] == 3);
    CHECK(mockbrain_connected(&b) == 3);
    for (i = 0; i < 3; i++) {
        CHECK(b.m[i].st == MS_JOINED);
        CHECK(h.state[i + 1] == SLOT_READY && h.ack[i + 1] == 1);
    }
    CHECK(h.count_by_id[MSG_LOADED] == 3);              // once each: no re-sends while acked
    CHECK(h.count_by_id[MSG_STATE] == 3);
    run(&b, &h, 8016, 20000, 1, 0x1234);                // steady state: silence
    CHECK(h.count_by_id[MSG_LOADED] == 3 && h.count_by_id[MSG_STATE] == 3);
}

static void test_resends_after_host_resets_slot(void) {
    MockBrain b;
    Host h;
    memset(&h, 0, sizeof h);
    h.auto_slot = 1;
    mockbrain_init(&b, 1, 100, 500);
    run(&b, &h, 0, 3000, 1, 0x10);
    CHECK(h.state[1] == SLOT_READY);
    h.state[1] = 3;                                     // level load: back to "joined, not ready"
    h.ack[1] = 0;
    run(&b, &h, 3016, 3400, 1, 0x10);
    CHECK(h.count_by_id[MSG_STATE] == 1);               // waits settle_ms before repeating
    run(&b, &h, 3416, 5000, 1, 0x10);
    CHECK(h.state[1] == SLOT_READY && h.ack[1] == 1);
    CHECK(h.count_by_id[MSG_STATE] == 2 && h.count_by_id[MSG_LOADED] == 2);
}

static void test_no_slot_times_out(void) {
    MockBrain b;
    Host h;
    memset(&h, 0, sizeof h);                            // host never assigns a slot
    mockbrain_init(&b, 1, 100, 500);
    run(&b, &h, 0, 20000, 1, 0x10);
    CHECK(b.m[0].st == MS_DROPPED && b.m[0].why != NULL);
    CHECK(h.count_by_id[MSG_NEW_INCOMING] == 1);
    CHECK(mockbrain_connected(&b) == 0);
}

static void test_drop_and_rejoin(void) {
    MockBrain b;
    Host h;
    MockEnv e;
    memset(&h, 0, sizeof h);
    h.auto_slot = 1;
    mockbrain_init(&b, 2, 100, 200);
    run(&b, &h, 0, 2000, 1, 0x10);
    CHECK(mockbrain_connected(&b) == 2);
    e = env(&h, 2016, 1, 0x10);
    CHECK(mockbrain_drop(&b, &e, 1, 1, "test"));
    CHECK(h.count_by_id[MSG_LOST] == 1 && h.last_mock == 1);
    CHECK(!mockbrain_drop(&b, &e, 1, 0, "again"));     // already gone
    CHECK(mockbrain_connected(&b) == 1);
    run(&b, &h, 2032, 4000, 1, 0x10);
    CHECK(b.m[1].st == MS_DROPPED);                     // stays gone until asked
    CHECK(mockbrain_rejoin(&b, 1, 4000));
    run(&b, &h, 4016, 6000, 1, 0x10);
    CHECK(b.m[1].st == MS_JOINED && mockbrain_connected(&b) == 2);
    mockbrain_closed(&b, 0, 6000);
    CHECK(b.m[0].st == MS_DROPPED && mockbrain_connected(&b) == 1);
}

static void test_session_change_keeps_connections(void) {
    MockBrain b;
    Host h;
    memset(&h, 0, sizeof h);
    h.auto_slot = 1;
    mockbrain_init(&b, 1, 100, 300);
    run(&b, &h, 0, 2000, 1, 0x10);
    CHECK(b.m[0].st == MS_JOINED);
    memset(h.used, 0, sizeof h.used);                   // new session object, empty slots
    run(&b, &h, 2016, 2016, 1, 0x20);
    CHECK(b.m[0].st == MS_CONNECTING && mockbrain_connected(&b) == 1);
    run(&b, &h, 2032, 4000, 1, 0x20);                   // no slot after settle -> connect again
    CHECK(h.count_by_id[MSG_NEW_INCOMING] == 2);
    CHECK(b.m[0].st == MS_JOINED);
    run(&b, &h, 4016, 4100, 1, 0);                      // session gone (back to menu): reset
    CHECK(b.m[0].st == MS_WAIT && mockbrain_connected(&b) == 0);
}

static void test_not_accepting_holds_joins(void) {
    MockBrain b;
    Host h;
    uint32_t t;
    memset(&h, 0, sizeof h);
    h.auto_slot = 1;
    mockbrain_init(&b, 1, 100, 300);
    for (t = 0; t < 3000; t += 16) {
        MockEnv e = env(&h, t, 1, 0x10);
        e.accepting = 0;
        mockbrain_tick(&b, &e);
    }
    CHECK(h.pushes == 0 && b.m[0].st == MS_WAIT);
}

static void test_ids_and_guids(void) {
    MockBrain b;
    uint8_t ts[10] = { MSG_TIMESTAMP, 1, 2, 3, 4, 5, 6, 7, 8, 0x83 };
    uint8_t one[1] = { 0x86 };
    mockbrain_init(&b, 3, 0, 0);
    CHECK(mockbrain_msg_id(ts, 10) == 0x83);
    CHECK(mockbrain_msg_id(ts, 9) == -1);
    CHECK(mockbrain_msg_id(one, 1) == 0x86);
    CHECK(mockbrain_msg_id(one, 0) == -1);
    CHECK(mockbrain_index(&b, MOCK_GUID(0)) == 0);
    CHECK(mockbrain_index(&b, MOCK_GUID(2)) == 2);
    CHECK(mockbrain_index(&b, MOCK_GUID(3)) == -1);    // only 3 mocks
    CHECK(mockbrain_index(&b, MOCK_GUID_TAG) == -1);
    CHECK(mockbrain_index(&b, 0x0123456789ull) == -1);
    CHECK(mockbrain_index(&b, ~0ull) == -1);
    mockbrain_init(&b, 99, 0, 0);
    CHECK(b.count == MOCK_MAX);
}

static void joined(MockBrain *b, Host *h, int n, uint32_t settle) {
    memset(h, 0, sizeof *h);
    memset(h->ready, 0xFF, sizeof h->ready);
    h->auto_slot = 1;
    mockbrain_init(b, n, 100, settle);
}

static void test_follow_host_ready(void) {
    static MockBrain b;
    Host h;
    int i;
    joined(&b, &h, 3, 300);
    run(&b, &h, 0, 1000, 1, 0x10);                     // all three have slots by now
    for (i = 0; i < 3; i++) CHECK(b.m[i].st == MS_JOINED);
    host_sends_vars(&b, 0x7, 0, 0xC0FFEE);            // host -> each new client: its table
    run(&b, &h, 1016, 1100, 1, 0x10);
    for (i = 1; i <= 3; i++) {
        CHECK(h.vars_n[i] == 2);                       // host's entry + our ready
        CHECK(h.ready[i] == 0);                        // host not ready, so neither are we
    }
    host_sets(&b, 0x7, nv_hash("ready"), 1);           // you press Play
    run(&b, &h, 1116, 1200, 1, 0x10);
    for (i = 1; i <= 3; i++) CHECK(h.ready[i] == 1);
    host_sets(&b, 0x7, nv_hash("ready"), 0);           // and change your mind
    run(&b, &h, 1216, 1300, 1, 0x10);
    for (i = 1; i <= 3; i++) CHECK(h.ready[i] == 0);
    host_sets(&b, 0x2, 0xBEEF, 4);                     // other host variables are copied
    run(&b, &h, 1316, 1400, 1, 0x10);
    CHECK(h.other_key[2] == 0xBEEF && h.other_key[1] == 0 && h.other_key[3] == 0);
    CHECK(b.m[1].mirrored == 1 && b.outbox_n == 0);
    run(&b, &h, 1416, 5000, 1, 0x10);                  // and nothing repeats
    CHECK(h.count_by_id[MSG_NETVARS_ALL] == 6);        // unprompted after settle_ms, then the reply
}

static void test_auto_manual_and_command(void) {
    static MockBrain b;
    Host h;
    MockEnv e;
    joined(&b, &h, 2, 300);
    b.ready_mode = READY_AUTO;
    run(&b, &h, 0, 1000, 1, 0x10);
    host_sends_vars(&b, 0x3, 0, 0x99);
    run(&b, &h, 1016, 1100, 1, 0x10);
    CHECK(h.ready[1] == 1 && h.ready[2] == 1);
    e = env(&h, 1200, 1, 0x10);
    CHECK(mockbrain_set_ready(&b, &e, 1, 0));          // command wins and sticks
    CHECK(h.ready[2] == 0 && b.m[1].manual);
    run(&b, &h, 1216, 3000, 1, 0x10);
    CHECK(h.ready[2] == 0 && h.ready[1] == 1);
    e = env(&h, 3016, 1, 0x10);
    CHECK(mockbrain_set_ready(&b, &e, 1, 1) && h.ready[2] == 1 && h.state[2] == SLOT_READY);
    CHECK(!mockbrain_set_ready(&b, &e, 5, 1));

    joined(&b, &h, 1, 300);
    b.ready_mode = READY_MANUAL;
    run(&b, &h, 0, 1000, 1, 0x10);
    host_sends_vars(&b, 0x1, 1, 0x99);                 // host ready, but manual mocks wait
    run(&b, &h, 1016, 2000, 1, 0x10);
    CHECK(h.ready[1] == 0);
}

static void test_vars_sent_without_host_and_profile(void) {
    static MockBrain b;
    Host h;
    uint8_t prof[600];
    joined(&b, &h, 1, 300);
    run(&b, &h, 0, 2000, 1, 0x10);                     // host never sends 0x87
    CHECK(h.count_by_id[MSG_NETVARS_ALL] == 1 && h.vars_n[1] == 1 && h.ready[1] == 0);
    memset(prof, 0x5A, sizeof prof);
    prof[0] = MSG_PROFILE;
    mockbrain_host_msg(&b, 0x1, prof, sizeof prof);
    run(&b, &h, 2016, 3000, 1, 0x10);
    CHECK(h.profile_len[1] == (int)sizeof prof && h.count_by_id[MSG_PROFILE] == 1);
    b.mirror_profile = 0;
    mockbrain_host_msg(&b, 0x1, prof, sizeof prof);
    run(&b, &h, 3016, 4000, 1, 0x10);
    CHECK(h.count_by_id[MSG_PROFILE] == 1);
    // a new session asks for everything again
    memset(h.used, 0, sizeof h.used);
    run(&b, &h, 4016, 8000, 1, 0x20);
    CHECK(b.m[0].st == MS_JOINED && h.count_by_id[MSG_NETVARS_ALL] == 2);
    {                                                  // bad 0x87 from the host is counted
        uint8_t bad[3] = { MSG_NETVARS_ALL, 1, 2 };
        mockbrain_host_msg(&b, 0x1, bad, 3);
        CHECK(b.host_vars_bad == 1 && b.host_vars.n == 0);
    }
}

// The order seen in-game: the host sends its 0x87 and 0x8e to a new client before giving
// it a slot. The mock must still answer both once the slot arrives.
static void test_host_talks_before_slot(void) {
    static MockBrain b;
    Host h;
    uint8_t prof[300];
    joined(&b, &h, 1, 300);
    h.auto_slot = 0;
    run(&b, &h, 0, 200, 1, 0x10);                      // mock 1 connects at 100 ms
    CHECK(b.m[0].st == MS_CONNECTING);
    host_sends_vars(&b, 0x1, 0, 0xC0FFEE);
    memset(prof, 0x11, sizeof prof);
    prof[0] = MSG_PROFILE;
    mockbrain_host_msg(&b, 0x1, prof, sizeof prof);
    run(&b, &h, 216, 300, 1, 0x10);
    CHECK(h.count_by_id[MSG_NETVARS_ALL] == 0 && h.count_by_id[MSG_PROFILE] == 0);
    h.used[1] = 1; h.guid[1] = MOCK_GUID(0); h.state[1] = 3;   // now the slot
    run(&b, &h, 316, 360, 1, 0x10);
    CHECK(b.m[0].st == MS_JOINED);
    CHECK(h.count_by_id[MSG_NETVARS_ALL] == 1 && h.vars_n[1] == 2 && h.ready[1] == 0);
    CHECK(h.profile_len[1] == (int)sizeof prof);
}

// Everyone ready: the host's Level_Load broadcasts 0x89 {go=1, level info} and loads only
// once a client answers 0x89 {1}. A 0x89 with go=0 (cancel) gets no answer.
static void test_acks_level_load(void) {
    static MockBrain b;
    Host h;
    uint8_t go[14] = { MSG_LEVEL_LOAD, 0x80 }, cancel[2] = { MSG_LEVEL_LOAD, 0x00 };
    uint8_t stamped[23] = { MSG_TIMESTAMP };
    int i;
    joined(&b, &h, 3, 300);
    run(&b, &h, 0, 1000, 1, 0x10);
    mockbrain_host_msg(&b, 0x7, cancel, sizeof cancel);
    run(&b, &h, 1016, 1100, 1, 0x10);
    CHECK(h.count_by_id[MSG_LEVEL_LOAD] == 0);
    mockbrain_host_msg(&b, 0x5, go, sizeof go);        // mocks 1 and 3
    run(&b, &h, 1116, 1200, 1, 0x10);
    CHECK(h.count_by_id[MSG_LEVEL_LOAD] == 2);
    CHECK(h.load_ack[1] == 0x80 && h.load_ack[2] == 0 && h.load_ack[3] == 0x80);
    for (i = 0; i < 3; i++) CHECK(!b.m[i].want_load);
    memcpy(stamped + 9, go, sizeof go);                // with a timestamp prefix too
    mockbrain_host_msg(&b, 0x2, stamped, sizeof stamped);
    run(&b, &h, 1216, 3000, 1, 0x10);                  // once each, never repeated
    CHECK(h.count_by_id[MSG_LEVEL_LOAD] == 3 && h.load_ack[2] == 0x80);
}

// Joined mocks keep the host's per-slot "last heard" time fresh (it warns at 6 s of
// silence): one keepalive a second, only while joined.
static void test_keepalive(void) {
    static MockBrain b;
    Host h;
    MockEnv e;
    int before;
    memset(&h, 0, sizeof h);
    h.auto_slot = 1;
    mockbrain_init(&b, 2, 1000, 500);
    run(&b, &h, 1000, 1900, 1, 0x20);                   // nobody joined yet
    CHECK(h.count_by_id[MSG_KEEPALIVE] == 0);
    run(&b, &h, 1916, 4000, 1, 0x20);                   // both joined by ~3000
    CHECK(b.m[0].st == MS_JOINED && b.m[1].st == MS_JOINED);
    before = h.count_by_id[MSG_KEEPALIVE];
    run(&b, &h, 4016, 14000, 1, 0x20);                  // 10 s: ~10 each, never a 6 s gap
    CHECK(h.count_by_id[MSG_KEEPALIVE] - before >= 18 && h.count_by_id[MSG_KEEPALIVE] - before <= 22);
    e = env(&h, 14000, 1, 0x20);
    CHECK(mockbrain_drop(&b, &e, 0, 0, "test"));
    before = h.count_by_id[MSG_KEEPALIVE];
    run(&b, &h, 14016, 24000, 1, 0x20);                 // the dropped mock goes silent
    CHECK(h.count_by_id[MSG_KEEPALIVE] - before >= 9 && h.count_by_id[MSG_KEEPALIVE] - before <= 11);
}

int main(void) {
    test_keepalive();
    test_acks_level_load();
    test_host_talks_before_slot();
    test_follow_host_ready();
    test_auto_manual_and_command();
    test_vars_sent_without_host_and_profile();
    test_three_mocks_join_and_ready();
    test_resends_after_host_resets_slot();
    test_no_slot_times_out();
    test_drop_and_rejoin();
    test_session_change_keeps_connections();
    test_not_accepting_holds_joins();
    test_ids_and_guids();
    printf("mockbrain: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}

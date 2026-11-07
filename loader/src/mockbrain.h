// mockbrain.h - the decision logic for mock players ([mpdebug] in omd2mp.ini).
//
// Pure C, no Windows or game access, so tests/c/test_mockbrain.c can drive it. The game
// side (mockpeer.c) supplies a MockEnv each tick: the time, whether we are a host that
// accepts players, a view of the session slot holding a given GUID, and a way to push a
// packet into RakPeer's receive queue as if the mock had sent it.
//
// What a real client does, as seen by the host (OrcsMustDie2.exe 0x51DD91A9):
//   0x13 ID_NEW_INCOMING_CONNECTION -> 0x4B23F0 assigns a slot (state 3), sends 0x83, Join
//   0x84 (no body)                  -> 0x4B2D40 sets the loaded-ack flag (slot+0x15)
//   0x86 <u8 state>                 -> 0x4B3800 sets the slot state; 5 = ready (UI checks ==5)
//   0x15 / 0x16                     -> disconnect / connection lost
//   0x87 / 0x88                     -> the player's net variables (netvars.h). The lobby UI
//                                      waits for Net_VariableGetBool(i, "ready") on everyone.
//   0x8e                            -> player profile (character etc.; handlers 0x46D8A0..)
// A mock replays those, and repeats 0x84 / 0x86 [5] if the host resets them (level loads).
// It answers each 0x87 / 0x8e the host sends it with its own: a copy of the host's net
// variables (with its own "ready") and of the host's profile, so it looks like the host.
#ifndef OMD2MP_MOCKBRAIN_H
#define OMD2MP_MOCKBRAIN_H
#include <stdint.h>
#include "netvars.h"

#define MOCK_MAX 15
#define MOCK_GUID_TAG 0xFEEDF00D00000000ull
#define MOCK_GUID(i) (MOCK_GUID_TAG | (uint64_t)((i) + 1))
#define MOCK_CONNECT_TIMEOUT_MS 10000u

enum { MSG_TIMESTAMP = 0x1B, MSG_NEW_INCOMING = 0x13, MSG_DISCONNECT = 0x15, MSG_LOST = 0x16,
       MSG_SESSION = 0x83, MSG_LOADED = 0x84, MSG_STATE = 0x86, MSG_LEVEL_LOAD = 0x89, MSG_PROFILE = 0x8E, SLOT_READY = 5,
       MSG_KEEPALIVE = 0xFE };

// The host shows "Experiencing some network trouble" (string 1326, 0x4930E0) while
// 0x457ED0 is true: Session 0x4B2A00 returns the longest time since any remote slot's last
// packet (recvTime[slot], stamped for EVERY received packet at 0x45721C, before dispatch),
// and >= 6.0 s (0x8CFA94) counts as trouble. Real clients stream gameplay traffic; mocks
// would go quiet once loaded, so a joined mock sends a 1-byte MSG_KEEPALIVE this often.
// 0xFE has no handler in the game's table (0xB9BAD8; registered ids are 0x10..0x16 and
// 0x82..0x9B), so the host only stamps the time and drops it.
#define MOCK_KEEPALIVE_MS 1000u

// How a mock's "ready" net variable is set.
typedef enum {
    READY_FOLLOW,           // copies the host's: you ready up, everyone is ready
    READY_AUTO,             // ready as soon as it joins
    READY_MANUAL            // only via mockbrain_set_ready (`mock ready <i>`)
} MockReadyMode;

#define MOCK_PROFILE_MAX 4096
#define MOCK_OUTBOX 16
#define MOCK_MSG_MAX (NV_MAX_VALUE + 8)

typedef enum { MS_WAIT, MS_CONNECTING, MS_JOINED, MS_DROPPED } MockState;

typedef struct MockSlotView {
    int index;              // -1 if no slot holds the GUID
    int state;              // slot+0x10
    uint8_t joined, ack, playerjoin;   // slot+0x14, +0x15, +0x16
} MockSlotView;

typedef struct MockEnv {
    void *ctx;
    uint32_t now;                       // ms, monotonic (wraps after 49 days; differences only)
    int hosting;                        // host and a session exists
    int accepting;                      // host lets new players in ([0xBBB99C]+8)
    uintptr_t session;                  // identity of the current session; a change resets mocks
    MockSlotView (*slot)(void *ctx, uint64_t guid);
    void (*push)(void *ctx, int mock, const uint8_t *data, int len);
    void (*log)(const char *fmt, ...);  // may be NULL
} MockEnv;

typedef struct Mock {
    uint64_t guid;
    MockState st;
    uint32_t t_state;       // when st last changed (also: rejoin request time)
    uint32_t t_ack, t_ready;            // last 0x84 / 0x86 push
    uint32_t t_slot;        // when the slot state last changed
    int slot_state;         // last seen slot state (-1 none)
    int slot;               // last seen slot index (-1 none)
    uint32_t pushed;        // packets injected as this mock
    int sent_connect;       // 0x13 pushed for the current session
    const char *why;        // reason for the last drop (static string)
    int want_vars, want_profile;        // host sent us 0x87 / 0x8e: answer with ours
    int want_load;          // host sent 0x89 (load level): ack it so the host starts loading
    int ready;              // our "ready" net variable as last sent (-1 never sent)
    int manual;             // set by mockbrain_set_ready: stop following the host
    uint32_t t_vars;        // last 0x87 push
    uint32_t t_alive;       // last MSG_KEEPALIVE push
    uint32_t mirrored;      // host 0x88 changes repeated as ours
} Mock;

typedef struct MockBrain {
    int count;              // mocks (party_size - 1)
    uint32_t join_delay, settle;
    uintptr_t session;
    uint32_t t_hosting;     // 0 = not hosting
    MockReadyMode ready_mode;
    int mirror_profile;     // answer 0x8e with a copy of the host's
    uint32_t key_ready;     // nv_hash("ready")
    NvTable host_vars;      // the host's net variables as it sent them (0x87, then 0x88)
    int host_ready;         // the host's "ready" (-1 unknown)
    uint8_t host_profile[MOCK_PROFILE_MAX];
    int host_profile_len;
    struct { uint16_t mask; uint16_t len; uint8_t d[MOCK_MSG_MAX]; } outbox[MOCK_OUTBOX];
    int outbox_n;           // host 0x88 changes waiting to be repeated by mocks in .mask
    uint32_t outbox_lost;
    uint32_t host_vars_bad;             // host 0x87 messages that did not parse
    Mock m[MOCK_MAX];
} MockBrain;

void mockbrain_init(MockBrain *b, int count, uint32_t join_delay_ms, uint32_t settle_ms);
void mockbrain_tick(MockBrain *b, const MockEnv *e);
int mockbrain_index(const MockBrain *b, uint64_t guid);      // mock index or -1
int mockbrain_connected(const MockBrain *b);                  // mocks the host should see as connected
int mockbrain_is_connected(const MockBrain *b, int i);
// Mock i leaves: pushes 0x15 (or 0x16 if lost) and stops counting as a connection.
int mockbrain_drop(MockBrain *b, const MockEnv *e, int i, int lost, const char *why);
// The host closed the connection itself (RakPeer::CloseConnection): no notification.
void mockbrain_closed(MockBrain *b, int i, uint32_t now);
// A dropped mock joins again join_delay ms from now.
int mockbrain_rejoin(MockBrain *b, int i, uint32_t now);
// What the host sent to the mocks in `mask` (bit i = mock i), as handed to RakPeer::Send.
void mockbrain_host_msg(MockBrain *b, uint16_t mask, const uint8_t *data, int len);
// Mock i sets its "ready" net variable (and slot state 5 when ready); stops following the
// host. Returns 0 if it is not joined.
int mockbrain_set_ready(MockBrain *b, const MockEnv *e, int i, int ready);
const char *mockbrain_ready_mode_name(MockReadyMode m);
const char *mockbrain_state_name(MockState s);
// Message id of a RakNet payload, skipping an ID_TIMESTAMP header (1 + 8 bytes). -1 if empty.
int mockbrain_msg_id(const uint8_t *data, int len);

#endif

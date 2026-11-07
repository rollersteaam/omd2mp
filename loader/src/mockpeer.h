// mockpeer.h - mock players for testing N-player co-op alone ([mpdebug] in omd2mp.ini).
//
// When enabled, the host's RakPeer gets up to party_size-1 extra "connections" that exist
// only in this DLL. Hooks on RakPeer's vtable (0x83375C) make them visible to the game:
//   NumberOfConnections   adds the connected mocks (Net_IsMultiplayer etc.)
//   Send (both overloads) messages to a mock are recorded and dropped; broadcasts are
//                         recorded for every mock, then sent to real peers as usual
//   Receive               runs the mock brain (mockbrain.c) once per drain, which pushes
//                         packets "from" the mocks into the receive queue
//   GetConnectionState / GetAveragePing / GetLastPing / GetLowestPing / CloseConnection
// Each mock copies the host's net variables (0x87/0x88) and profile (0x8e), so it shows up
// in the lobby as a second copy of the host, and sets its own "ready" variable.
// Mocks have no hero and send no gameplay input: they exercise the lobby, session slots,
// per-player arrays and every message the host sends, which is where >2-player bugs live.
#ifndef OMD2MP_MOCKPEER_H
#define OMD2MP_MOCKPEER_H

typedef struct MockConfig {
    int party_size;         // total players including the host (2..players)
    int join_delay_ms;      // mock i connects i*join_delay_ms after hosting starts
    int settle_ms;          // how long a slot may sit un-acked / not ready before a mock re-sends
    int slot_base;          // Session offset of slots[] (0x38 vanilla, 0x36068 patched)
    int slot_count;         // slots in the session (2 vanilla, players patched)
    int ready_mode;         // MockReadyMode: 0 follow the host, 1 auto, 2 manual
    int mirror_profile;     // answer the host's 0x8e profile with a copy of it
    void (*log)(const char *fmt, ...);
} MockConfig;

int mock_start(const MockConfig *cfg);        // 1 if the hooks are in
int mock_enabled(void);
// `mock ...` dev-console command; writes one JSON object to out. Main thread only.
void mock_command(int argc, const char *const *argv, char *out, int cap);
// Logs the last n messages the host sent to mocks (for crash reports; takes no locks).
void mock_log_recent(int n);

#endif

// nettrace.h - network trace for diagnosing real multiplayer sessions ([nettrace] in omd2mp.ini).
//
// Hooks RakPeer's vtable (0x83375C) Send (both overloads) and Receive, chaining whatever is
// installed there already (the mock players, if on), and writes omd2mp-net.log:
//   - every message sent / received: time, direction, message ID, length, peer GUID, the
//     first bytes. Each (direction, ID) logs its first `full` messages, then every `every`th.
//   - a Session snapshot whenever it changes: is_host, connection count, localSlot,
//     playerCount, and every slot (guid, state, flags) in both the vanilla array (+0x38)
//     and the relocated one (+0x36068).
//   - a per-ID message count summary every `summary_s` seconds.
// Works on host and client and on an unpatched game. Read-only: it never alters a message.
#ifndef OMD2MP_NETTRACE_H
#define OMD2MP_NETTRACE_H

typedef struct NetTraceConfig {
    char path[260];         // log file
    int full;               // messages logged in full per (direction, ID) before sampling
    int every;              // after that, log every Nth
    int summary_s;          // count summary period, seconds (0 = off)
    int snapshot_ms;        // how often the Session is checked for changes
    int slot_count;         // slots in the relocated array (players), or 0 if slots unpatched
    int slot_new_base;      // Session offset of the relocated array
    void (*log)(const char *fmt, ...);
} NetTraceConfig;

int nettrace_start(const NetTraceConfig *cfg);    // 1 if the hooks are in

#endif

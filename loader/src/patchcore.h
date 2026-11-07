// patchcore.h - platform-independent patch engine.
//
// Nothing in here touches process memory or Windows APIs, so it is compiled into both
// the loader DLL and the host-side unit tests (tests/c/test_patchcore.c).
//
// Flow used by the loader:
//   1. omd2mp_check()        every enabled patch's original bytes match  (all-or-nothing)
//   2. omd2mp_render_cave()  build each trampoline's cave for a chosen cave address
//   3. omd2mp_render_site()  build the bytes to write over the original site
#pragma once
#include "patchset.h"

#define OMD2MP_MAX_PLAYERS 16
#define OMD2MP_MAX_SITE 16
#define OMD2MP_MAX_CAVE 64

typedef enum {
    PC_OK = 0,
    PC_MISMATCH,        // original bytes differ (other build / other mod)
    PC_RANGE,           // value does not fit the immediate's size
    PC_BADPATCH,        // malformed patch record
    PC_NOHOOK,          // fixup references an unknown C hook
} PcResult;

typedef double (*PcEval)(int expr, int N);
typedef uint32_t (*PcHookAddr)(int hook);   // address of a C hook, 0 if unknown

const char *omd2mp_result_str(PcResult r);

// Is the group enabled in a comma-separated list such as "lobby,mesh,slots"?
int omd2mp_group_enabled(const char *list, const char *group);

// Compare `cur` (bytes currently at p->va) with the expected original bytes.
PcResult omd2mp_check(const Patch *p, const uint8_t *cur);

// Encode `value` as a little-endian immediate of `size` bytes (1 = signed int8, 4 = int32/float).
PcResult omd2mp_encode(PatchKind kind, double value, int size, uint8_t *out);

// Build the cave for a trampoline placed at `cave_va`. `out` needs p->cave_len bytes.
PcResult omd2mp_render_cave(const Patch *p, int N, uint32_t cave_va, PcEval eval, PcHookAddr hook, uint8_t *out);

// Build the new site bytes (p->orig_len of them) for N players.
// For trampolines this is `jmp cave_va` padded with int3 (never executed).
PcResult omd2mp_render_site(const Patch *p, int N, uint32_t cave_va, PcEval eval, uint8_t *out);

// Session slot replication used by the slots hook: copy the reset template slot at
// `old_base` into slots[0..N) at `new_base`. `session` is the raw Session bytes.
void omd2mp_replicate_slots_impl(uint8_t *session, uint32_t old_base, uint32_t new_base,
                                 uint32_t slot_size, int N);

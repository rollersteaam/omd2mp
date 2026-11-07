// patchset.h - data model for generated patches (see tools/patchgen/build_patches.py).
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef enum { PK_IMM = 0, PK_FLOAT = 1, PK_TRAMP = 2 } PatchKind;

// A rel32 inside a cave that must point at `target` (absolute VA in the game),
// or at a C hook when `hook` != 0 (see OMD2MP_HOOK_*).
typedef struct { uint16_t off; uint32_t target; uint8_t hook; } Fixup;

// An immediate inside a cave whose value depends on the player count.
// isfloat: encode as a 32-bit IEEE float instead of an integer (size must be 4).
typedef struct { uint16_t cave_off; uint8_t size; int8_t expr; uint8_t isfloat; } CaveImm;

typedef struct {
    const char *id;
    const char *group;          // "lobby", "mesh", "slots", "economy", "netfix"
    PatchKind kind;
    uint32_t va;                // site address in the game image
    uint8_t orig_len;           // bytes at `va` that must match before patching
    const uint8_t *orig;
    uint8_t off, size;          // PK_IMM / PK_FLOAT: where the value lives inside orig
    int8_t expr;                // index for omd2mp_eval(), -1 for trampolines
    uint16_t cave_len;          // PK_TRAMP: code to place in executable memory
    const uint8_t *cave;
    uint8_t nfix;
    const Fixup *fix;
    uint8_t nimm;
    const CaveImm *imms;
} Patch;

#define OMD2MP_HOOK_REPLICATE_SLOTS 1
#define OMD2MP_HOOK_SESSION_INIT 2      // replicate slots + zero the relocated u64 per-slot arrays

// Addresses only known at run time (storage the loader owns). Patch expressions name them
// as SYM_*; the loader (and the unit tests) define omd2mp_symbol().
#define OMD2MP_SYM_NETVARS 1            // NetVars[OMD2MP_MAX_PLAYERS], replaces the game's NetVars[2]
#define OMD2MP_SYM_HEROES 2             // Hero*[OMD2MP_MAX_PLAYERS], replaces the game's heroByPlayer[2] (0xBA4468)
uint32_t omd2mp_symbol(int id);

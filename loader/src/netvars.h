// netvars.h - the game's per-player "net variables" (Net_VariableGet*/Set* in the UI) on the wire.
//
// Pure C so tests/c/test_netvars.c can check it. OrcsMustDie2.exe 0x51DD91A9:
//   key      = hash of the name the UI script passes (0x40E840, below), e.g. "ready"
//   storage  = one 0x100C-byte table per player (0xBE7D90 + slot*0x100C; the local player
//              uses 0xBE9DA8; the slots group relocates the remote ones)
//   0x87     = the sender's whole table (handler 0x501CA0 -> clear + 0x501900):
//              repeat { u32 key (big-endian); if key == 0 stop; u8 len; len bytes }
//   0x88     = one change (handler 0x501CE0 -> 0x501BB0):
//              u32 key (big-endian); 1 bit "has value"; if set: u8 len; len bytes.
//              The bit makes everything after it unaligned (RakNet packs bits MSB first).
// A bool is one byte (Net_VariableSetBool 0x502310 stores &byte, 1).
// Every message below includes its leading id byte, the way RakNet hands it to the game.
#ifndef OMD2MP_NETVARS_H
#define OMD2MP_NETVARS_H
#include <stdint.h>

#define MSG_NETVARS_ALL 0x87
#define MSG_NETVARS_SET 0x88
#define NV_MAX_ENTRIES 96
#define NV_MAX_VALUE 255

typedef struct NvEntry { uint32_t key; uint8_t len; uint8_t val[NV_MAX_VALUE]; } NvEntry;
typedef struct NvTable { int n; NvEntry e[NV_MAX_ENTRIES]; } NvTable;

uint32_t nv_hash(const char *name);

void nv_clear(NvTable *t);
const NvEntry *nv_get(const NvTable *t, uint32_t key);
// len 0 / val NULL removes the key. Returns 0 if the table is full.
int nv_set(NvTable *t, uint32_t key, const uint8_t *val, int len);

// Parse a 0x87 message into t (replacing its contents). 0 if malformed.
int nv_parse_all(NvTable *t, const uint8_t *msg, int len);
// Build a 0x87 message from t. Returns the byte length, or 0 if cap is too small.
int nv_build_all(const NvTable *t, uint8_t *out, int cap);
// Parse a 0x88 message. *has = 0 means the key was removed. 0 if malformed.
int nv_parse_set(const uint8_t *msg, int len, uint32_t *key, int *has, uint8_t *val, int *vlen);
// Build a 0x88 message; val NULL / vlen 0 with has=0 removes the key. Returns the byte length.
int nv_build_set(uint32_t key, int has, const uint8_t *val, int vlen, uint8_t *out, int cap);

#endif

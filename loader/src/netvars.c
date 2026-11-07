// netvars.c - see netvars.h.
#include <string.h>
#include "netvars.h"

// 0x40E840: an ELF-style hash with an arithmetic shift (sar), over signed chars.
uint32_t nv_hash(const char *s) {
    uint32_t h = 0;
    if (!s) return 0;
    for (; *s; s++) {
        uint32_t g;
        h = (h << 4) + (uint32_t)(int32_t)(signed char)*s;
        g = h & 0xF0000000u;
        if (g) h ^= (uint32_t)((int32_t)g >> 24) ^ g;
    }
    return h;
}

void nv_clear(NvTable *t) { t->n = 0; }

const NvEntry *nv_get(const NvTable *t, uint32_t key) {
    int i;
    for (i = 0; i < t->n; i++)
        if (t->e[i].key == key) return &t->e[i];
    return 0;
}

int nv_set(NvTable *t, uint32_t key, const uint8_t *val, int len) {
    int i;
    if (len < 0 || len > NV_MAX_VALUE || key == 0) return 0;
    for (i = 0; i < t->n && t->e[i].key != key; i++) {}
    if (!val || !len) {                                   // remove
        if (i < t->n) { t->e[i] = t->e[t->n - 1]; t->n--; }
        return 1;
    }
    if (i == t->n) {
        if (t->n >= NV_MAX_ENTRIES) return 0;
        t->n++;
    }
    t->e[i].key = key;
    t->e[i].len = (uint8_t)len;
    memcpy(t->e[i].val, val, len);
    return 1;
}

// ---------------------------------------------------------------- bits, MSB first (RakNet)
typedef struct Bits { uint8_t *p; int cap_bits, at; } Bits;

static int put_bit(Bits *b, int v) {
    if (b->at >= b->cap_bits) return 0;
    if (v) b->p[b->at >> 3] |= (uint8_t)(0x80 >> (b->at & 7));
    else b->p[b->at >> 3] &= (uint8_t)~(0x80 >> (b->at & 7));
    b->at++;
    return 1;
}
static int put_byte(Bits *b, uint8_t v) {
    int i;
    for (i = 7; i >= 0; i--) if (!put_bit(b, (v >> i) & 1)) return 0;
    return 1;
}
static int put_u32be(Bits *b, uint32_t v) {
    return put_byte(b, (uint8_t)(v >> 24)) && put_byte(b, (uint8_t)(v >> 16)) &&
           put_byte(b, (uint8_t)(v >> 8)) && put_byte(b, (uint8_t)v);
}

typedef struct RBits { const uint8_t *p; int len_bits, at; } RBits;
static int get_bit(RBits *b, int *v) {
    if (b->at >= b->len_bits) return 0;
    *v = (b->p[b->at >> 3] >> (7 - (b->at & 7))) & 1;
    b->at++;
    return 1;
}
static int get_byte(RBits *b, uint8_t *v) {
    int i, bit;
    *v = 0;
    for (i = 0; i < 8; i++) {
        if (!get_bit(b, &bit)) return 0;
        *v = (uint8_t)((*v << 1) | bit);
    }
    return 1;
}
static int get_u32be(RBits *b, uint32_t *v) {
    uint8_t x[4];
    if (!get_byte(b, &x[0]) || !get_byte(b, &x[1]) || !get_byte(b, &x[2]) || !get_byte(b, &x[3])) return 0;
    *v = (uint32_t)x[0] << 24 | (uint32_t)x[1] << 16 | (uint32_t)x[2] << 8 | x[3];
    return 1;
}

// ---------------------------------------------------------------- messages
int nv_parse_all(NvTable *t, const uint8_t *msg, int len) {
    RBits b = { msg, len * 8, 8 };
    nv_clear(t);
    if (!msg || len < 1 || msg[0] != MSG_NETVARS_ALL) return 0;
    for (;;) {
        uint32_t key;
        uint8_t n, v[NV_MAX_VALUE];
        int i;
        if (!get_u32be(&b, &key)) return 0;
        if (!key) return 1;
        if (!get_byte(&b, &n)) return 0;
        for (i = 0; i < n; i++) if (!get_byte(&b, &v[i])) return 0;
        if (!nv_set(t, key, v, n) && n) return 0;
    }
}

int nv_build_all(const NvTable *t, uint8_t *out, int cap) {
    Bits b = { out, cap * 8, 0 };
    int i, j;
    if (!put_byte(&b, MSG_NETVARS_ALL)) return 0;
    for (i = 0; i < t->n; i++) {
        if (!put_u32be(&b, t->e[i].key) || !put_byte(&b, t->e[i].len)) return 0;
        for (j = 0; j < t->e[i].len; j++) if (!put_byte(&b, t->e[i].val[j])) return 0;
    }
    if (!put_u32be(&b, 0)) return 0;
    return (b.at + 7) / 8;
}

int nv_parse_set(const uint8_t *msg, int len, uint32_t *key, int *has, uint8_t *val, int *vlen) {
    RBits b = { msg, len * 8, 8 };
    uint8_t n;
    int i;
    *vlen = 0;
    if (!msg || len < 1 || msg[0] != MSG_NETVARS_SET) return 0;
    if (!get_u32be(&b, key) || !get_bit(&b, has)) return 0;
    if (!*has) return 1;
    if (!get_byte(&b, &n)) return 0;
    for (i = 0; i < n; i++) if (!get_byte(&b, &val[i])) return 0;
    *vlen = n;
    return 1;
}

int nv_build_set(uint32_t key, int has, const uint8_t *val, int vlen, uint8_t *out, int cap) {
    Bits b = { out, cap * 8, 0 };
    int i;
    if (vlen < 0 || vlen > NV_MAX_VALUE) return 0;
    if (has && (!val || !vlen)) has = 0;
    if (!put_byte(&b, MSG_NETVARS_SET) || !put_u32be(&b, key) || !put_bit(&b, has)) return 0;
    if (has) {
        if (!put_byte(&b, (uint8_t)vlen)) return 0;
        for (i = 0; i < vlen; i++) if (!put_byte(&b, val[i])) return 0;
    }
    while (b.at & 7) if (!put_bit(&b, 0)) return 0;
    return b.at / 8;
}

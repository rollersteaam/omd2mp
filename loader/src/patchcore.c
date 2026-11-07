// patchcore.c - see patchcore.h. Pure C99, no OS calls.
#include "patchcore.h"
#include <string.h>
#include <math.h>

const char *omd2mp_result_str(PcResult r) {
    switch (r) {
    case PC_OK: return "ok";
    case PC_MISMATCH: return "original bytes differ";
    case PC_RANGE: return "value out of range";
    case PC_BADPATCH: return "malformed patch";
    case PC_NOHOOK: return "unknown hook";
    }
    return "?";
}

int omd2mp_group_enabled(const char *list, const char *group) {
    size_t g = strlen(group);
    const char *s = list;
    while (s && *s) {
        while (*s == ' ' || *s == ',') s++;
        const char *e = s;
        while (*e && *e != ',' && *e != ' ') e++;
        if ((size_t)(e - s) == g && strncmp(s, group, g) == 0) return 1;
        s = e;
    }
    return 0;
}

PcResult omd2mp_check(const Patch *p, const uint8_t *cur) {
    if (!p->orig || p->orig_len == 0) return PC_BADPATCH;
    return memcmp(cur, p->orig, p->orig_len) == 0 ? PC_OK : PC_MISMATCH;
}

static void put_u32(uint8_t *o, uint32_t v) {
    o[0] = (uint8_t)v; o[1] = (uint8_t)(v >> 8); o[2] = (uint8_t)(v >> 16); o[3] = (uint8_t)(v >> 24);
}

PcResult omd2mp_encode(PatchKind kind, double value, int size, uint8_t *out) {
    if (kind == PK_FLOAT) {
        if (size != 4) return PC_BADPATCH;
        float f = (float)value;
        uint32_t bits;
        memcpy(&bits, &f, 4);
        put_u32(out, bits);
        return PC_OK;
    }
    double r = floor(value + 0.5);
    if (r != value) return PC_RANGE;               // immediates must be whole numbers
    if (size == 1) {
        if (r < -128 || r > 127) return PC_RANGE;  // imm8 is sign-extended by push/cmp
        out[0] = (uint8_t)(int8_t)r;
        return PC_OK;
    }
    if (size == 4) {
        if (r < -2147483648.0 || r > 4294967295.0) return PC_RANGE;
        put_u32(out, r < 0 ? (uint32_t)(int32_t)r : (uint32_t)r);
        return PC_OK;
    }
    return PC_BADPATCH;
}

PcResult omd2mp_render_cave(const Patch *p, int N, uint32_t cave_va, PcEval eval, PcHookAddr hook, uint8_t *out) {
    if (p->kind != PK_TRAMP || !p->cave || p->cave_len > OMD2MP_MAX_CAVE) return PC_BADPATCH;
    memcpy(out, p->cave, p->cave_len);
    for (int i = 0; i < p->nfix; i++) {
        const Fixup *f = &p->fix[i];
        if (f->off + 4u > p->cave_len) return PC_BADPATCH;
        uint32_t target = f->target;
        if (f->hook) {
            target = hook ? hook(f->hook) : 0;
            if (!target) return PC_NOHOOK;
        }
        put_u32(out + f->off, target - (cave_va + f->off + 4u));   // rel32 from end of field
    }
    for (int i = 0; i < p->nimm; i++) {
        const CaveImm *c = &p->imms[i];
        if (c->cave_off + (unsigned)c->size > p->cave_len) return PC_BADPATCH;
        PcResult r = omd2mp_encode(c->isfloat ? PK_FLOAT : PK_IMM, eval(c->expr, N), c->size, out + c->cave_off);
        if (r) return r;
    }
    return PC_OK;
}

PcResult omd2mp_render_site(const Patch *p, int N, uint32_t cave_va, PcEval eval, uint8_t *out) {
    if (p->orig_len > OMD2MP_MAX_SITE) return PC_BADPATCH;
    memcpy(out, p->orig, p->orig_len);
    if (p->kind == PK_TRAMP) {
        if (p->orig_len < 5) return PC_BADPATCH;
        out[0] = 0xE9;
        put_u32(out + 1, cave_va - (p->va + 5u));
        for (int i = 5; i < p->orig_len; i++) out[i] = 0xCC;
        return PC_OK;
    }
    if (p->off + (unsigned)p->size > p->orig_len) return PC_BADPATCH;
    return omd2mp_encode(p->kind, eval(p->expr, N), p->size, out + p->off);
}

void omd2mp_replicate_slots_impl(uint8_t *session, uint32_t old_base, uint32_t new_base,
                                 uint32_t slot_size, int N) {
    for (int i = 0; i < N; i++)
        memcpy(session + new_base + (uint32_t)i * slot_size, session + old_base, slot_size);
}

// test_patchcore.c - host-side unit tests for loader/src/patchcore.c.
//
// Two kinds of test:
//   * hand-written synthetic patches with known answers (encoding, rel32 math, aborts)
//   * property tests over the generated table in patchset_gen.h, using only the data
//     in that table (no game files): every record renders for N=2..8, rendering at
//     N=2 reproduces the vanilla bytes, and rel32 fields land where they should.
#include <stdio.h>
#include <string.h>
#include "patchcore.h"
#include "patchset_gen.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static double eval_test(int expr, int N) { return expr == 0 ? N : expr == 1 ? N - 1 : 1.0 / N; }
static uint32_t hook_test(int h) { return h == 1 ? 0x10000000u : h == 2 ? 0x10000100u : 0; }
// stands in for the loader's NetVars[16] (dllmain.c)
uint32_t omd2mp_symbol(int id) { return id == OMD2MP_SYM_NETVARS ? 0x10200000u : id == OMD2MP_SYM_HEROES ? 0x10300000u : 0; }
static uint32_t hook_none(int h) { (void)h; return 0; }

static void test_groups(void) {
    CHECK(omd2mp_group_enabled("lobby,mesh,slots", "mesh"));
    CHECK(omd2mp_group_enabled(" lobby , slots", "slots"));
    CHECK(!omd2mp_group_enabled("lobby,mesh", "me"));
    CHECK(!omd2mp_group_enabled("lobbyx", "lobby"));
    CHECK(!omd2mp_group_enabled("", "lobby"));
}

static void test_encode(void) {
    uint8_t b[4];
    CHECK(omd2mp_encode(PK_IMM, 3, 1, b) == PC_OK && b[0] == 3);
    CHECK(omd2mp_encode(PK_IMM, -1, 1, b) == PC_OK && b[0] == 0xff);
    CHECK(omd2mp_encode(PK_IMM, 128, 1, b) == PC_RANGE);
    CHECK(omd2mp_encode(PK_IMM, 2.5, 4, b) == PC_RANGE);
    CHECK(omd2mp_encode(PK_IMM, -0xa8, 4, b) == PC_OK && rd32(b) == 0xffffff58u);
    CHECK(omd2mp_encode(PK_IMM, 0x36068 + 4 * 0x18, 4, b) == PC_OK && rd32(b) == 0x360c8u);
    CHECK(omd2mp_encode(PK_FLOAT, 0.25, 4, b) == PC_OK && rd32(b) == 0x3e800000u);
    CHECK(omd2mp_encode(PK_IMM, 1, 2, b) == PC_BADPATCH);
}

static void test_imm_site(void) {
    static const uint8_t orig[] = {0x6a, 0x01};               // push 1
    Patch p = {"t.push", "lobby", PK_IMM, 0x456f1e, 2, orig, 1, 1, 1, 0, NULL, 0, NULL, 0, NULL};
    uint8_t mem[2] = {0x6a, 0x01}, out[16];
    CHECK(omd2mp_check(&p, mem) == PC_OK);
    mem[1] = 7;
    CHECK(omd2mp_check(&p, mem) == PC_MISMATCH);
    CHECK(omd2mp_render_site(&p, 4, 0, eval_test, out) == PC_OK && out[0] == 0x6a && out[1] == 3);
    CHECK(omd2mp_render_site(&p, 200, 0, eval_test, out) == PC_RANGE);
}

static void test_tramp(void) {
    // site: push ebx ; call 0x605ee0  (6 bytes at 0x45795d)
    static const uint8_t orig[] = {0x53, 0xe8, 0x7d, 0xe5, 0x1a, 0x00};
    // cave: push 1 ; call <target> ; jmp back
    static const uint8_t cave[] = {0x6a, 0x01, 0xe8, 0, 0, 0, 0, 0xe9, 0, 0, 0, 0};
    static const Fixup fx[] = {{3, 0x605ee0, 0}, {8, 0x457963, 0}};
    Patch p = {"t.tramp", "mesh", PK_TRAMP, 0x45795d, 6, orig, 0, 0, -1, sizeof cave, cave, 2, fx, 0, NULL};
    uint8_t c[64], s[16];
    uint32_t cave_va = 0x20000000;
    CHECK(omd2mp_render_cave(&p, 4, cave_va, eval_test, hook_none, c) == PC_OK);
    CHECK(cave_va + 7 + rd32(c + 3) == 0x605ee0);              // call lands on original callee
    CHECK(cave_va + 12 + rd32(c + 8) == 0x457963);             // jmp back lands after the stolen bytes
    CHECK(omd2mp_render_site(&p, 4, cave_va, eval_test, s) == PC_OK);
    CHECK(s[0] == 0xe9 && 0x45795d + 5 + rd32(s + 1) == cave_va && s[5] == 0xcc);
}

static void test_hook_and_cave_imm(void) {
    static const uint8_t orig[] = {0x8d, 0x73, 0x38, 0xbf, 0x01, 0x00, 0x00, 0x00};
    static const uint8_t cave[] = {0x60, 0x9c, 0x56, 0xe8, 0, 0, 0, 0, 0x83, 0xc4, 0x04, 0x9d, 0x61,
                                   0xbf, 0x01, 0, 0, 0, 0xe9, 0, 0, 0, 0};
    static const Fixup fx[] = {{4, 0, 1}, {19, 0x4b1af1, 0}};
    static const CaveImm im[] = {{14, 4, 1, 0}};               // mov edi, N-1
    Patch p = {"t.hook", "slots", PK_TRAMP, 0x4b1ae9, 8, orig, 0, 0, -1, sizeof cave, cave, 2, fx, 1, im};
    uint8_t c[64];
    CHECK(omd2mp_render_cave(&p, 8, 0x30000000, eval_test, hook_test, c) == PC_OK);
    CHECK(0x30000000u + 8 + rd32(c + 4) == 0x10000000u);
    CHECK(rd32(c + 14) == 7);
    CHECK(omd2mp_render_cave(&p, 8, 0x30000000, eval_test, hook_none, c) == PC_NOHOOK);
}

static double eval_inv(int expr, int N) { (void)expr; return 1.0 / N; }

static void test_float_cave_imm(void) {
    // economy: push eax; mov eax, <float 1/N>; movd xmm0, eax; pop eax; jmp back
    static const uint8_t orig[] = {0xf3, 0x0f, 0x10, 0x05, 0x10, 0x78, 0x85, 0x00};
    static const uint8_t cave[] = {0x50, 0xb8, 0, 0, 0, 0, 0x66, 0x0f, 0x6e, 0xc0, 0x58, 0xe9, 0, 0, 0, 0};
    static const Fixup fx[] = {{12, 0x45804a, 0}};
    static const CaveImm im[] = {{2, 4, 0, 1}};
    Patch p = {"t.float", "economy", PK_TRAMP, 0x458042, 8, orig, 0, 0, -1, sizeof cave, cave, 1, fx, 1, im};
    uint8_t c[64];
    for (int N = 2; N <= 16; N++) {
        float want = (float)(1.0 / N), got;
        uint32_t bits;
        CHECK(omd2mp_render_cave(&p, N, 0x30000000, eval_inv, hook_none, c) == PC_OK);
        bits = rd32(c + 2);
        memcpy(&got, &bits, 4);
        CHECK(got == want);
        CHECK(0x30000000u + 16 + rd32(c + 12) == 0x45804au);
    }
    CHECK(rd32(c + 2) == 0x3d800000u);                        // N=16 -> 0.0625f
}

static void test_replicate(void) {
    uint8_t s[0x200];
    memset(s, 0, sizeof s);
    for (int i = 0; i < 0x18; i++) s[0x38 + i] = (uint8_t)(i + 1);
    omd2mp_replicate_slots_impl(s, 0x38, 0x100, 0x18, 4);
    for (int k = 0; k < 4; k++) CHECK(memcmp(s + 0x100 + k * 0x18, s + 0x38, 0x18) == 0);
    CHECK(s[0x100 + 4 * 0x18] == 0);                          // no write past slots[N-1]
}

// ---------------------------------------------------------------- generated table
static void test_generated_table(void) {
    CHECK(OMD2MP_PATCH_COUNT > 100);
    for (size_t i = 0; i < OMD2MP_PATCH_COUNT; i++) {
        const Patch *p = &OMD2MP_PATCHES[i];
        uint8_t site[OMD2MP_MAX_SITE], cave[OMD2MP_MAX_CAVE];
        CHECK(omd2mp_check(p, p->orig) == PC_OK);
        for (size_t j = 0; j < i; j++) {                       // sites never overlap
            const Patch *q = &OMD2MP_PATCHES[j];
            int overlap = p->va < q->va + q->orig_len && q->va < p->va + p->orig_len;
            if (overlap) printf("overlap %s / %s\n", p->id, q->id);
            CHECK(!overlap);
        }
        for (int N = 2; N <= 8; N++) {
            uint32_t cave_va = 0x7f000000u;
            PcResult r = PC_OK;
            if (p->kind == PK_TRAMP) {
                r = omd2mp_render_cave(p, N, cave_va, omd2mp_eval, hook_test, cave);
                if (r) printf("%s N=%d cave: %s\n", p->id, N, omd2mp_result_str(r));
                CHECK(r == PC_OK);
                // the last instruction is `jmp back` to the byte after the stolen region
                CHECK(p->cave[p->cave_len - 5] == 0xe9);
                CHECK(cave_va + p->cave_len + rd32(cave + p->cave_len - 4) == p->va + p->orig_len);
            }
            r = omd2mp_render_site(p, N, cave_va, omd2mp_eval, site);
            if (r) printf("%s N=%d site: %s\n", p->id, N, omd2mp_result_str(r));
            CHECK(r == PC_OK);
            // vanilla identity: for 2 players, value patches reproduce the original bytes,
            // except the ones that change behaviour regardless of N.
            int n_independent = !strcmp(p->group, "netfix") || !strncmp(p->id, "slots.session_alloc", 19) ||
                                !strncmp(p->id, "slots.netvars_base", 18) || !strncmp(p->id, "slots.heroes", 12) ||
                                !strcmp(p->id, "lobby.force_max_incoming");   // clients: 0 -> N-1
            if (N == 2 && p->kind != PK_TRAMP && !n_independent) {
                if (memcmp(site, p->orig, p->orig_len)) printf("not vanilla at N=2: %s\n", p->id);
                CHECK(memcmp(site, p->orig, p->orig_len) == 0);
            }
            if (N == 2 && p->kind == PK_TRAMP && !n_independent) {               // cave immediates too
                for (int k = 0; k < p->nimm; k++)
                    CHECK(memcmp(cave + p->imms[k].cave_off, p->cave + p->imms[k].cave_off, p->imms[k].size) == 0);
            }
        }
    }
    // the Session allocation must cover slots[16] and the two relocated u64[16] arrays,
    // and the NetVars base sites must point at the loader's array
    int allocs = 0, netvars = 0;
    for (size_t i = 0; i < OMD2MP_PATCH_COUNT; i++) {
        const Patch *p = &OMD2MP_PATCHES[i];
        uint8_t site[OMD2MP_MAX_SITE];
        if (!strncmp(p->id, "slots.session_alloc", 19)) {
            allocs++;
            CHECK(omd2mp_render_site(p, 4, 0, omd2mp_eval, site) == PC_OK);
            CHECK(rd32(site + p->off) == OMD2MP_SESSION_SIZE);
        }
        if (!strncmp(p->id, "slots.netvars_base", 18)) {
            netvars++;
            CHECK(omd2mp_render_site(p, 4, 0, omd2mp_eval, site) == PC_OK);
            CHECK(rd32(site + p->off) == 0x10200000u);
        }
    }
    CHECK(allocs == 3 && netvars == 8);
    CHECK(OMD2MP_SLOT_NEW_BASE + OMD2MP_MAX_PLAYERS * OMD2MP_SLOT_SIZE <= OMD2MP_VOICE_TIME_NEW);
    CHECK(OMD2MP_VOICE_TIME_NEW + OMD2MP_MAX_PLAYERS * 8 <= OMD2MP_RECV_TIME_NEW);
    CHECK(OMD2MP_RECV_TIME_NEW + OMD2MP_MAX_PLAYERS * 8 <= OMD2MP_SESSION_SIZE);
}

int main(void) {
    test_groups();
    test_encode();
    test_imm_site();
    test_tramp();
    test_hook_and_cave_imm();
    test_float_cave_imm();
    test_replicate();
    test_generated_table();
    printf("%d/%d checks passed\n", checks - fails, checks);
    return fails != 0;
}

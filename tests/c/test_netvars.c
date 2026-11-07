// test_netvars.c - unit tests for loader/src/netvars.c (net-variable wire format).
#include <stdio.h>
#include <string.h>
#include "netvars.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// Straight transcription of 0x40E840 for comparison: h = (h << 4) + c; g = h & 0xF0000000;
// if (g) h ^= (g sar 24) ^ g.
static unsigned ref_hash(const char *s) {
    unsigned h = 0;
    while (*s) {
        unsigned g;
        int c = (signed char)*s++;
        h = (h << 4) + (unsigned)c;
        g = h & 0xF0000000u;
        if (g) {
            unsigned sar = (g >> 24) | ((g & 0x80000000u) ? 0xFFFFFF00u : 0);
            h ^= sar ^ g;
        }
    }
    return h;
}

static void test_hash(void) {
    CHECK(nv_hash("") == 0);
    CHECK(nv_hash(NULL) == 0);
    CHECK(nv_hash("a") == 'a');
    CHECK(nv_hash("ab") == ('a' << 4) + 'b');
    CHECK(nv_hash("ready") == 0x0078B7B9u);
    CHECK(nv_hash("ready") == ref_hash("ready"));
    CHECK(nv_hash("a_much_longer_variable_name") == ref_hash("a_much_longer_variable_name"));
    CHECK(nv_hash("\xe9t\xe9") == ref_hash("\xe9t\xe9"));     // signed chars
}

static void test_table(void) {
    static NvTable t;
    uint8_t one = 1, two[2] = { 2, 3 };
    nv_clear(&t);
    CHECK(nv_set(&t, 10, &one, 1) && t.n == 1);
    CHECK(nv_set(&t, 10, two, 2) && t.n == 1 && nv_get(&t, 10)->len == 2);
    CHECK(nv_set(&t, 11, &one, 1) && t.n == 2);
    CHECK(nv_set(&t, 10, NULL, 0) && t.n == 1 && !nv_get(&t, 10) && nv_get(&t, 11));
    CHECK(!nv_set(&t, 0, &one, 1));                   // key 0 ends a 0x87 list
}

static void test_all_roundtrip(void) {
    static NvTable a, b;
    uint8_t buf[2048], name[5] = { 'O', 0, 'K', 0, 0 };
    uint8_t r = 1;
    int n;
    nv_clear(&a);
    nv_set(&a, nv_hash("ready"), &r, 1);
    nv_set(&a, 0x12345678u, name, 5);
    n = nv_build_all(&a, buf, sizeof buf);
    CHECK(n == 1 + (4 + 1 + 1) + (4 + 1 + 5) + 4);
    CHECK(buf[0] == MSG_NETVARS_ALL);
    CHECK(buf[1] == 0x00 && buf[2] == 0x78 && buf[3] == 0xB7 && buf[4] == 0xB9);   // big-endian key
    CHECK(buf[5] == 1 && buf[6] == 1);
    CHECK(nv_parse_all(&b, buf, n) && b.n == 2);
    CHECK(nv_get(&b, 0x12345678u) && nv_get(&b, 0x12345678u)->len == 5 && !memcmp(nv_get(&b, 0x12345678u)->val, name, 5));
    CHECK(!nv_parse_all(&b, buf, n - 1));               // missing terminator
    CHECK(!nv_parse_all(&b, buf + 1, n - 1));           // wrong id
    CHECK(nv_build_all(&a, buf, 10) == 0);              // too small
    nv_clear(&a);
    CHECK(nv_build_all(&a, buf, sizeof buf) == 5);      // empty: id + terminator
    CHECK(nv_parse_all(&b, buf, 5) && b.n == 0);
}

static void test_set_bits(void) {
    uint8_t buf[300], v[NV_MAX_VALUE], one = 1;
    uint32_t key;
    int has, vlen, n;
    // ready = 1: id, key, bit 1, len 1, value 1 -> 1 + 4 bytes + 17 bits = 8 bytes
    n = nv_build_set(0x0078B7B9u, 1, &one, 1, buf, sizeof buf);
    CHECK(n == 8);
    CHECK(buf[0] == MSG_NETVARS_SET && buf[1] == 0x00 && buf[2] == 0x78 && buf[3] == 0xB7 && buf[4] == 0xB9);
    CHECK(buf[5] == 0x80);                              // has-bit, then len 0x01 starting at bit 1
    CHECK(buf[6] == 0x80);                              // len's last bit + value's first 7 bits
    CHECK(buf[7] == 0x80);                              // value 0x01's last bit
    CHECK(nv_parse_set(buf, n, &key, &has, v, &vlen) && key == 0x0078B7B9u && has && vlen == 1 && v[0] == 1);
    // removal: just the bit
    n = nv_build_set(0xABCDEF01u, 0, NULL, 0, buf, sizeof buf);
    CHECK(n == 6 && buf[5] == 0);
    CHECK(nv_parse_set(buf, n, &key, &has, v, &vlen) && key == 0xABCDEF01u && !has && vlen == 0);
    CHECK(!nv_parse_set(buf, 5, &key, &has, v, &vlen));         // no bit
    memset(v, 0xA5, sizeof v);
    n = nv_build_set(7, 1, v, NV_MAX_VALUE, buf, sizeof buf);
    CHECK(n == 1 + 4 + 1 + NV_MAX_VALUE + 1);                 // 40 + 1 + 8 + 255*8 bits, rounded up
    memset(v, 0, sizeof v);
    CHECK(nv_parse_set(buf, n, &key, &has, v, &vlen) && vlen == NV_MAX_VALUE && v[0] == 0xA5 && v[254] == 0xA5);
    CHECK(!nv_parse_set(buf, n - 1, &key, &has, v, &vlen));
}

int main(void) {
    test_hash();
    test_table();
    test_all_roundtrip();
    test_set_bits();
    printf("netvars: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}

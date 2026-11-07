// test_devproto.c - host-side unit tests for loader/src/devproto.c (the dev console's
// line protocol: tokenising, token -> value, value -> JSON, hex). Portable: no game, no Windows.
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "devproto.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void test_gval_layout(void) {
    // The game reads args at +8 with stride 16; this only holds on 32-bit builds.
    if (sizeof(void *) == 4) {
        CHECK(sizeof(GVal) == 16);
        CHECK(offsetof(GVal, type) == 4);
        CHECK(offsetof(GVal, v) == 8);
    }
}

static void test_split(void) {
    DevTok t[DEV_MAX_ARGS];
    char a[] = "  call Player_GetCash 0  ";
    CHECK(dev_split(a, t, DEV_MAX_ARGS) == 3);
    CHECK(!strcmp(t[0].s, "call") && !strcmp(t[1].s, "Player_GetCash") && !strcmp(t[2].s, "0"));
    CHECK(!t[0].quoted && !t[2].quoted);

    char b[] = "log \"hello world\" x";
    CHECK(dev_split(b, t, DEV_MAX_ARGS) == 3);
    CHECK(!strcmp(t[1].s, "hello world") && t[1].quoted);
    CHECK(!strcmp(t[2].s, "x"));

    char c[] = "log \"a\\\"b\\\\c\\nd\"";
    CHECK(dev_split(c, t, DEV_MAX_ARGS) == 2);
    CHECK(!strcmp(t[1].s, "a\"b\\c\nd"));

    char d[] = "log \"\"";
    CHECK(dev_split(d, t, DEV_MAX_ARGS) == 2 && t[1].quoted && t[1].s[0] == 0);

    char e[] = "log \"unterminated";
    CHECK(dev_split(e, t, DEV_MAX_ARGS) == -1);
    char f[] = "log \"ab\"cd";
    CHECK(dev_split(f, t, DEV_MAX_ARGS) == -1);

    char g[] = "a b c";
    CHECK(dev_split(g, t, 2) == -2);
    char h[] = "a b";
    CHECK(dev_split(h, t, 2) == 2);

    char i[] = "   \t\r\n";
    CHECK(dev_split(i, t, DEV_MAX_ARGS) == 0);
    char j[] = "ping\r\n";
    CHECK(dev_split(j, t, DEV_MAX_ARGS) == 1 && !strcmp(t[0].s, "ping"));
    char k[] = "x\ty";
    CHECK(dev_split(k, t, DEV_MAX_ARGS) == 2 && !strcmp(t[1].s, "y"));
}

static void tok(DevTok *t, char *s, int q) { t->s = s; t->quoted = q; }

static void test_token_gval(void) {
    DevTok t; GVal v;
    char s1[] = "true";  tok(&t, s1, 0); dev_token_gval(&t, &v); CHECK(v.type == GV_BOOL && v.v.b == 1);
    char s2[] = "false"; tok(&t, s2, 0); dev_token_gval(&t, &v); CHECK(v.type == GV_BOOL && v.v.b == 0);
    char s3[] = "null";  tok(&t, s3, 0); dev_token_gval(&t, &v); CHECK(v.type == GV_NULL);
    char s4[] = "undefined"; tok(&t, s4, 0); dev_token_gval(&t, &v); CHECK(v.type == GV_UNDEFINED);
    char s5[] = "3";     tok(&t, s5, 0); dev_token_gval(&t, &v); CHECK(v.type == GV_NUMBER && v.v.n == 3.0);
    char s6[] = "-2.5";  tok(&t, s6, 0); dev_token_gval(&t, &v); CHECK(v.type == GV_NUMBER && v.v.n == -2.5);
    char s7[] = "1e3";   tok(&t, s7, 0); dev_token_gval(&t, &v); CHECK(v.type == GV_NUMBER && v.v.n == 1000.0);
    char s8[] = "3abc";  tok(&t, s8, 0); dev_token_gval(&t, &v); CHECK(v.type == GV_STRING && !strcmp(v.v.s, "3abc"));
    char s9[] = "hello"; tok(&t, s9, 0); dev_token_gval(&t, &v); CHECK(v.type == GV_STRING && v.v.s == s9);
    char sa[] = "3";     tok(&t, sa, 1); dev_token_gval(&t, &v); CHECK(v.type == GV_STRING && !strcmp(v.v.s, "3"));
    char sb[] = "true";  tok(&t, sb, 1); dev_token_gval(&t, &v); CHECK(v.type == GV_STRING);
    char sc[] = "";      tok(&t, sc, 1); dev_token_gval(&t, &v); CHECK(v.type == GV_STRING && v.v.s[0] == 0);
    CHECK(v.iface == 0);
}

static void test_u32(void) {
    uint32_t x = 7;
    CHECK(dev_parse_u32("0", &x) && x == 0);
    CHECK(dev_parse_u32("1234", &x) && x == 1234);
    CHECK(dev_parse_u32("0xB9D108", &x) && x == 0xB9D108);
    CHECK(dev_parse_u32("0xffffffff", &x) && x == 0xffffffffu);
    CHECK(!dev_parse_u32("", &x));
    CHECK(!dev_parse_u32(0, &x));
    CHECK(!dev_parse_u32("-1", &x));
    CHECK(!dev_parse_u32("+1", &x));
    CHECK(!dev_parse_u32("12z", &x));
    CHECK(!dev_parse_u32("0x", &x) || x == 0);   // strtoul quirk; must not crash
    if (sizeof(unsigned long) > 4) CHECK(!dev_parse_u32("0x100000000", &x));
}

static void test_json_str(void) {
    char o[64];
    CHECK(dev_json_str(o, sizeof o, "abc") == 5 && !strcmp(o, "\"abc\""));
    CHECK(dev_json_str(o, sizeof o, "a\"b\\c") > 0 && !strcmp(o, "\"a\\\"b\\\\c\""));
    CHECK(dev_json_str(o, sizeof o, "x\ny\tz\r") > 0 && !strcmp(o, "\"x\\ny\\tz\\r\""));
    CHECK(dev_json_str(o, sizeof o, "\x01") > 0 && !strcmp(o, "\"\\u0001\""));
    CHECK(dev_json_str(o, 4, "abcdef") == -1 && o[0] == 0);
    CHECK(dev_json_str(o, 0, "a") == -1);
    CHECK(dev_json_str(o, 4, "a") == 3 && !strcmp(o, "\"a\""));   // exactly fits with NUL
    CHECK(dev_json_str(o, 3, "a") == -1);
}

static void test_json_gval(void) {
    char o[128]; GVal v;
    memset(&v, 0, sizeof v); v.type = GV_UNDEFINED;
    CHECK(dev_json_gval(o, sizeof o, &v) > 0 && !strcmp(o, "{\"type\":\"undefined\"}"));
    v.type = GV_NULL;
    CHECK(dev_json_gval(o, sizeof o, &v) > 0 && !strcmp(o, "{\"type\":\"null\"}"));
    v.type = GV_BOOL; v.v.b = 1;
    CHECK(dev_json_gval(o, sizeof o, &v) > 0 && !strcmp(o, "{\"type\":\"bool\",\"value\":true}"));
    v.v.b = 0;
    CHECK(dev_json_gval(o, sizeof o, &v) > 0 && !strcmp(o, "{\"type\":\"bool\",\"value\":false}"));
    v.type = GV_NUMBER; v.v.n = 3;
    CHECK(dev_json_gval(o, sizeof o, &v) > 0 && !strcmp(o, "{\"type\":\"number\",\"value\":3}"));
    v.v.n = 0.1;
    CHECK(dev_json_gval(o, sizeof o, &v) > 0 && !strcmp(o, "{\"type\":\"number\",\"value\":0.10000000000000001}"));
    v.v.n = NAN;
    CHECK(dev_json_gval(o, sizeof o, &v) > 0 && !strcmp(o, "{\"type\":\"number\",\"value\":null}"));
    v.v.n = INFINITY;
    CHECK(dev_json_gval(o, sizeof o, &v) > 0 && !strcmp(o, "{\"type\":\"number\",\"value\":null}"));
    v.v.n = -INFINITY;
    CHECK(dev_json_gval(o, sizeof o, &v) > 0 && !strcmp(o, "{\"type\":\"number\",\"value\":null}"));
    v.type = GV_STRING | GV_MANAGED; v.v.s = "hi \"x\"";
    CHECK(dev_json_gval(o, sizeof o, &v) > 0 && !strcmp(o, "{\"type\":\"string\",\"value\":\"hi \\\"x\\\"\"}"));
    v.type = GV_STRING; v.v.s = 0;
    CHECK(dev_json_gval(o, sizeof o, &v) > 0 && !strcmp(o, "{\"type\":\"string\",\"value\":\"\"}"));
    static const uint16_t w[] = { 'O', 'r', 'c', 0xE9, 0 };
    v.type = GV_STRINGW; v.v.ws = w;
    CHECK(dev_json_gval(o, sizeof o, &v) > 0 && !strcmp(o, "{\"type\":\"stringw\",\"value\":\"Orc?\"}"));
    v.type = GV_OBJECT;
    CHECK(dev_json_gval(o, sizeof o, &v) > 0 && !strcmp(o, "{\"type\":\"object\"}"));
    v.type = GV_ARRAY;
    CHECK(dev_json_gval(o, sizeof o, &v) > 0 && !strcmp(o, "{\"type\":\"array\"}"));
    v.type = 12;
    CHECK(dev_json_gval(o, sizeof o, &v) > 0 && !strcmp(o, "{\"type\":\"unknown\"}"));
    v.type = GV_NUMBER; v.v.n = 3;
    CHECK(dev_json_gval(o, 10, &v) == -1 && o[0] == 0);
}

static void test_type_names(void) {
    CHECK(!strcmp(dev_type_name(GV_NUMBER), "number"));
    CHECK(!strcmp(dev_type_name(GV_STRING | GV_MANAGED), "string"));
    CHECK(!strcmp(dev_type_name(GV_DISPLAYOBJECT), "displayobject"));
    CHECK(!strcmp(dev_type_name(9), "unknown"));
}

static void test_hex(void) {
    const uint8_t in[] = { 0x00, 0x7f, 0xa9, 0xff };
    char o[16]; uint8_t b[8];
    CHECK(dev_hex(o, sizeof o, in, 4) == 8 && !strcmp(o, "007fa9ff"));
    CHECK(dev_hex(o, 9, in, 4) == 8);
    CHECK(dev_hex(o, 8, in, 4) == -1 && o[0] == 0);
    CHECK(dev_hex(o, sizeof o, in, 0) == 0 && o[0] == 0);
    CHECK(dev_unhex("007FA9ff", b, sizeof b) == 4 && !memcmp(b, in, 4));
    CHECK(dev_unhex("", b, sizeof b) == 0);
    CHECK(dev_unhex("abc", b, sizeof b) == -1);
    CHECK(dev_unhex("zz", b, sizeof b) == -1);
    CHECK(dev_unhex("0011", b, 1) == -1);
    // round trip
    uint8_t all[256], back[256]; char hx[513];
    for (int i = 0; i < 256; i++) all[i] = (uint8_t)i;
    CHECK(dev_hex(hx, sizeof hx, all, 256) == 512);
    CHECK(dev_unhex(hx, back, sizeof back) == 256 && !memcmp(all, back, 256));
}

int main(void) {
    test_gval_layout();
    test_split();
    test_token_gval();
    test_u32();
    test_json_str();
    test_json_gval();
    test_type_names();
    test_hex();
    printf("devproto: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}

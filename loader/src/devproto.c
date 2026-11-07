// devproto.c - see devproto.h. No Windows, no game: everything here is unit-tested.
#include "devproto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int dev_split(char *line, DevTok *toks, int max) {
    int n = 0;
    char *p = line;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) return n;
        if (n == max) return -2;
        if (*p == '"') {
            char *w = ++p;
            toks[n].s = w;
            toks[n].quoted = 1;
            for (;;) {
                if (!*p) return -1;
                if (*p == '"') { p++; break; }
                if (*p == '\\' && p[1]) {
                    p++;
                    *w++ = (*p == 'n') ? '\n' : *p;
                    p++;
                    continue;
                }
                *w++ = *p++;
            }
            if (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') return -1;  // "ab"cd
            if (*p) p++;
            *w = 0;
        } else {
            toks[n].s = p;
            toks[n].quoted = 0;
            while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
            if (*p) *p++ = 0;
        }
        n++;
    }
}

void dev_token_gval(const DevTok *t, GVal *out) {
    memset(out, 0, sizeof *out);
    const char *s = t->s;
    if (!t->quoted) {
        if (!strcmp(s, "true") || !strcmp(s, "false")) { out->type = GV_BOOL; out->v.b = s[0] == 't'; return; }
        if (!strcmp(s, "null")) { out->type = GV_NULL; return; }
        if (!strcmp(s, "undefined")) { out->type = GV_UNDEFINED; return; }
        if (*s) {
            char *end;
            double d = strtod(s, &end);
            if (*end == 0) { out->type = GV_NUMBER; out->v.n = d; return; }
        }
    }
    out->type = GV_STRING;
    out->v.s = s;
}

int dev_parse_u32(const char *s, uint32_t *out) {
    if (!s || !*s || *s == '-' || *s == '+') return 0;
    char *end;
    unsigned long v = strtoul(s, &end, 0);
    if (*end || v > 0xFFFFFFFFul) return 0;
    *out = (uint32_t)v;
    return 1;
}

// ------------------------------------------------------------------ JSON
typedef struct { char *p; size_t n, len; int ovf; } Buf;

static void put(Buf *b, const char *s, size_t k) {
    if (b->ovf) return;
    if (b->len + k >= b->n) { b->ovf = 1; return; }
    memcpy(b->p + b->len, s, k);
    b->len += k;
    b->p[b->len] = 0;
}
static void putc_(Buf *b, char c) { put(b, &c, 1); }

static void put_jstr(Buf *b, const char *s) {
    putc_(b, '"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { putc_(b, '\\'); putc_(b, (char)c); }
        else if (c == '\n') put(b, "\\n", 2);
        else if (c == '\r') put(b, "\\r", 2);
        else if (c == '\t') put(b, "\\t", 2);
        else if (c < 0x20) { char e[8]; snprintf(e, sizeof e, "\\u%04x", c); put(b, e, 6); }
        else putc_(b, (char)c);
    }
    putc_(b, '"');
}

static int finish(Buf *b) {
    if (b->ovf) { if (b->n) b->p[0] = 0; return -1; }
    return (int)b->len;
}

int dev_json_str(char *out, size_t n, const char *s) {
    Buf b = { out, n, 0, n == 0 };
    if (n) out[0] = 0;
    put_jstr(&b, s);
    return finish(&b);
}

const char *dev_type_name(uint32_t type) {
    static const char *names[] = { "undefined", "null", "bool", "number", "string",
                                   "stringw", "object", "array", "displayobject" };
    uint32_t t = type & GV_TYPE_MASK;
    return t < sizeof names / sizeof *names ? names[t] : "unknown";
}

int dev_json_gval(char *out, size_t n, const GVal *v) {
    Buf b = { out, n, 0, n == 0 };
    if (n) out[0] = 0;
    uint32_t t = v->type & GV_TYPE_MASK;
    put(&b, "{\"type\":", 8);
    put_jstr(&b, dev_type_name(v->type));
    char num[64];
    switch (t) {
    case GV_BOOL:   put(&b, v->v.b ? ",\"value\":true" : ",\"value\":false", v->v.b ? 13 : 14); break;
    case GV_NUMBER: {
        double d = v->v.n;
        if (d != d || d > 1.7e308 || d < -1.7e308) put(&b, ",\"value\":null", 13);   // NaN/inf are not JSON
        else { int k = snprintf(num, sizeof num, ",\"value\":%.17g", d); put(&b, num, (size_t)k); }
        break;
    }
    case GV_STRING: put(&b, ",\"value\":", 9); put_jstr(&b, v->v.s ? v->v.s : ""); break;
    case GV_STRINGW: {
        // ASCII-fold for the log; non-ASCII becomes '?'. Bounded to 1024 chars.
        char tmp[1025]; size_t i = 0;
        if (v->v.ws) for (; v->v.ws[i] && i < 1024; i++) tmp[i] = v->v.ws[i] < 0x80 ? (char)v->v.ws[i] : '?';
        tmp[i] = 0;
        put(&b, ",\"value\":", 9); put_jstr(&b, tmp);
        break;
    }
    default: break;   // undefined/null/object/array: type only
    }
    putc_(&b, '}');
    return finish(&b);
}

int dev_hex(char *out, size_t n, const uint8_t *p, size_t len) {
    static const char hx[] = "0123456789abcdef";
    if (n < len * 2 + 1) { if (n) out[0] = 0; return -1; }
    for (size_t i = 0; i < len; i++) { out[2 * i] = hx[p[i] >> 4]; out[2 * i + 1] = hx[p[i] & 15]; }
    out[len * 2] = 0;
    return (int)(len * 2);
}

static int nib(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int dev_kill_verdict(const DevKillCand *c) {
    if (c->is_player) return DK_PLAYER;
    if (c->dead || !(c->health > 0)) return DK_DEAD;
    if (c->team == c->player_team) return DK_ALLY;
    if (c->net_id != 0xFFFF && ((c->net_id >> 10) & 0xFF) != (c->local_slot & 0xFF)) return DK_NOT_OWNED;
    return DK_KILL;
}

const char *dev_kill_verdict_name(int v) {
    static const char *names[] = { "killed", "players", "dead", "allies", "not_owned" };
    return v >= 0 && v <= DK_NOT_OWNED ? names[v] : "unknown";
}

float dev_god_health(int god, int is_player, float current, float wanted) {
    return god && is_player && wanted < current ? current : wanted;
}

int dev_unhex(const char *s, uint8_t *out, size_t cap) {
    size_t L = strlen(s);
    if (L % 2 || L / 2 > cap) return -1;
    for (size_t i = 0; i < L / 2; i++) {
        int h = nib(s[2 * i]), l = nib(s[2 * i + 1]);
        if (h < 0 || l < 0) return -1;
        out[i] = (uint8_t)(h << 4 | l);
    }
    return (int)(L / 2);
}

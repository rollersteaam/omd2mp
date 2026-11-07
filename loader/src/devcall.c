// devcall.c - see devcall.h.
#include "devcall.h"
#include <string.h>

#if defined(_M_IX86) || defined(__i386__)

#define MAX_VTABLES 8
static struct { void **vt; DevRetValFn orig; } g_hooked[MAX_VTABLES];
static int g_nhooked;

static int g_capturing, g_captured;
static GVal *g_ret;
static char *g_strbuf;
static size_t g_strcap;

void devcall_reset(void) {
    memset(g_hooked, 0, sizeof g_hooked);
    g_nhooked = 0;
    g_capturing = g_captured = 0;
}

void devcall_end(void) { g_capturing = 0; }

static DevRetValFn orig_for(void *movie) {
    void **vt = *(void ***)movie;
    for (int i = 0; i < g_nhooked; i++)
        if (g_hooked[i].vt == vt) return g_hooked[i].orig;
    return 0;
}

void __fastcall devcall_capture(void *movie, void *edx, const GVal *v) {
    if (g_capturing) {
        *g_ret = *v;
        g_ret->iface = 0;                       // never keep a reference to a managed value
        uint32_t t = v->type & GV_TYPE_MASK;
        if (t == GV_STRING) {
            const char *s = v->v.s ? v->v.s : "";
            size_t k = strlen(s);
            if (k >= g_strcap) k = g_strcap - 1;
            memcpy(g_strbuf, s, k);
            g_strbuf[k] = 0;
            g_ret->v.s = g_strbuf;
        } else if (t == GV_STRINGW) {
            size_t i = 0;
            if (v->v.ws) for (; v->v.ws[i] && i + 1 < g_strcap; i++)
                g_strbuf[i] = v->v.ws[i] < 0x80 ? (char)v->v.ws[i] : '?';
            g_strbuf[i] = 0;
            g_ret->type = GV_STRING;
            g_ret->v.s = g_strbuf;
        } else if (t >= GV_OBJECT) {
            memset(&g_ret->v, 0, sizeof g_ret->v);   // object/array: report the type only
        }
        g_captured = 1;
        return;
    }
    DevRetValFn o = orig_for(movie);
    if (o) o(movie, edx, v);
}

int devcall_hook_movie(void *movie, DevWritePtr write_ptr) {
    void **vt = *(void ***)movie;
    if (vt[DEVCALL_RETVAL_SLOT] == (void *)devcall_capture) return 1;
    if (orig_for(movie)) return 1;              // hooked but someone re-wrote it: leave it alone
    if (g_nhooked == MAX_VTABLES) return 0;
    DevRetValFn orig = (DevRetValFn)vt[DEVCALL_RETVAL_SLOT];
    g_hooked[g_nhooked].vt = vt;
    g_hooked[g_nhooked].orig = orig;
    g_nhooked++;                                // registered before the write: no window where
    if (!write_ptr(&vt[DEVCALL_RETVAL_SLOT], (void *)devcall_capture)) {   // capture can't forward
        g_nhooked--;
        return 0;
    }
    return 1;
}

int devcall_invoke(DevBindingFn h, void *self, GVal *args, unsigned argc,
                   GVal *ret, char *strbuf, size_t strcap) {
    memset(ret, 0, sizeof *ret);
    ret->type = GV_UNDEFINED;
    g_ret = ret;
    g_strbuf = strbuf;
    g_strcap = strcap;
    g_captured = 0;
    g_capturing = 1;
    h(self, args, argc, 0);
    g_capturing = 0;
    return g_captured;
}

#endif

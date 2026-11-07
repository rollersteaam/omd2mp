// devcon.c - the game side of the dev console. See devcon.h and CONTRIBUTING.md §12.
//
// Threads:
//   pipe thread  - accepts one client at a time, reads lines, hands each to the main
//                  thread and waits (up to 5 s) for the reply. `hello` is answered here.
//   main thread  - run_hook() is called once per frame by the engine; it executes at most
//                  one pending command. All game access happens here.
//
// Game addresses (OrcsMustDie2.exe, stamp 0x51DD91A9). Each pointer we replace is checked
// against its expected original value first; any mismatch -> no hook is installed.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "devcon.h"
#include "devcall.h"
#include "devcheat.h"
#include "devproto.h"
#include "mockpeer.h"

#define APP_RUN_SLOT     0x00868E18u    // BApp vtable[16] = Run(), called once per frame
#define APP_RUN_ORIG     0x004A77B0u
#define UI_CALL_SLOT     0x00866290u    // UI ExternalInterface callback: Call(name,args,argc)
#define UI_CALL_ORIG     0x0048E500u
#define BIND_LOOKUP      0x0048DE10u    // void *__cdecl lookup(const char *name)
#define SESSION_GLOBAL   0x00B9D108u
#define SLOT_OLD_BASE    0x38u
#define SLOT_NEW_BASE    0x36068u
#define SLOT_SIZE        0x18u

static const uint8_t BIND_LOOKUP_PROLOGUE[] = { 0x6A, 0xFF, 0x68, 0xFB, 0x3F, 0x80, 0x00 };

typedef char(__fastcall *RunFn)(void *self, void *edx);
typedef void(__fastcall *UiCallFn)(void *self, void *edx, const char *name, void *args, unsigned argc);
typedef void *(__cdecl *LookupFn)(const char *name);
typedef void(__fastcall *SetSlowFn)(void *timer, void *edx, int on);
typedef void(__fastcall *SetScaleFn)(void *timer, void *edx, float scale);

static DevConfig g_cfg;
static RunFn g_orig_run;
static UiCallFn g_orig_ui;
static void *volatile g_ui_self;
static volatile LONG g_ticks;

// ------------------------------------------------------------------ command hand-off
enum { ST_IDLE, ST_PENDING, ST_RUNNING, ST_DONE };
static CRITICAL_SECTION g_lock;
static HANDLE g_done;
static int g_state;
static char g_cmd[DEV_MAX_LINE];
static char g_reply[65536];

#define LOG(...) do { if (g_cfg.log) g_cfg.log(__VA_ARGS__); } while (0)

static int write_ptr(void **slot, void *value) {
    DWORD old;
    if (!VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old)) return 0;
    *slot = value;
    VirtualProtect(slot, sizeof *slot, old, &old);
    return 1;
}

static int readable(const void *p, size_t n) {
    MEMORY_BASIC_INFORMATION mi;
    const uint8_t *a = p, *end = a + n;
    while (a < end) {
        if (!VirtualQuery(a, &mi, sizeof mi) || mi.State != MEM_COMMIT) return 0;
        if (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
        a = (const uint8_t *)mi.BaseAddress + mi.RegionSize;
    }
    return 1;
}

// ------------------------------------------------------------------ replies
static int g_rlen;
static void rs(const char *s) {
    size_t k = strlen(s);
    if (g_rlen + k + 2 >= sizeof g_reply) return;
    memcpy(g_reply + g_rlen, s, k);
    g_rlen += (int)k;
    g_reply[g_rlen] = 0;
}
static void rf(const char *fmt, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(tmp, sizeof tmp - 1, fmt, ap);
    tmp[sizeof tmp - 1] = 0;
    va_end(ap);
    rs(tmp);
}
static void rjstr(const char *s) {
    char tmp[2048];
    if (dev_json_str(tmp, sizeof tmp, s) < 0) rs("\"(too long)\"");
    else rs(tmp);
}
static void err(const char *msg) { g_rlen = 0; g_reply[0] = 0; rs("{\"ok\":false,\"error\":"); rjstr(msg); rs("}"); }

// ------------------------------------------------------------------ commands (main thread)
static void cmd_call(DevTok *t, int n) {
    static GVal args[DEV_MAX_ARGS];
    static char strbuf[8192];
    GVal ret;
    if (n < 2) { err("usage: call <Binding> [args...]"); return; }
    void *self = g_ui_self;
    if (!self) { err("UI not seen yet: open a menu once so the game calls a binding"); return; }
    void *movie = *(void **)((uint8_t *)self + DEVCALL_MOVIE_OFF);
    if (!movie) { err("UI has no movie"); return; }
    DevBindingFn h = (DevBindingFn)((LookupFn)BIND_LOOKUP)(t[1].s);
    if (!h) { err("unknown binding (names are case-sensitive; see research/bindings.tsv)"); return; }
    if (!devcall_hook_movie(movie, write_ptr)) { err("could not hook the movie's return-value slot"); return; }
    for (int i = 2; i < n; i++) dev_token_gval(&t[i], &args[i - 2]);
    int got = devcall_invoke(h, self, args, (unsigned)(n - 2), &ret, strbuf, sizeof strbuf);
    char v[sizeof strbuf + 64];
    if (dev_json_gval(v, sizeof v, &ret) < 0) strcpy(v, "{\"type\":\"string\",\"value\":\"(too long)\"}");
    rf("{\"ok\":true,\"returned\":%s,\"result\":", got ? "true" : "false");
    rs(v);
    rs("}");
}

static void cmd_session(void) {
    uint8_t *s = *(uint8_t **)SESSION_GLOBAL;
    if (!s) { rs("{\"ok\":true,\"session\":null}"); return; }
    uint32_t base = g_cfg.slots_patched ? SLOT_NEW_BASE : SLOT_OLD_BASE;
    int nslots = g_cfg.slots_patched ? g_cfg.players : 2;
    rf("{\"ok\":true,\"session\":{\"addr\":\"%#x\",\"local_slot\":%d,\"player_count\":%d,\"slot_base\":\"%#x\",\"slots\":[",
       (unsigned)(uintptr_t)s, (int)*(int8_t *)(s + 0x6C), *(int32_t *)(s + 0x70), base);
    for (int i = 0; i < nslots; i++) {
        uint8_t *sl = s + base + (uint32_t)i * SLOT_SIZE;
        char g[17];
        dev_hex(g, sizeof g, sl, 8);
        rf("%s{\"guid\":\"%s\",\"system_index\":%u,\"state\":\"%#x\",\"flags\":[%u,%u]}", i ? "," : "",
           g, *(uint16_t *)(sl + 8), *(uint32_t *)(sl + 0x10), sl[0x16], sl[0x17]);
    }
    rs("]}}");
}

static void cmd_timescale(DevTok *t, int n) {
    static void *timer;
    static SetSlowFn set_slow;
    static SetScaleFn set_scale;
    if (!timer) {
        HMODULE v = GetModuleHandleA("vision90.dll");
        if (v) {
            timer = (void *)GetProcAddress(v, "?Timer@Vision@@2VVisTimer_cl@@A");
            set_slow = (SetSlowFn)GetProcAddress(v, "?SetSlowMotion@VisTimer_cl@@QAEXW4VIS_CFG_OnOff@@@Z");
            set_scale = (SetScaleFn)GetProcAddress(v, "?SetSlowMotionTimeScale@VisTimer_cl@@QAEXM@Z");
        }
        if (!timer || !set_slow || !set_scale) { timer = 0; err("vision90.dll timer exports not found"); return; }
    }
    double f = 0;
    if (n != 2 || sscanf(t[1].s, "%lf", &f) != 1 || f <= 0 || f > 64) { err("usage: timescale <0..64>  (1 = normal)"); return; }
    set_scale(timer, 0, (float)f);
    set_slow(timer, 0, f != 1.0);
    rf("{\"ok\":true,\"timescale\":%g}", f);
}

static void cmd_read(DevTok *t, int n) {
    uint32_t a, len;
    static uint8_t buf[1024];
    static char hex[2049];
    if (n != 3 || !dev_parse_u32(t[1].s, &a) || !dev_parse_u32(t[2].s, &len) || len == 0 || len > sizeof buf) {
        err("usage: read <addr> <len 1..1024>");
        return;
    }
    if (!readable((void *)(uintptr_t)a, len)) { err("address not readable"); return; }
    memcpy(buf, (void *)(uintptr_t)a, len);
    dev_hex(hex, sizeof hex, buf, len);
    rf("{\"ok\":true,\"addr\":\"%#x\",\"hex\":\"%s\"}", a, hex);
}

static void cmd_write(DevTok *t, int n) {
    uint32_t a;
    static uint8_t buf[1024];
    if (!g_cfg.allow_write) { err("write is disabled (set allow_write=1 under [dev] in omd2mp.ini)"); return; }
    int len = n == 3 && dev_parse_u32(t[1].s, &a) ? dev_unhex(t[2].s, buf, sizeof buf) : -1;
    if (len <= 0) { err("usage: write <addr> <hexbytes>"); return; }
    if (!readable((void *)(uintptr_t)a, (size_t)len)) { err("address not mapped"); return; }
    DWORD old;
    if (!VirtualProtect((void *)(uintptr_t)a, (size_t)len, PAGE_EXECUTE_READWRITE, &old)) { err("VirtualProtect failed"); return; }
    memcpy((void *)(uintptr_t)a, buf, (size_t)len);
    VirtualProtect((void *)(uintptr_t)a, (size_t)len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)(uintptr_t)a, (size_t)len);
    LOG("dev: wrote %d bytes at %#x", len, a);
    rf("{\"ok\":true,\"written\":%d}", len);
}

static void cmd_cheat(DevTok *t, int n) {
    static const char usage[] = "usage: cheat gold [amount] | cheat killall | cheat god [on|off]";
    const char *e, *sub = n >= 2 ? t[1].s : "";
    if (!strcmp(sub, "gold") && n <= 3) {
        double a = 10000;
        int cash = 0;
        if (n == 3 && (sscanf(t[2].s, "%lf", &a) != 1 || a < -1e7 || a > 1e7)) { err("amount must be -10000000..10000000"); return; }
        if ((e = devcheat_gold((int)a, &cash)) != NULL) { err(e); return; }
        LOG("dev: cheat gold %d -> %d", (int)a, cash);
        rf("{\"ok\":true,\"added\":%d,\"cash\":%d}", (int)a, cash);
    } else if (!strcmp(sub, "killall") && n == 2) {
        DevKillStats st;
        if ((e = devcheat_killall(&st)) != NULL) { err(e); return; }
        LOG("dev: cheat killall: %d killed of %d characters, %d faults", st.counts[DK_KILL], st.characters, st.faults);
        rf("{\"ok\":true,\"entities\":%d,\"characters\":%d", st.entities, st.characters);
        for (int v = DK_KILL; v <= DK_NOT_OWNED; v++) rf(",\"%s\":%d", dev_kill_verdict_name(v), st.counts[v]);
        rf(",\"overflow\":%d,\"faults\":%d}", st.overflow, st.faults);
    } else if (!strcmp(sub, "god") && n <= 3) {
        if (n == 3) {
            int on = !strcmp(t[2].s, "on") || !strcmp(t[2].s, "1") || !strcmp(t[2].s, "true");
            if (!on && strcmp(t[2].s, "off") && strcmp(t[2].s, "0") && strcmp(t[2].s, "false")) { err(usage); return; }
            if ((e = devcheat_god(on)) != NULL) { err(e); return; }
            LOG("dev: cheat god %s", on ? "on" : "off");
        }
        rf("{\"ok\":true,\"god\":%s}", devcheat_god_enabled() ? "true" : "false");
    } else err(usage);
}

// Where a command faulted: code, module+offset, the address an access violation touched,
// and return-address-looking stack words inside the exe's .text (a cheap backtrace).
static char g_fault[512];

static void describe_addr(char *out, int cap, uintptr_t a) {
    HMODULE m = NULL;
    char path[MAX_PATH] = "?";
    const char *base;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)a, &m) && m)
        GetModuleFileNameA(m, path, sizeof path);
    base = strrchr(path, '\\') ? strrchr(path, '\\') + 1 : path;
    _snprintf(out, cap - 1, "%#x (%s+%#x)", (unsigned)a, base, (unsigned)(a - (uintptr_t)m));
    out[cap - 1] = 0;
}

static int fault_filter(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *r = ep->ExceptionRecord;
    char where[160], *o = g_fault;
    int left = sizeof g_fault, k, hints = 0;
    uint32_t *sp = (uint32_t *)(uintptr_t)ep->ContextRecord->Esp;
    describe_addr(where, sizeof where, (uintptr_t)r->ExceptionAddress);
    k = _snprintf(o, left, "exception %#lx at %s", r->ExceptionCode, where);
    if (k > 0) { o += k; left -= k; }
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2 && left > 0) {
        k = _snprintf(o, left, " %s %#x", r->ExceptionInformation[0] == 1 ? "writing" :
                      r->ExceptionInformation[0] == 8 ? "executing" : "reading", (unsigned)r->ExceptionInformation[1]);
        if (k > 0) { o += k; left -= k; }
    }
    if (left > 0) { k = _snprintf(o, left, "; stack"); if (k > 0) { o += k; left -= k; } }
    for (k = 0; k < 256 && hints < 12 && left > 12 && readable(sp + k, 4); k++)
        if (sp[k] >= 0x401000u && sp[k] < 0x830000u) {
            int w = _snprintf(o, left, " %#x", sp[k]);
            if (w > 0) { o += w; left -= w; hints++; }
        }
    g_fault[sizeof g_fault - 1] = 0;
    return EXCEPTION_EXECUTE_HANDLER;
}

static void cmd_mock(DevTok *t, int n) {
    const char *argv[DEV_MAX_ARGS + 2];
    static char out[16384];
    int i;
    for (i = 0; i < n; i++) argv[i] = t[i].s;
    mock_command(n, argv, out, sizeof out);
    rs(out);
}

static void exec_main(char *line) {
    DevTok t[DEV_MAX_ARGS + 2];
    int n = dev_split(line, t, DEV_MAX_ARGS + 2);
    g_rlen = 0;
    g_reply[0] = 0;
    if (n == -1) { err("unterminated quote"); return; }
    if (n < 0) { err("too many arguments"); return; }
    if (n == 0) { err("empty command"); return; }
    const char *c = t[0].s;
    __try {
        if (!strcmp(c, "ping")) rf("{\"ok\":true,\"ticks\":%ld}", g_ticks);
        else if (!strcmp(c, "call")) cmd_call(t, n);
        else if (!strcmp(c, "session")) cmd_session();
        else if (!strcmp(c, "timescale")) cmd_timescale(t, n);
        else if (!strcmp(c, "read")) cmd_read(t, n);
        else if (!strcmp(c, "write")) cmd_write(t, n);
        else if (!strcmp(c, "cheat")) cmd_cheat(t, n);
        else if (!strcmp(c, "mock")) cmd_mock(t, n);
        else if (!strcmp(c, "log")) {
            LOG("dev-client: %s", n > 1 ? t[1].s : "");
            rs("{\"ok\":true}");
        } else err("unknown command (hello ping call session timescale read write cheat mock log)");
    } __except (fault_filter(GetExceptionInformation())) {
        devcall_end();
        char m[640];
        _snprintf(m, sizeof m - 1, "%s while running '%s' (game state may be inconsistent)", g_fault, c);
        m[sizeof m - 1] = 0;
        LOG("dev: %s", m);
        err(m);
    }
}

// Called by the engine once per frame (thiscall Run(this) -> bool, via fastcall shim).
static char __fastcall run_hook(void *self, void *edx) {
    InterlockedIncrement(&g_ticks);
    EnterCriticalSection(&g_lock);
    if (g_state == ST_PENDING) {
        static char line[DEV_MAX_LINE];
        memcpy(line, g_cmd, sizeof line);
        g_state = ST_RUNNING;
        LeaveCriticalSection(&g_lock);
        exec_main(line);
        EnterCriticalSection(&g_lock);
        g_state = ST_DONE;
        SetEvent(g_done);
    }
    LeaveCriticalSection(&g_lock);
    return g_orig_run(self, edx);
}

static void __fastcall ui_call_hook(void *self, void *edx, const char *name, void *args, unsigned argc) {
    g_ui_self = self;
    g_orig_ui(self, edx, name, args, argc);
}

// ------------------------------------------------------------------ pipe thread
static void send_line(HANDLE p, const char *s) {
    DWORD w;
    WriteFile(p, s, (DWORD)strlen(s), &w, NULL);
    WriteFile(p, "\n", 1, &w, NULL);
}

static void handle_line(HANDLE p, char *line) {
    char probe[64];
    lstrcpynA(probe, line, sizeof probe);
    DevTok t[2];
    int n = dev_split(probe, t, 2);
    if (n >= 1 && !strcmp(t[0].s, "hello")) {
        char r[256];
        wsprintfA(r, "{\"ok\":true,\"name\":\"omd2mp-dev\",\"version\":1,\"ticks\":%ld,\"ui\":%s,"
                     "\"players\":%d,\"slots_patched\":%s,\"allow_write\":%s}",
                  g_ticks, g_ui_self ? "true" : "false", g_cfg.players,
                  g_cfg.slots_patched ? "true" : "false", g_cfg.allow_write ? "true" : "false");
        send_line(p, r);
        return;
    }
    EnterCriticalSection(&g_lock);
    lstrcpynA(g_cmd, line, sizeof g_cmd);
    g_state = ST_PENDING;
    ResetEvent(g_done);
    LeaveCriticalSection(&g_lock);
    if (WaitForSingleObject(g_done, 5000) != WAIT_OBJECT_0) {
        EnterCriticalSection(&g_lock);
        int st = g_state;
        if (st == ST_PENDING) g_state = ST_IDLE;     // never picked up: cancel it
        LeaveCriticalSection(&g_lock);
        if (st == ST_PENDING) {
            char r[200];
            wsprintfA(r, "{\"ok\":false,\"error\":\"main thread did not pick up the command in 5 s (ticks=%ld)\"}", g_ticks);
            send_line(p, r);
            return;
        }
        WaitForSingleObject(g_done, INFINITE);         // running: it will finish
    }
    EnterCriticalSection(&g_lock);
    send_line(p, g_reply);
    g_state = ST_IDLE;
    LeaveCriticalSection(&g_lock);
}

static DWORD WINAPI pipe_thread(LPVOID unused) {
    (void)unused;
    char name[96];
    wsprintfA(name, "\\\\.\\pipe\\%s", g_cfg.pipe);
    for (;;) {
        HANDLE p = CreateNamedPipeA(name, PIPE_ACCESS_DUPLEX,
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                    1, 65536, 65536, 0, NULL);
        if (p == INVALID_HANDLE_VALUE) { LOG("dev: CreateNamedPipe(%s) failed: %lu", name, GetLastError()); return 1; }
        if (ConnectNamedPipe(p, NULL) || GetLastError() == ERROR_PIPE_CONNECTED) {
            static char acc[DEV_MAX_LINE];
            int len = 0, overflow = 0;
            char buf[1024];
            DWORD got;
            while (ReadFile(p, buf, sizeof buf, &got, NULL) && got) {
                for (DWORD i = 0; i < got; i++) {
                    char ch = buf[i];
                    if (ch == '\n') {
                        acc[len] = 0;
                        if (overflow) send_line(p, "{\"ok\":false,\"error\":\"line too long\"}");
                        else if (len) handle_line(p, acc);
                        len = overflow = 0;
                    } else if (len < DEV_MAX_LINE - 1) acc[len++] = ch;
                    else overflow = 1;
                }
            }
        }
        DisconnectNamedPipe(p);
        CloseHandle(p);
    }
}

// ------------------------------------------------------------------ install
static int swap(uint32_t slot, uint32_t expect, void *hook, void **orig) {
    void **s = (void **)(uintptr_t)slot;
    if (!readable(s, sizeof *s)) { LOG("dev: %#x not readable", slot); return 0; }
    if ((uint32_t)(uintptr_t)*s != expect) { LOG("dev: %#x holds %p, expected %#x", slot, *s, expect); return 0; }
    *orig = *s;
    return write_ptr(s, hook);
}

int devcon_start(const DevConfig *cfg) {
    g_cfg = *cfg;
    if (!g_cfg.pipe[0]) lstrcpyA(g_cfg.pipe, "omd2mp-dev");
    if (memcmp((void *)(uintptr_t)BIND_LOOKUP, BIND_LOOKUP_PROLOGUE, sizeof BIND_LOOKUP_PROLOGUE)) {
        LOG("dev: binding lookup at %#x does not match; dev console not started", BIND_LOOKUP);
        return 0;
    }
    void **app = (void **)(uintptr_t)APP_RUN_SLOT, **ui = (void **)(uintptr_t)UI_CALL_SLOT;
    if (!readable(app, 4) || !readable(ui, 4) ||
        (uint32_t)(uintptr_t)*app != APP_RUN_ORIG || (uint32_t)(uintptr_t)*ui != UI_CALL_ORIG) {
        LOG("dev: vtable slots do not match (app %p ui %p); dev console not started",
            readable(app, 4) ? *app : 0, readable(ui, 4) ? *ui : 0);
        return 0;
    }
    InitializeCriticalSection(&g_lock);
    g_done = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!swap(APP_RUN_SLOT, APP_RUN_ORIG, (void *)run_hook, (void **)&g_orig_run) ||
        !swap(UI_CALL_SLOT, UI_CALL_ORIG, (void *)ui_call_hook, (void **)&g_orig_ui)) {
        LOG("dev: hook install failed");
        if (g_orig_run) write_ptr(app, (void *)g_orig_run);
        return 0;
    }
    HANDLE th = CreateThread(NULL, 0, pipe_thread, NULL, 0, NULL);
    if (th) CloseHandle(th);
    LOG("dev: console on \\\\.\\pipe\\%s (allow_write=%d)", g_cfg.pipe, g_cfg.allow_write);
    return 1;
}

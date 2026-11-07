// crashlog.c - see crashlog.h.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "crashlog.h"
#include "mockpeer.h"

#define MAX_REPORTS 4

static CrashLogFn g_log;
static uintptr_t g_cave_lo, g_cave_hi;
static volatile LONG g_reports;
static volatile LONG g_faulted;

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

static int executable(uintptr_t a) {
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery((void *)a, &mi, sizeof mi) || mi.State != MEM_COMMIT) return 0;
    return (mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

// "0x4b2bf6 (OrcsMustDie2.exe+0xb2bf6)", or "(omd2mp cave+0x40)".
static void where(char *out, int cap, uintptr_t a) {
    HMODULE m = NULL;
    char path[MAX_PATH] = "?";
    const char *base;
    if (a >= g_cave_lo && a < g_cave_hi) {
        _snprintf(out, cap - 1, "%#x (omd2mp cave+%#x)", (unsigned)a, (unsigned)(a - g_cave_lo));
    } else if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  (LPCSTR)a, &m) && m) {
        GetModuleFileNameA(m, path, sizeof path);
        base = strrchr(path, '\\') ? strrchr(path, '\\') + 1 : path;
        _snprintf(out, cap - 1, "%#x (%s+%#x)", (unsigned)a, base, (unsigned)(a - (uintptr_t)m));
    } else {
        _snprintf(out, cap - 1, "%#x (no module)", (unsigned)a);
    }
    out[cap - 1] = 0;
}

static int fatal(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_IN_PAGE_ERROR:
    case 0xC0000374:    // heap corruption
    case 0xC0000409:    // stack buffer overrun / __fastfail
        return 1;
    }
    return 0;
}

static LONG CALLBACK on_exception(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *r = ep->ExceptionRecord;
    CONTEXT *c = ep->ContextRecord;
    char w[160], line[512];
    uint32_t *sp;
    int k, n, hints = 0;
    if (!fatal(r->ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&g_reports) > MAX_REPORTS) return EXCEPTION_CONTINUE_SEARCH;
    InterlockedExchange(&g_faulted, 1);

    where(w, sizeof w, (uintptr_t)r->ExceptionAddress);
    g_log("CRASH (first chance, thread %lu): exception %#lx at %s", GetCurrentThreadId(), r->ExceptionCode, w);
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
        g_log("  %s address %#x", r->ExceptionInformation[0] == 1 ? "writing" :
              r->ExceptionInformation[0] == 8 ? "executing" : "reading", (unsigned)r->ExceptionInformation[1]);
    g_log("  eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx",
          c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi);
    g_log("  eip=%08lx esp=%08lx ebp=%08lx", c->Eip, c->Esp, c->Ebp);
    if (readable((void *)(uintptr_t)c->Eip, 16)) {
        const uint8_t *ip = (const uint8_t *)(uintptr_t)c->Eip;
        n = _snprintf(line, sizeof line, "  code at eip:");
        for (k = 0; k < 16 && n > 0 && n < (int)sizeof line - 4; k++) n += _snprintf(line + n, sizeof line - n, " %02x", ip[k]);
        g_log("%s", line);
    }
    // The raw top of the stack, then words that point into executable code (likely return
    // addresses; a few will be stale, but the first ones are usually the real call chain).
    sp = (uint32_t *)(uintptr_t)c->Esp;
    if (readable(sp, 32)) {
        n = _snprintf(line, sizeof line, "  stack:");
        for (k = 0; k < 8 && n > 0; k++) n += _snprintf(line + n, sizeof line - n, " %08x", sp[k]);
        g_log("%s", line);
    }
    for (k = 0; k < 1024 && hints < 24 && readable(sp + k, 4); k++)
        if (sp[k] >= 0x10000u && executable(sp[k])) {
            where(w, sizeof w, sp[k]);
            g_log("  [esp+%#05x] %s", k * 4, w);
            hints++;
        }
    if (mock_enabled()) mock_log_recent(12);
    return EXCEPTION_CONTINUE_SEARCH;
}

void crashlog_start(CrashLogFn log, uintptr_t cave_lo, uintptr_t cave_hi) {
    g_log = log;
    g_cave_lo = cave_lo;
    g_cave_hi = cave_hi;
    if (!AddVectoredExceptionHandler(1, on_exception)) { log("crashlog: AddVectoredExceptionHandler failed"); return; }
    log("crashlog: on (faults go to this log; caves %#x..%#x)", (unsigned)cave_lo, (unsigned)cave_hi);
}

void crashlog_exit(int process_terminating) {
    if (!g_log) return;
    g_log("process exit (%s%s)", process_terminating ? "process terminating" : "FreeLibrary",
          g_faulted ? ", after a logged fault" : ", no fault logged");
}

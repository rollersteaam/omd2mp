// dllmain.c - omd2mp loader, shipped as version.dll next to OrcsMustDie2.exe.
//
// Why version.dll: the game's Vision engine DLL (vBase90.dll) imports version.dll, and
// Windows looks in the application directory first, so our copy is loaded before the
// game's code runs. We forward every real export to the system version.dll.
//
// What it does on load (all-or-nothing):
//   1. reads omd2mp.ini (players=, groups=) from the game directory
//   2. checks the game exe's PE TimeDateStamp matches the build the patches were made for
//   3. checks every enabled patch's original bytes; any mismatch -> patch nothing
//   4. writes trampolines/caves and immediates, then logs to omd2mp.log
//
// The game's own files on disk are never modified. Delete version.dll to uninstall.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include "patchcore.h"
#include "patchset_gen.h"

#pragma comment(linker, "/export:GetFileVersionInfoA=_px_GetFileVersionInfoA")
#pragma comment(linker, "/export:GetFileVersionInfoByHandle=_px_GetFileVersionInfoByHandle")
#pragma comment(linker, "/export:GetFileVersionInfoExA=_px_GetFileVersionInfoExA")
#pragma comment(linker, "/export:GetFileVersionInfoExW=_px_GetFileVersionInfoExW")
#pragma comment(linker, "/export:GetFileVersionInfoSizeA=_px_GetFileVersionInfoSizeA")
#pragma comment(linker, "/export:GetFileVersionInfoSizeExA=_px_GetFileVersionInfoSizeExA")
#pragma comment(linker, "/export:GetFileVersionInfoSizeExW=_px_GetFileVersionInfoSizeExW")
#pragma comment(linker, "/export:GetFileVersionInfoSizeW=_px_GetFileVersionInfoSizeW")
#pragma comment(linker, "/export:GetFileVersionInfoW=_px_GetFileVersionInfoW")
#pragma comment(linker, "/export:VerFindFileA=_px_VerFindFileA")
#pragma comment(linker, "/export:VerFindFileW=_px_VerFindFileW")
#pragma comment(linker, "/export:VerInstallFileA=_px_VerInstallFileA")
#pragma comment(linker, "/export:VerInstallFileW=_px_VerInstallFileW")
#pragma comment(linker, "/export:VerLanguageNameA=_px_VerLanguageNameA")
#pragma comment(linker, "/export:VerLanguageNameW=_px_VerLanguageNameW")
#pragma comment(linker, "/export:VerQueryValueA=_px_VerQueryValueA")
#pragma comment(linker, "/export:VerQueryValueW=_px_VerQueryValueW")

// ------------------------------------------------------------------ export forwarding
#define PROXIES(X) X(GetFileVersionInfoA) X(GetFileVersionInfoByHandle) X(GetFileVersionInfoExA) \
    X(GetFileVersionInfoExW) X(GetFileVersionInfoSizeA) X(GetFileVersionInfoSizeExA)            \
    X(GetFileVersionInfoSizeExW) X(GetFileVersionInfoSizeW) X(GetFileVersionInfoW)              \
    X(VerFindFileA) X(VerFindFileW) X(VerInstallFileA) X(VerInstallFileW) X(VerLanguageNameA)   \
    X(VerLanguageNameW) X(VerQueryValueA) X(VerQueryValueW)

#define DECL_PTR(n) static FARPROC real_##n;
PROXIES(DECL_PTR)
// Naked jumps keep the caller's stack untouched, so signatures don't matter.
#define DECL_STUB(n) __declspec(naked) void px_##n(void) { __asm { jmp dword ptr [real_##n] } }
PROXIES(DECL_STUB)

static void load_real_version_dll(void) {
    char path[MAX_PATH];
    UINT n = GetSystemDirectoryA(path, MAX_PATH);   // WOW64 redirects this to SysWOW64
    lstrcpynA(path + n, "\\version.dll", MAX_PATH - n);
    HMODULE h = LoadLibraryA(path);
#define RESOLVE(n) real_##n = h ? GetProcAddress(h, #n) : NULL;
    PROXIES(RESOLVE)
}

// ------------------------------------------------------------------ logging / config
static char g_dir[MAX_PATH];
static FILE *g_log;
static int g_players = 2;

static void mlog(const char *fmt, ...) {
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static void game_dir(void) {
    GetModuleFileNameA(NULL, g_dir, MAX_PATH);
    char *s = strrchr(g_dir, '\\');
    if (s) s[1] = 0;
}

// ------------------------------------------------------------------ C hooks
static void __cdecl hook_replicate_slots(uint8_t *session) {
    omd2mp_replicate_slots_impl(session, OMD2MP_SLOT_OLD_BASE, OMD2MP_SLOT_NEW_BASE,
                                OMD2MP_SLOT_SIZE, g_players);
}

static uint32_t hook_addr(int hook) {
    switch (hook) {
    case OMD2MP_HOOK_REPLICATE_SLOTS: return (uint32_t)(uintptr_t)&hook_replicate_slots;
    }
    return 0;
}

// ------------------------------------------------------------------ patching
static int write_code(uint32_t va, const uint8_t *bytes, size_t n) {
    DWORD old;
    if (!VirtualProtect((void *)(uintptr_t)va, n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy((void *)(uintptr_t)va, bytes, n);
    VirtualProtect((void *)(uintptr_t)va, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)(uintptr_t)va, n);
    return 1;
}

static int exe_timestamp_ok(void) {
    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    mlog("exe base %p timestamp %#lx (expected %#x)", base, nt->FileHeader.TimeDateStamp, OMD2MP_EXE_TIMESTAMP);
    return (uintptr_t)base == 0x400000 && nt->FileHeader.TimeDateStamp == OMD2MP_EXE_TIMESTAMP;
}

static void apply_all(const char *groups) {
    size_t i, cave_total = 0, enabled = 0;
    for (i = 0; i < OMD2MP_PATCH_COUNT; i++) {
        const Patch *p = &OMD2MP_PATCHES[i];
        if (!omd2mp_group_enabled(groups, p->group)) continue;
        enabled++;
        PcResult r = omd2mp_check(p, (const uint8_t *)(uintptr_t)p->va);
        if (r) {
            mlog("ABORT: %s @%#x: %s - nothing was patched", p->id, p->va, omd2mp_result_str(r));
            return;
        }
        cave_total += (p->cave_len + 15u) & ~15u;
    }
    uint8_t *caves = cave_total ? VirtualAlloc(NULL, cave_total, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE) : NULL;
    if (cave_total && !caves) { mlog("ABORT: VirtualAlloc failed"); return; }

    // Render everything first so a bad record aborts before any byte is written.
    static uint8_t site[OMD2MP_PATCH_COUNT][OMD2MP_MAX_SITE];
    uint8_t *cur = caves;
    for (i = 0; i < OMD2MP_PATCH_COUNT; i++) {
        const Patch *p = &OMD2MP_PATCHES[i];
        if (!omd2mp_group_enabled(groups, p->group)) continue;
        uint32_t cave_va = 0;
        PcResult r = PC_OK;
        if (p->kind == PK_TRAMP) {
            cave_va = (uint32_t)(uintptr_t)cur;
            r = omd2mp_render_cave(p, g_players, cave_va, omd2mp_eval, hook_addr, cur);
            cur += (p->cave_len + 15u) & ~15u;
        }
        if (!r) r = omd2mp_render_site(p, g_players, cave_va, omd2mp_eval, site[i]);
        if (r) {
            mlog("ABORT: %s: %s - nothing was patched", p->id, omd2mp_result_str(r));
            if (caves) VirtualFree(caves, 0, MEM_RELEASE);
            return;
        }
    }
    for (i = 0; i < OMD2MP_PATCH_COUNT; i++) {
        const Patch *p = &OMD2MP_PATCHES[i];
        if (!omd2mp_group_enabled(groups, p->group)) continue;
        if (!write_code(p->va, site[i], p->orig_len)) mlog("WARN: VirtualProtect failed at %#x (%s)", p->va, p->id);
        else mlog("patched %-40s @%#x", p->id, p->va);
    }
    mlog("done: %u patches for %d players, %u bytes of caves", (unsigned)enabled, g_players, (unsigned)cave_total);
}

static void init(void) {
    char ini[MAX_PATH], logp[MAX_PATH], groups[256];
    game_dir();
    wsprintfA(ini, "%somd2mp.ini", g_dir);
    wsprintfA(logp, "%somd2mp.log", g_dir);
    g_log = fopen(logp, "w");
    g_players = GetPrivateProfileIntA("omd2mp", "players", 4, ini);
    GetPrivateProfileStringA("omd2mp", "groups", "lobby,mesh,slots,economy", groups, sizeof groups, ini);
    mlog("omd2mp loader: players=%d groups=%s", g_players, groups);
    if (GetPrivateProfileIntA("omd2mp", "enabled", 1, ini) == 0) { mlog("disabled in ini"); return; }
    if (g_players < 2 || g_players > OMD2MP_MAX_PLAYERS) { mlog("ABORT: players must be 2..%d", OMD2MP_MAX_PLAYERS); return; }
    if (!exe_timestamp_ok()) { mlog("ABORT: unsupported game build"); return; }
    apply_all(groups);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        load_real_version_dll();
        init();
    }
    return TRUE;
}

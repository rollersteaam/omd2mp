// crashlog.h - writes fatal exceptions (where, registers, a stack backtrace) to omd2mp.log.
//
// The game dies without a dump or a Windows Error Reporting record, so this is the only
// way to see where. A vectored handler sees every exception first-chance; we log only the
// codes that kill the process (access violation, stack overflow, ...), at most a few
// times, and never handle them: the game's own handlers still run.
#ifndef OMD2MP_CRASHLOG_H
#define OMD2MP_CRASHLOG_H
#include <stdint.h>

typedef void (*CrashLogFn)(const char *fmt, ...);

// cave_lo/cave_hi: the loader's patch caves, so a fault inside one is named as such.
void crashlog_start(CrashLogFn log, uintptr_t cave_lo, uintptr_t cave_hi);
// Called from DllMain on process detach: tells a clean exit apart from a silent kill.
void crashlog_exit(int process_terminating);

#endif

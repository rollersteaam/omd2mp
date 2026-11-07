// devcon.h - omd2mp developer console (off by default; [dev] enabled=1 in omd2mp.ini).
//
// A local named pipe (\\.\pipe\<pipe>, remote clients rejected) that accepts one-line
// commands and answers with one line of JSON. Commands run on the game's main thread,
// once per frame, from a hook on the game app's Run() method. Client: tools/devcon.py.
// Protocol and commands: docs/CONTRIBUTING.md §12.
#ifndef OMD2MP_DEVCON_H
#define OMD2MP_DEVCON_H

typedef struct DevConfig {
    char pipe[64];          // pipe name without the \\.\pipe\ prefix
    int allow_write;        // enables the `write` command
    int players;            // configured party size
    int slots_patched;      // 1 if the `slots` group was applied (slots relocated, N of them)
    void (*log)(const char *fmt, ...);
} DevConfig;

// Installs the two vtable hooks and starts the pipe thread. Safe to call from DllMain.
// Returns 1 on success; on failure it logs why and leaves the game untouched.
int devcon_start(const DevConfig *cfg);

#endif

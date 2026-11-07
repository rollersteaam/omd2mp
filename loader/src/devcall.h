// devcall.h - calling the game's script bindings from C and catching their return value.
//
// How the game calls a binding (from the UI dispatcher at 0x48E500, see CONTRIBUTING §12):
//     handler(ui_self, GVal *args, unsigned argc, unsigned first /*always 0*/)   // __cdecl
// and the handler returns its value by calling, on the UI's Scaleform movie,
//     movie->vtable[50](movie, const GVal *v)        // SetExternalInterfaceRetVal, __thiscall
// where movie = *(void **)(ui_self + 0x40).
//
// To read that value we replace vtable slot 50 with devcall_capture. While a dev-console
// call is running (`capturing`), it copies the value into our buffer instead of handing it
// to Scaleform; at all other times it forwards to the original, so the UI is unaffected.
//
// x86-only (thiscall/fastcall). Everything here runs on the game's main thread.
#ifndef OMD2MP_DEVCALL_H
#define OMD2MP_DEVCALL_H
#include "devproto.h"

#define DEVCALL_MOVIE_OFF   0x40
#define DEVCALL_RETVAL_SLOT 50          // 0xC8 / 4

#if defined(_M_IX86) || defined(__i386__)
typedef void(__cdecl *DevBindingFn)(void *self, GVal *args, unsigned argc, unsigned first);
typedef void(__fastcall *DevRetValFn)(void *movie, void *edx, const GVal *v);

// Writes one pointer, returning 1 on success (the game passes a VirtualProtect wrapper).
typedef int (*DevWritePtr)(void **slot, void *value);

// Makes sure the movie's vtable slot 50 goes through devcall_capture. Idempotent.
// Returns 1 if hooked (now or before), 0 if the table of hooked vtables is full or the write failed.
int devcall_hook_movie(void *movie, DevWritePtr write_ptr);

// Calls `h` with `args` and stores what it returned in *ret (GV_UNDEFINED if nothing).
// String results are copied into `strbuf` (truncated to strcap-1) and ret->v.s points there.
// Returns 1 if the handler set a return value, 0 if it returned nothing.
int devcall_invoke(DevBindingFn h, void *self, GVal *args, unsigned argc,
                   GVal *ret, char *strbuf, size_t strcap);

// Call after an exception escaped devcall_invoke so UI return values flow again.
void devcall_end(void);

// Exposed for tests.
void __fastcall devcall_capture(void *movie, void *edx, const GVal *v);
void devcall_reset(void);
#endif
#endif

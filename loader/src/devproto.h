// devproto.h - the pure (platform-free, unit-tested) half of the omd2mp dev console.
//
// The dev console is a line protocol over a local named pipe. Each request is one line of
// space-separated tokens ("call Player_GetCash 0"), each reply is one line of JSON.
// This file holds everything that does not touch the game: tokenising, turning tokens into
// Scaleform values, and turning values back into JSON. devcon.c holds the game side.
#ifndef OMD2MP_DEVPROTO_H
#define OMD2MP_DEVPROTO_H
#include <stddef.h>
#include <stdint.h>

#define DEV_MAX_ARGS 16
#define DEV_MAX_LINE 4096

// Scaleform GFx 3.x GFxValue as the game uses it: {object interface, type, value}.
// On the 32-bit game this is exactly 16 bytes (args are read at +8 with stride 16).
enum {
    GV_UNDEFINED = 0, GV_NULL = 1, GV_BOOL = 2, GV_NUMBER = 3, GV_STRING = 4,
    GV_STRINGW = 5, GV_OBJECT = 6, GV_ARRAY = 7, GV_DISPLAYOBJECT = 8,
    GV_TYPE_MASK = 0x0F, GV_MANAGED = 0x40
};
typedef struct GVal {
    void *iface;
    uint32_t type;
    union { double n; uint8_t b; const char *s; const uint16_t *ws; uint8_t raw[8]; } v;
} GVal;

typedef struct DevTok { char *s; int quoted; } DevTok;

// Splits `line` in place. Tokens are separated by spaces/tabs; "double quoted" tokens
// may contain spaces and the escapes \" \\ \n. Returns the token count, or -1 on an
// unterminated quote, or -2 if there are more than `max` tokens.
int dev_split(char *line, DevTok *toks, int max);

// Token -> value. Quoted tokens are always strings. Otherwise: true/false -> bool,
// null/undefined, a full numeric literal (strtod, incl. 0x.. hex) -> number, else string.
void dev_token_gval(const DevTok *t, GVal *out);

// Parses an unsigned 32-bit number (decimal or 0x hex). Returns 1 on success.
int dev_parse_u32(const char *s, uint32_t *out);

// JSON helpers. All return the number of chars written (excluding NUL), or -1 if `n`
// is too small; output is always NUL-terminated when n > 0.
int dev_json_str(char *out, size_t n, const char *s);
int dev_json_gval(char *out, size_t n, const GVal *v);   // {"type":"number","value":3}
int dev_hex(char *out, size_t n, const uint8_t *p, size_t len);
// Parses an even-length hex string into `out` (max `cap` bytes). Returns length or -1.
int dev_unhex(const char *s, uint8_t *out, size_t cap);

const char *dev_type_name(uint32_t type);

#endif

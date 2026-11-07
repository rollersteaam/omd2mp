// devcheat.h - dev console cheats (`cheat ...` commands). Main thread only.
//
// The retail game still lists CheatGold / CheatChaosGems / CheatPower / CheatKillAllEnemies
// in its input table (0x8EB3DC, codes 0x82..0x85), but their handlers were compiled out,
// so these are reimplemented on top of the game's own functions. See CONTRIBUTING.md §12.6.
#ifndef OMD2MP_DEVCHEAT_H
#define OMD2MP_DEVCHEAT_H

typedef struct DevKillStats {
    int entities;           // non-null slots in Vision's entity table
    int characters;         // entities with a valid health component
    int counts[5];          // per DK_* verdict (devproto.h); counts[DK_KILL] = killed
    int overflow;           // victims beyond the per-call cap (run it again)
    int faults;             // victims skipped: bad stats pointer or SetHealth faulted
} DevKillStats;

// Each returns NULL on success or a short error message.
const char *devcheat_gold(int amount, int *cash_after);
const char *devcheat_killall(DevKillStats *st);
const char *devcheat_god(int on);       // installs the SetHealth hook on first use
int devcheat_god_enabled(void);

#endif

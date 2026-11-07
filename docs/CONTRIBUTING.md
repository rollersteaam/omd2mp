# Contributor guide: omd2mp and Orcs Must Die! 2 modding

This guide is for a software engineer who has never modded a game. It covers how
the game is put together, how this mod changes it, how the code in this repo works,
and how to extend it safely: more players, new rebalance rules, and later new
characters or abilities.

Read sections 1–3 to get oriented. Section 4 onward is reference material.

---

## 1. The 30-second mental model

OMD2 hard-codes "co-op = 2 players" in two places:

1. **Networking caps.** The Steam lobby is created with `maxMembers = 2`, and RakNet
   (the networking library) is started with room for 1 connection.
2. **A fixed-size array.** The game's `Session` object stores players in
   `Slot slots[2]`, and about 100 machine-code instructions index that array with the
   assumption that it has 2 entries.

We can't recompile the game, so the mod changes these places **at runtime, in memory**:

```
 research (Python, static analysis of the exe)
      │  tools/re/*.py        find every place that matters, write it to research/*.tsv
      ▼
 patch data (Python generator)
      │  tools/patchgen/build_patches.py
      │      → patches/patchset.json     human-reviewable list of every patch
      │      → loader/src/patchset_gen.h the same data as C arrays
      ▼
 loader (C, a 32-bit DLL named version.dll)
         loader/src/dllmain.c + patchcore.c
         Windows loads it into the game; it checks and applies the patches before game code runs

 rebalance data mod (Python → XML)
         mods/rebalance/build_rebalance.py → build/mods/omd2mp_<N>p/mod/game/onslaughts/
```

**The game's files on disk are never modified.** Deleting `version.dll` from the game
folder fully uninstalls the mod. Keep it that way: do not write tools that edit or
redistribute `OrcsMustDie2.exe`, and do not touch its Steam DRM wrapper (the `.bind`
section). Everything here works on the already-running game in memory.

## 2. Getting set up

| Need | Why | Where |
|---|---|---|
| Python 3.10+ with `pefile`, `capstone`, `numpy` | research tools, patch generator, full test suite | the venv at `Desktop/rea/venv` already has them: `rea/venv/Scripts/python.exe` |
| CMake ≥ 3.16 and VS 2019 Build Tools (x86 C compiler) | builds the 32-bit loader DLL | already installed on the dev machine |
| any `gcc` (optional) | fast host build of the C unit tests | Strawberry Perl's gcc works |
| The game + the "Orcs Must Die! 2 Workshop Tool" (Steam) | the exe we analyse; the Workshop Tool ships the wave/character XML sources | `steamapps/common/` |

```bash
# everything, every time before you commit
rea/venv/Scripts/python.exe tests/run_tests.py

# build the loader DLL (32-bit!)
cmake -S . -B build/msvc -G "Visual Studio 16 2019" -A Win32
cmake --build build/msvc --config Release        # -> build/msvc/Release/version.dll

# build the 4-player wave rebalance
python mods/rebalance/build_rebalance.py --players 4

# install / uninstall into the game folder (only writes version.dll + omd2mp.ini)
python tools/install.py
python tools/install.py --uninstall
```

The real game exe is `steamapps/common/Orcs Must Die 2/build/game/OrcsMustDie2.exe`;
`build/release/OrcsMustDie2.exe` is only a small Qt launcher. `install.py` accepts the
Steam folder or the game folder and picks the exe whose PE stamp is `0x51DD91A9`.

## 3. Repository map

| Path | What it is |
|---|---|
| `tools/re/pe32.py` | The reverse-engineering toolkit: PE loading, string search, cross-references, disassembly, function bounds, C++ RTTI/vtable recovery. Every other tool imports it. |
| `tools/re/slotscan.py` | Finds every instruction that touches `Session.slots[]` → `research/slot_sites.tsv`. |
| `tools/re/bindings.py` | Dumps the 374 script-callable natives (`Net_*`, `Character_*`, `Gear_*`…) → `research/bindings.tsv`. |
| `tools/re/srcpaths.py` | Lists source-file paths embedded in the exe (that's how we know it uses RakNet 4.0). |
| `tools/patchgen/build_patches.py` | The patch generator. Defines every patch; checks original bytes against the exe; writes JSON + C. |
| `patches/patchset.json` | Generated. The reviewable truth: one record per patch, with original bytes, new code and a `why`. |
| `loader/src/patchset.h` | C data model for patches. |
| `loader/src/patchset_gen.h` | Generated from the JSON. Never edit by hand. |
| `loader/src/patchcore.{h,c}` | Platform-independent patch engine: verify, encode, render caves/sites. Unit-tested on the host. |
| `loader/src/dllmain.c` | The Windows part: `version.dll` export forwarding, ini/log, memory protection, the C hook. |
| `loader/src/devproto.{h,c}` | Dev console protocol: tokenising, token → Scaleform value, value → JSON. Platform-free, unit-tested. |
| `loader/src/devcall.{h,c}`, `devcon.{h,c}` | Dev console game side: calling script bindings, the per-frame hook, the named pipe (§12). |
| `tools/devcon.py` | Dev console client: REPL, `state`, `watch`, `soak` (§12). |
| `mods/rebalance/build_rebalance.py` | Generates the wave rebalance data mod. |
| `tests/python/`, `tests/c/` | Unit tests; `tests/run_tests.py` runs both. |
| `dist/omd2mp.ini` | Default loader settings shipped next to the DLL. |
| `docs/DISCONNECT_FIX_PROPOSAL.md` | Analysis of the co-op LAN disconnects and the proposed `netfix` patch. |
| `research/` | Generated research tables, committed so reviewers can diff them. |

---

## 4. Primer: reading a 2013 Windows game binary

You need surprisingly little x86 knowledge. These are the facts this project relies on.

- **32-bit, fixed base.** `OrcsMustDie2.exe` loads at `0x400000` every time (no ASLR).
  So an address like `0x45704D` in this repo is a real runtime address. The loader checks
  the base and the PE `TimeDateStamp` (`0x51DD91A9`); a different build makes it do nothing.
- **Code is readable as shipped.** The Steam DRM stub doesn't encrypt `.text`, so
  `pe32.py` disassembles the file directly with capstone.
- **MSVC C++ leaves breadcrumbs.**
  - RTTI type names (`.?AVRakPeer@RakNet@@`) let us map vtables to class names:
    `pe32.py rtti RakPeer`, `pe32.py vtable RakPeer`.
  - Assert and log strings name source files and functions. Start most investigations
    with `pe32.py strings <regex>` and then `pe32.py xref <string>`.
- **Statically linked libraries.** RakNet 4.0 is compiled into the exe, so its open
  source code is a map of what the machine code does. When you find a RakNet function,
  read the matching RakNet 4.0 source alongside the disassembly.
- **How MSVC indexes `slots[i]`** (with 24-byte slots at struct offset `0x38`):
  `lea eax,[i+i*2]` (×3) then `[session + eax*8 + 0x38 + field]` (×8). That is
  `0x38 + 24*i + field`. `slotscan.py` pattern-matches exactly these shapes (A/B/C/D/K/L,
  documented at the top of the file). A K-shape pointer can be stepped by `0x18` far from
  where it is formed: `Session::Broadcast` (0x4B2150, which sends every net-variable change)
  does it 47 instructions later. An earlier scanner only looked 11 instructions ahead, which
  silently broke every broadcast to players 2+. A missed site doesn't crash anything: the
  code just reads the old, empty array.

Useful commands:

```bash
P=rea/venv/Scripts/python.exe
$P tools/re/pe32.py strings "Lobby"          # find strings by regex
$P tools/re/pe32.py xref 0x00B9D108          # who references the Session global
$P tools/re/pe32.py func 0x4B1AA0            # disassemble the whole Session constructor
$P tools/re/pe32.py calls 0x457C30           # callers of Net_OtherIndex
$P tools/re/pe32.py rtti "FullyConnected"    # classes + vtable sizes
$P tools/re/bindings.py "^Net_"              # script natives by regex
```

## 5. How the game's networking fits together

```
Steam lobby (ISteamMatchmaking)  ──►  Lobby2 client (custom, Lobby2Client_Steam_Impl.cpp)
                                          │ members join → host starts RakNet
RakPeer (RakNet 4.0, global 0xB9D0B8) ── SocketLayerOverride ──► ISteamNetworking P2P packets
   └─ FullyConnectedMesh2 plugin (0xB9D0C8): host election / peer mesh
Session (global 0xB9D108, malloc 0x36058 bytes)
   +0x38  Slot slots[2]   24 bytes each: RakNetGUID (+0x00), state ptr (+0x10), flags (+0x14..0x17)
   +0x68  dirty flag   +0x6C local slot   +0x70 player count
```

The wire format already encodes a slot count (up to 127), so the work is in **local**
assumptions, not the protocol.

Two-player assumptions that are easy to miss (all patched; `test_two_slot_limits_all_covered`
lists every site):
- **8-bit compares.** `tools/re/slotscan.py` only finds 32-bit `cmp r,1/2`. Thirteen loops
  bound slots with `cmp r8,1` / `cmp r8,2` (e.g. Join `0x4B267A`, PlayerJoined `0x4B2791`,
  the 0x84 handler `0x4B2D4D`). Missing them meant players 3 and 4 never joined or counted.
- **Per-slot u64 arrays.** `Session+0x00` voiceTime[2] and `+0x10` recvTime[2] are indexed
  by slot. Slot 2's recvTime would overwrite the pointer at `+0x24`. They are moved past the
  slots (`OMD2MP_VOICE_TIME_NEW` / `OMD2MP_RECV_TIME_NEW`), and the session allocation grows to
  `OMD2MP_SESSION_SIZE`.
- **Static NetVars[2]** at `0xBE7D90` (0x100C bytes each; ctor `0x502740`) holds per-player
  network variables, including ready-up (`Level_ReadyUp` → `Net_VariableSetBool`). Index 2
  aliased the global block at `0xBE9DA8`. Every indexed access now points at a loader-owned
  `g_netvars[16]` (`SYM_NETVARS` in the patch set, resolved by `omd2mp_symbol`).

## 6. The patch pipeline in detail

### 6.1 Patch kinds

Every record in `patches/patchset.json` has `id`, `group`, `va`, `orig` (hex bytes that
must be present) and `why`.

| kind | What the loader writes | Example |
|---|---|---|
| `imm` | an immediate inside the original instruction; value = `expr` evaluated with `N` = players | `push 1` → `push N-1` at `0x456F1E` |
| `float` | a 4-byte float constant, only if nothing else reads it (the generator refuses pooled constants) | none in the current set |
| `tramp` | `jmp cave` over ≥5 bytes (leftover bytes become `int3`); the cave runs re-encoded copies of the stolen instructions, then `jmp` back | `cmp [esi+eax*8],0` → `cmp [esi+eax*8+0x36030],0` |

**Why trampolines?** Moving `slots[]` to the end of a bigger allocation means adding
`delta = 0x36030` to displacements. A displacement that fits in 1 byte (`+0x38`) becomes
4 bytes (`+0x36068`), so the instruction grows and no longer fits in place. We move the
instruction into newly allocated executable memory (the *cave*) and jump there and back.

Trampoline rules that `tramp()` enforces (each one exists because breaking it crashes the game):

1. **Only steal whole instructions**, at least 5 bytes (the size of `jmp rel32`).
2. **No branch may land inside the stolen bytes**, except at the first one. Otherwise
   that code would jump into the middle of our `jmp`. The generator collects every direct
   branch target plus every relocation into `.text` and refuses such sites. If a loop
   head follows the site, it retries with `back=1..3`, starting the steal *before* the
   site instead.
3. **Relative branches are re-targeted.** `call`/`jmp`/`jcc` inside the cave get
   `fixups` (cave offset → absolute target). The loader computes the rel32 once it
   knows the cave address. Short `jcc` is widened to the near form.
4. **Values that depend on N inside stolen bytes** become `cave_imms`. The constructor's
   `mov edi, 1` loop counter is one.
5. **Hooks into C** wrap the call in `pushad; pushfd … popfd; popad`, so the game's
   registers and flags are untouched.

### 6.2 The C hook

`Session::Reset` (`0x4B1CE0`) and the client deserializer (`0x4B2A50`) initialise the two
slots with unrolled code instead of a loop, so there is no loop bound to patch. We let
them initialise the *old* slot 0 as usual (which is now just a template). Then
`hook_replicate_slots` copies that template into `slots[0..N)` at the new location.
See `omd2mp_replicate_slots_impl`.

### 6.3 Adding or changing a patch

1. Investigate with `pe32.py` until you can write down the original bytes and why they
   matter.
2. Add a call in `Gen.build()` (or teach `slotscan.py` a new shape). Use a precise `why`;
   it ends up in the JSON that reviewers read.
3. Run `build_patches.py` (needs the exe). It **exits** if the original bytes don't
   match, a site overlaps another, or a steal window contains a branch target.
4. Run `tests/run_tests.py`. If you only changed the C emitter, `build_patches.py
   --emit-only` re-renders the header from the JSON.
5. Commit `patchset.json`, `patchset_gen.h` and any `research/*.tsv` changes together.
   The test `test_header_matches_json` fails if they drift.

## 7. The loader

`loader/src/dllmain.c` is short on purpose. It runs inside `DllMain` before the game's
entry point, so no game thread exists yet and patches can't race the game.

1. It forwards all 17 `version.dll` exports to the real system DLL through naked
   `jmp [ptr]` stubs.
2. It reads `omd2mp.ini` (`players`, `groups`, `enabled`).
3. It checks the exe's base and timestamp.
4. **Pass 1** checks the original bytes of every enabled patch. **Pass 2** renders every
   cave and site into buffers. **Pass 3** writes them. Any failure in pass 1 or 2 means
   **nothing is written**. A half-patched game is far more dangerous than an unpatched one.
5. It logs every step to `omd2mp.log` next to the exe. Look there first when something
   doesn't work.

After patching, if `[dev] enabled=1`, it starts the developer console (§12). That also
happens with `[omd2mp] enabled=0`, so you can inspect an unpatched game.

Only `patchcore.c` (and the dev console's `devproto.c`) contain logic worth unit-testing,
and neither has Windows dependencies. That is why the tests can run it on the host with any C compiler.

## 8. Rebalancing

| Lever | Mechanism | Where |
|---|---|---|
| Gold | In multiplayer the game multiplies all gold by `NetMoneyFactor` (vanilla 0.5 = 1/2). The `economy` patch sets the default to `1/N`, so total team gold equals vanilla 2-player. A `NetMoneyFactor` line in `game.cfg` still overrides it. | `economy.net_money_factor` |
| Enemies | `<Entity Coop="">` entries spawn only in co-op: one extra player's worth. For N players `build_rebalance.py` multiplies their `Count` by `(N-1) × coop_scale`. N=2 with scale 1.0 is byte-identical to vanilla, and a test checks that. | `mods/rebalance/` |

Tuning ideas that are not done yet and need playtesting:

- A `coop_scale` below 1.0 for 6–8 players, because traps scale super-linearly with
  more people building.
- Scaling `Break` (the time between waves).
- Per-map overrides.

Add each as a pure function in `build_rebalance.py` with a unit test that pins the N=2
identity.

## 9. Tests: what they prove and what they don't

`python tests/run_tests.py` runs:

- **C (`tests/c/test_patchcore.c`)**:
  - Encoding edge cases.
  - rel32 math for calls, jumps and hooks.
  - All-or-nothing failure codes.
  - Slot replication bounds.
  - For **every generated patch and N = 2..8**: it renders; the cave's last jump returns
    to `va + steal`; no two sites overlap; at N=2 every value patch reproduces the vanilla
    bytes. Sizing patches and `netfix` are excluded, since they change behaviour on purpose.
- **Python (`tests/python/`)**:
  - Structural invariants of `patchset.json`.
  - With capstone: every cave decodes cleanly, every fixup is a branch operand, every
    relocated slot access moved by exactly `delta`, and no slot rewrite touches a stack
    address (`[esp+…]`/`[ebp+…]`).
  - The header matches the JSON.
  - The rebalance transform.

The tests only read files in this repo, never the game. They prove the patch data is
**internally consistent and rendered correctly**. They **cannot** prove the patched game
*behaves* correctly. For example, a slot site the scanner never found would pass every
test and still break 3+ player games. That is what in-game testing is for (see the README
status table).

The tests have already caught real bugs:

- **In-game, the original `economy` patch broke the HUD, aiming and ragdolls.** It rewrote
  the float at `0x857810` from 0.5 to `1/N`, assuming that was NetMoneyFactor's default.
  It is MSVC's pooled `0.5f`, read by ~60 instructions (screen centre, HUD anchoring,
  ragdoll physics). With 4 players every `x * 0.5` became `x * 0.25`: the crosshair moved
  to the top-left, the minimap to the bottom, and ragdolls stretched. The fix patches the
  one reader instead (`0x458042` in the gold scaler `0x458020` gets `1/N` from an
  immediate). `build_patches.py` now refuses a `float` patch on any constant with more
  than one reader, and `test_shared_float_constants_untouched` guards it. **Lesson:
  never patch a value in `.rdata` without counting its readers.**

- The C emitter silently dropped `cave_imms`, so the constructor would have initialised
  only 2 slots.
- `slotscan.py` treated `lea eax,[esp+0x38]` (the SEH frame setup in the Session
  constructor) as a slot access. Relocating it would have crashed on the first
  exception unwind.
- The rebalance missed 12 maps whose files are named `*.Onslaught` (capital O).

## 10. Toward character and ability mods

Not in scope for the multiplayer mod, but the groundwork is here:

- **Data first.** The Workshop Tool's `SourceXML/ensembles/*.xml` (120 files) define
  traps, weapons, mobs and heroes (for example `DLC003_WeaponStoneStaff.xml`). New
  abilities that reuse existing behaviour classes are XML-only and ship like the
  rebalance mod.
- **Script natives.** `research/bindings.tsv` maps every UI/script function name to its
  handler address. The `Character_*`, `CharacterLocal_*`, `CharacterProto_*`, `Gear_*`
  and `Player_*` families are the entry points the UI uses to list, buy and equip
  heroes and gear. When a new hero doesn't appear in a menu, start reading there.
- **New behaviour** (a genuinely new ability type) needs code. Find the class that
  implements a similar ability with `pe32.py rtti <name>`, read its vtable, and hook it
  with the same trampoline machinery. Add a new hook id next to
  `OMD2MP_HOOK_REPLICATE_SLOTS`.
- **Phase 2 of multiplayer** is the same kind of work. `Net_OtherIndex` (`0x457C30`)
  returns "the other player" and feeds the buddy HUD. With 4 players it needs to become
  a list. Its 5 callers are listed by `pe32.py calls 0x457C30`.

## 11. Workflow and review checklist

- Branch, change, then run `tests/run_tests.py`. Regenerate artefacts in the same commit
  as the code that produces them.
- A PR that changes patches should paste the changed `patchset.json` records and explain
  each `why`.
- Reviewers should check:
  - Does every new site have a provable link to the Session? (A `review` tag in
    `slot_sites.tsv` means *no*.)
  - Is N=2 still vanilla?
  - Does anything write to the game's files on disk? It must not.
- In-game results go in the PR: the `omd2mp.log` excerpt, player count, map, wave reached.

## 12. The developer console (automated testing in a running game)

Problems that only show up at high rounds in co-op cannot be unit-tested. The dev console
lets a script drive a running game and read its state without anyone at the keyboard.

### 12.1 Turning it on

In `omd2mp.ini` next to the exe:

```ini
[dev]
enabled=1
pipe=omd2mp-dev      ; pipe name; change it to run two game instances on one machine
allow_write=0        ; 1 enables raw memory writes (patch experiments only)
```

`omd2mp.log` will then contain `dev: console on ...`. Open any menu once: the console can
only call script bindings after it has seen the UI make a call.

### 12.2 Using it

```bash
python tools/devcon.py                          # prompt: type raw commands, or `state`
python tools/devcon.py call Net_PlayerCount     # one raw command
python tools/devcon.py state                    # one JSON snapshot
python tools/devcon.py watch --interval 1 --csv run.csv
python tools/devcon.py soak --timescale 4 --until-wave 40 --ready-up --log soak.jsonl
python tools/devcon.py skip --to 10                 # fast-forward to wave 10 (§12.6)
python tools/devcon.py cheat gold 5000              # cheats (§12.6)
python tools/devcon.py cheat killall
python tools/devcon.py cheat god on
python tools/devcon.py call Level_ReadyUp true      # start the next wave now
```

Bindings take the same arguments as in the game's UI script. `Level_ReadyUp` takes a
bool: with no argument it sets "ready" to false, which does nothing visible.

`soak` is the high-round test.
- It optionally speeds the game up and presses ready while a wave countdown is running.
- It samples state every `--interval` seconds into the JSONL log.
- It stops at `--until-wave`, on a win, at `--max-minutes`, or if the game goes away.
  It always restores time scale 1 on exit.
- It records these events:
  - `disconnect`: `Net_PlayerCount` dropped.
  - `silent_peer`: `Net_GetTimeSinceLastReceived(i)` exceeded `--silence`, 10 s by default.
  - `connection_lost`: the pipe closed, meaning the game exited or crashed.
  - `wave`: a new wave started.
- The exit code is 0 if the run reached its goal, and 2 otherwise.

**Co-op:** run `soak` on every machine in the session. Game speed is per process and not
synchronised. Give every peer the same `--timescale`, or keep it at 1 when you want a
faithful reproduction of the disconnect. Compare the JSONL logs from both sides.

### 12.3 Protocol

The client sends one line of space-separated tokens. A token in `"double quotes"` may
contain spaces and the escapes `\" \\ \n`. The reply is one line of JSON: either
`{"ok":true,...}` or `{"ok":false,"error":"..."}`.

| Command | Reply | Notes |
|---|---|---|
| `hello` | `name, version, ticks, ui, players, slots_patched, allow_write` | answered on the pipe thread, works even while the game is loading |
| `ping` | `ticks` | frames seen by the hook; if it stops rising, the main thread is stuck |
| `call <Binding> [args]` | `returned, result:{type,value}` | any name in `research/bindings.tsv`; args: `true/false/null/undefined`, numbers, otherwise strings (quoted tokens are always strings) |
| `session` | `session:{addr, local_slot, player_count, slot_base, slots:[{guid, system_index, state, flags}]}` | uses the relocated slot array when the `slots` group is applied |
| `timescale <x>` | `timescale` | 0 < x ≤ 64, 1 = normal (Vision `VisTimer_cl::SetSlowMotion`) |
| `read <addr> <len>` | `addr, hex` | len ≤ 1024, checked with `VirtualQuery` |
| `write <addr> <hex>` | `written` | only with `allow_write=1`; logged |
| `cheat gold [n]` | `added, cash` | adds n gold (default 10000, negative allowed) to the local player; logged |
| `cheat killall` | `entities, characters, killed, players, dead, allies, not_owned, overflow, faults` | kills living enemies this peer owns; logged |
| `cheat god [on\|off]` | `god` | no argument reports the state; logged |
| `log <text>` | | writes `dev-client: <text>` to `omd2mp.log`, to mark test phases |

All commands except `hello` run on the game's main thread, one per frame. The pipe accepts
local clients only (`PIPE_REJECT_REMOTE_CLIENTS`), one at a time.

### 12.4 How it works

- **Per-frame hook.** The game application's vtable (`0x868DD8`) slot 16 is `Run()`
  (`0x4A77B0`), called once per frame. The console swaps that pointer for `run_hook`, which
  runs at most one pending command and then calls the original.
- **Script bindings.** The UI dispatcher (`0x48E500`, reached only through vtable slot
  `0x866290`) looks a name up with `0x48DE10` and calls the handler as
  `handler(ui_self, GFxValue *args, unsigned argc, 0)` (cdecl).
  - A second hook on that slot records `ui_self`; this is why a menu must be opened once.
  - A handler returns its value through `movie->vtable[50]` (`SetExternalInterfaceRetVal`),
    where `movie = *(ui_self + 0x40)`. `devcall.c` routes that slot through a function that
    captures the value during a console call and forwards it at all other times.
- **Values** are Scaleform GFx 3 `GFxValue`s, 16 bytes on x86: `{iface, type, value@+8}`.
  The types are 0 undefined, 1 null, 2 bool, 3 number (double), 4 string, 5 wide string,
  6 object, 7 array, plus the managed flag `0x40`.
- **Safety.**
  - Every pointer the console replaces is checked against its expected original first; on
    a mismatch it logs and stays off.
  - Each command runs inside `__try/__except`, so a bad `call` or `read` returns an error
    instead of crashing the game. The game may still be left inconsistent, though: treat
    the session as tainted.
  - If the main thread does not pick up a command within 5 s, the command is cancelled.

### 12.5 What is verified

| Part | Verified by |
|---|---|
| protocol (`devproto.c`) | `tests/c/test_devproto.c` (gcc and MSVC) |
| client logic, soak detection, CSV, `skip` | `tests/python/test_devcon.py`, against a fake console |
| cheat decisions (who `killall` kills, god-mode clamp) | `tests/c/test_devproto.c` |
| client ↔ real Windows named pipe | manual self-test with a local stdlib pipe server |
| hooks, binding calls, timescale in the game | in-game run, single player (`hello`, `state`, `timescale 4`) |
| cheats in the game | **not yet**: report `omd2mp.log` plus the JSON replies |

Open questions:
- Is `VIS_CFG_OnOff` "on" really 1?
- Do any bindings return values through a movie method other than slot 50? Those would
  show `returned:false`.
- Does changing the time scale on one co-op peer cause a desync?

### 12.6 Cheats

The retail exe still lists `CheatGold`, `CheatChaosGems`, `CheatPower`,
`CheatKillAllEnemies` and `CheatKillAllMinions` in its input-action table. That table is
`{hash, name, flags, code, 1}` × 0x86 entries at `0x8EB3DC`, with names registered at
`0x81D700`. The cheats have codes 0x82 to 0x85. `CheatKillAllMinions` falls one entry past
the end of the table, so it was already dead. No code polls any of these codes: the
player-input function `0x4DA150` checks Rotate (0x7C) and Use (0x7D) by code and stops
there. The handlers were compiled out, so `devcheat.c` rebuilds the three that matter from
the game's own functions.

| Cheat | How |
|---|---|
| gold | `AddCash` `0x49B510` (eax = health component, edi = amount): the same call kill rewards use, so the HUD updates through `GoldChanged` |
| killall | walk Vision's entity table (`vision90.dll` `VisElementManager_cl<VisBaseEntity_cl*>`, data at `elementTable+8`, `g_iElementCount` entries). Keep entities whose `+0x310` component has vtable `0x8688E8` and points back at them (`+0x40`). Call `SetHealth(comp, 0, 0, 0x8EA8BC, -1, 0)`, which is the call the game's own out-of-bounds kill (`0x4E02F7`) makes |
| god | 7-byte inline hook on `SetHealth` `0x49AE30` (`void __stdcall (comp, float hp, attacker, dmg, -1, flag)`), installed on the first `god on`. For player characters (entity `+0x39C` bit 0) it refuses decreases |

Who `killall` skips (`dev_kill_verdict`, unit-tested):
- **players:** player characters.
- **dead:** health component `+0x4C` (the *alive* flag: set at spawn `0x49ACF1`, cleared at death `0x49B849`) is 0, or health ≤ 0. Half-spawned enemies also have it at 0, so they are left alone.
- **faults:** victims skipped because their stats pointer (entity `+0x2D4`) was bad or `SetHealth` raised an exception; the rest are still killed.
- **allies:** same team as the local player (component `+0x50`; the same field `IsAlly` `0x4ACB90` compares).
- **not_owned:** the entity's net id (`+0x39E`, owner slot in bits 10+) names another peer.
  The death handler `0x49C000` only broadcasts deaths of entities the local peer owns, so in
  co-op run `killall` on the peer that owns the enemies (expected: the host) and check
  `not_owned` in the reply.

`skip --to N` speeds the game up (8× by default). It then repeats kill all, `Level_ReadyUp
true` and sleep until `Level_GetCurrentWaveNumber` reaches N, and it restores time scale 1
when it stops. A wave's enemies spawn over time, so it keeps killing until the wave ends.
Kills have no attacker, so they award no gold; use `cheat gold` for that.

Limits:
- God mode also refuses the out-of-bounds kill, so a god-mode player who falls out of the
  level stays there. Turn it off to respawn.
- ChaosGems and Power have no surviving code to say what they did, so they are not
  reimplemented.

### 12.7 When a command faults

Commands run inside `__try`. A fault replies with the exception code and the faulting
address as `module+offset`. For an access violation it also gives the address read or
written, plus up to 12 stack words that look like return addresses into the exe's `.text`
(a cheap backtrace). The same line goes to `omd2mp.log`. For example:

```
exception 0xc0000005 at 0x4e1a2b (OrcsMustDie2.exe+0xe1a2b) reading 0x10; stack 0x4e0f11 0x4a12c0 ... while running 'cheat'
```

`skip` stops on the first error and prints `error in <step> at wave <n>: <that line>`.
Use `pe32.py func`/`dis` on the addresses to find the function.

## 13. Mock players (testing 4-player co-op alone)

`[mpdebug] enabled=1` adds `party_size-1` fake players to a party you host. They go through
the real join path, so the lobby leaves "Connecting... waiting for other player" and the
session, slots, per-player arrays and every host→client message run for N players. Real
memory errors in that code show up without human testers. Code: `loader/src/mockpeer.c`
(game side), `loader/src/mockbrain.c` (decisions, unit-tested in `tests/c/test_mockbrain.c`).

### 13.1 How it works

The loader hooks RakPeer's vtable (`0x83375C`, each entry checked against its exported
`RakPeer::` method first):

| vtable | method | mock behaviour |
|---|---|---|
| 9 | NumberOfConnections | real + connected mocks (so `Net_IsMultiplayer` is true) |
| 19, 20 | Send (BitStream / char*) | to a mock: recorded, dropped, returns a fake receipt. Broadcast: recorded for each mock, then sent to real peers |
| 23 | Receive | runs the mock brain once per drain (≥15 ms), then the real Receive |
| 26 | CloseConnection | on a mock: marks it dropped (no notification, as in RakNet) |
| 27 | GetConnectionState | IS_CONNECTED (2) for connected mocks |
| 40-42 | Get{Average,Last,Lowest}Ping | 30, 40, 50 ms for mocks 1-3 |
| 67, 69 | PushBackPacket, AllocatePacket | called (not hooked) to inject packets |

Mock *i* has GUID `0xFEEDF00D0000000i`, systemIndex 0xFFFF (so RakPeer never indexes its
arrays with it) and address 10.77.0.*i*. Injected packets go through the game's normal net
loop (`0x457150`) and dispatch table (`0xB9BAD8`), and RakNet plugins (FullyConnectedMesh2)
see them too. The brain does, per mock:

1. Host, session and accepting (`[0xBBB99C]+8`, the check in `Session::OnConnect`
   `0x4B23F0`): wait `i*join_delay_ms`, then push `0x13` ID_NEW_INCOMING_CONNECTION. The host
   assigns a slot (state 3), sends `0x83` session data and calls Join.
2. Once a slot holds the GUID, push `0x84` (loaded ack, sets slot+0x15). Then
   PlayerJoined/`playerjoin` fires.
3. When the slot state is 3 or 4 and stays there for `settle_ms`, push `0x86 [5]` (ready;
   the UI checks state == 5). Repeat 0x84 / 0x86 [5] whenever the host resets them,
   e.g. on level load.
4. Answer the host's net variables. The lobby script (`Net_VariableGetBool(i, "ready")` over
   `Net_PlayerCount`) waits on every player's `ready` **net variable**, not on the slot state.
   The host sends each new client its whole table (`0x87`) and its profile (`0x8e`, ~1.5 KB).
   Each mock answers with its own `0x87`, a copy of the host's table with its own `ready`,
   and a copy of the host's `0x8e`. Later host changes (`0x88`) are copied too. So a mock
   looks like a second copy of you, character included. `[mpdebug] ready` decides when a
   mock's `ready` turns on: `follow` (when yours does), `auto`, or `manual`.
5. On a new session object, mocks that are still connected wait for a slot and connect
   again if none comes. With no session (back to the menu), all mocks reset.

**Net variables** (`loader/src/netvars.c`, tests in `tests/c/test_netvars.c`) are a
per-player table of `key -> bytes`. The key is the hash `0x40E840` of the name the UI passes:
ELF-style with an *arithmetic* shift, so keys with the top bit set keep their top nibble.
For example, `ready` = `0x0078B7B9`. A bool is one byte.

| id | handler | body |
|---|---|---|
| `0x87` | `0x501CA0` → clear + `0x501900` | repeat { u32 key (big-endian); 0 ends; u8 len; bytes } |
| `0x88` | `0x501CE0` → `0x501BB0` | u32 key; 1 bit has-value; if set: u8 len; bytes. The bit leaves the rest unaligned |

**Level load (`0x89`).** When everyone is ready, the lobby's `LoadLevel()` calls `Level_Load`
(`0x423A10` → `0x4A73E0`). On a multiplayer host with clients, that does *not* load yet: it
broadcasts `0x89 {bit go=1; u32 ×3 level info}` and sets `+0x11A9` (waiting). The host only
starts loading when a client answers `0x89 {bit 1}` (`0x80`). The handler `0x4A71A0` then sets
`+0x11A8` and `0x4A77B0` enters the load state. `0x89 {bit 0}` is a cancel (level not found)
and resets through `0x4A72E0`, which clears `ready`. Mocks have nothing to load, so they ack
straight away. The client's ack goes through `Session::SendToSlot(&slots[0])` via
`add r, 0x38`. That shape is the `K:slot0+0x38 send` kind in `slotscan.py` (5 sites).

### 13.2 Using it

```
python tools/devcon.py mock                 # host flags + one line per mock, with message ids received
python tools/devcon.py mock log 50          # the last 50 host->mock messages (id, size, first bytes)
python tools/devcon.py mock drop 2          # mock 2 disconnects (0x15); `drop 2 lost` = 0x16 timeout
python tools/devcon.py mock join 2          # a dropped mock joins again after join_delay_ms
python tools/devcon.py mock ready 3         # mock 3: ready net variable on + slot state 5 (stops following you)
python tools/devcon.py mock ready 3 0       # ...and off again
python tools/devcon.py mock vars            # your net variables as the mocks copy them (hash, bytes)
python tools/devcon.py mock send 3 8705ff   # push any bytes as mock 3 (replay a captured client message)
python tools/devcon.py mock clear           # reset the counters
```

The log records each mock's state changes and the first time each message id arrives.

### 13.3 Limits

- **Host only.** Mocks never act as the host, and a real client sees no mocks.
- **No hero, no input.** Mocks answer 0x13/0x84/0x86/0x87/0x88/0x89/0x8e and nothing else. If
  the host waits on another message, `mock status` shows which ids arrived. Use
  `mock send` to try a reply, then add it to the brain.
- **The lobby shows two portraits.** The level-select movie has exactly two player clips
  (`mcLocal`, `mcRemote`), and the in-game party HUD uses `Net_OtherIndex`, which means
  "the other player". Showing players 3 and 4 needs those Scaleform movies changed (Phase 2).
- The profile handler (`0x47D3C0`) queues incoming `0x8e`..`0x9a` messages in a
  **two-entry** pending list (`[obj+0x2C]`, count `+0x34`; `cmp eax, 2`). A third message
  that arrives before the list drains is dropped (and leaked). Mocks join 1.5 s apart, so
  this rarely matters for them. Three real clients joining at once could hit it.
- The Steam lobby itself has only you in it. Anything that reads the Steam member list,
  not the session, still sees one player.

## 14. Network trace (diagnosing real multiplayer games)

Mock players never parse what the host sends them. So a bug in a message that only real
clients read passes every mock test. An example is the host's 0x83 session-data writer
(`0x4B38D0`), which walked the old slot array and sent clients an empty slot list. Real
games then hang on "Waiting for other player..." (host) and "Connecting..." (client). To
see what actually goes over the wire, turn on the trace on **every** machine:

```ini
[nettrace]
enabled=1
```

Each game writes `omd2mp-net.log` next to `omd2mp.log` (`loader/src/nettrace.c`). The trace
hooks RakPeer Send/Receive (it chains after the mocks, so it works with them too) and
writes three kinds of line:

- `SEND` / `RECV`: message ID and name, a per-ID counter, length, the peer GUID (for a
  broadcast, the excluded GUID), and the first bytes. Handshake and session messages
  (RakNet 0x09..0x1A, game 0x83/0x84/0x86/0x89/0x8c) dump 160 bytes and other messages 32.
  Each (direction, ID) logs its first `full=` messages, then every `every=`th.
- `SESSION`: on any change, is_host, connection count, the accept flag, `[0xB9D10C]`,
  localSlot, playerCount, and every slot (guid, state, flags) in both the vanilla array
  (`old[]`, +0x38) and the relocated one (`new[]`, +0x36068). A slot that changes only in
  `old[]` is a site the patch set missed.
- `COUNT`: messages per ID over the last `summary_s=` seconds. A summary is written only
  when something was sent or received, so a log with no `COUNT` lines had no traffic at all.
- `CONNECT`: every RakPeer::Connect call (vtable slot 12), with its address and result. Only
  a client connects: the join handler (`0x456980`) calls Session::Connect (`0x4B1E00`) once
  the Steam lobby join is done and `slots[localSlot].state == 1`.
- `LOBBY2`: every Steam lobby (Lobby2) result the game handles. The game's
  Lobby2Callbacks subclass (vtable `0x85E388`) overrides four slots: 19 (`0x456990`,
  room join / member joined → Session::Connect), 61 (`0x456AD0`, overlay invite
  accepted), 64 (`0x456B10`, lobby search result) and 165 (`0x456BA0`). Every other slot
  is the default `0x52E810`, which tail-jumps to slot 165, so the four hooks see every
  result. Each line has the message vtable (the exe has no RTTI), `result`, any
  `mov eax,imm; ret` getters among the first 12 vtable entries (the message ID), the
  join address string for slot 19, and the first 0x70 bytes. It is followed by `SESSION`
  snapshots taken before and after the game's handler runs. For slot 61 it also logs
  the path taken: with no Session the game only sends a raw JoinRoom (`0x4575B0`); with
  one, it only stores the lobby id at session+0x28.

Reading a failed join: the host should log `RECV NEW_INCOMING_CONNECTION`, a `new[1]` slot
with the client's GUID, and then `SEND game:SESSION_DATA` (0x83) to that GUID. The client
should log `RECV CONNECTION_REQUEST_ACCEPTED`, then the 0x83, then a `SESSION` with its own
`localSlot`. The first step that is missing on either side is where to look.

A client with a `SESSION CLIENT` slot at `state=1` and no `CONNECT` line never tried to
connect. That happened when the join gate's `slots[localSlot].state` read (`0x456A7E`,
`add eax,3; lea edx,[eax+eax*2]; cmp [ecx+edx*8],1`) still pointed at the old array:
`slotscan.py` only looked for the `+3` on the index register, not on its source.

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
  documented at the top of the file).

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

## 6. The patch pipeline in detail

### 6.1 Patch kinds

Every record in `patches/patchset.json` has `id`, `group`, `va`, `orig` (hex bytes that
must be present) and `why`.

| kind | What the loader writes | Example |
|---|---|---|
| `imm` | an immediate inside the original instruction; value = `expr` evaluated with `N` = players | `push 1` → `push N-1` at `0x456F1E` |
| `float` | a 4-byte float constant | `NetMoneyFactor` 0.5 → `1/N` at `0x857810` |
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

Only `patchcore.c` contains logic worth unit-testing, and it has no Windows
dependencies. That is why the tests can run it on the host with any C compiler.

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

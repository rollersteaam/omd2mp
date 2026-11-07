# omd2mp — Orcs Must Die! 2 multiplayer (4+ players) mod

Work in progress. Goal: raise OMD2 co-op from 2 to 4 players (designed for N up to 8),
rebalance for player count, and document the game so future mods can add characters and abilities.

## Status
| Area | State |
|---|---|
| Reverse-engineering toolkit (`tools/re/`) | done |
| Network/lobby limits mapped | done (see findings) |
| Player-slot array inventory (`research/slot_sites.tsv`) | done: 96 session sites |
| Patch set (`patches/patchset.json`, 145 patches) | generated, unit-tested, **not run in-game** |
| Loader DLL (`loader/`, builds `version.dll`) | built, unit-tested, **not run in-game** |
| Wave rebalance data mod (`mods/rebalance/`) | generator done, **not playtested** |
| Contributor guide | [`docs/CONTRIBUTING.md`](docs/CONTRIBUTING.md) |
| Co-op disconnect fix | proposal: `docs/DISCONNECT_FIX_PROPOSAL.md` (patch exists as group `netfix`, off by default) |
| Developer console (`tools/devcon.py`; `[dev] enabled=1`) | state reads, binding calls and time scale confirmed in-game (single player). Automated high-round `soak` with disconnect detection ([§12](docs/CONTRIBUTING.md#12-the-developer-console-automated-testing-in-a-running-game)). Cheats `gold` / `killall` / `god` and `skip --to N` are unit-tested but **not run in-game** ([§12.6](docs/CONTRIBUTING.md#126-cheats)) |
| Mock players (`[mpdebug]`; [§13](docs/CONTRIBUTING.md#13-mock-players-testing-4-player-co-op-alone)) | host-side fake clients join through the real RakNet/session path: confirmed in-game (slots reach state 5). They copy the host's net variables/profile and follow its "ready" (unit-tested, **not yet run in-game**) |
| Phase 2 (HUD / `Net_OtherIndex` for >2 players; lobby shows 2 portraits: `mcLocal`/`mcRemote` in the Scaleform movies in `data/gfx.ref`) | not started |

Run all tests: `rea/venv/Scripts/python.exe tests/run_tests.py`.

## Sending a build to testers
`python tools/package.py` runs the tests, builds the Win32 loader and writes
`build/package/omd2mp-<git describe>.zip`. Testers need no Python: they unzip it and run
`install.bat` (finds the game through Steam), `collect-logs.bat` (zips `omd2mp.log` onto their
Desktop to send back) and `uninstall.bat`. The packaged `omd2mp.ini` always has `[dev]` and
`[mpdebug]` off. The tool refuses a tree with uncommitted changes (`--allow-dirty` overrides), so each zip
maps to a commit. Everyone in a party must run the same zip. The tester scripts live in `dist/tester/`.

## Key findings (OrcsMustDie2.exe, TimeDateStamp 0x51DD91A9, fixed base 0x400000)
- Networking: RakNet 4.0 (static, symbols exported) over Steam P2P via a `SocketLayerOverride`; Steam lobbies through a custom Lobby2 client.
- Globals: `0xB9D0B8` RakPeer, `0xB9D0C0` Lobby2 msg factory, `0xB9D0C4` Steam Lobby2 client, `0xB9D0C8` FullyConnectedMesh2, `0xB9D0AD` is-host, `0xB9D108` Session.
- 2-player limits:
  - Steam lobby `maxMembers = 2` at `0x45704D`.
  - Host `RakPeer::Startup(1, …)` at `0x456F1E` and `SetMaximumIncomingConnections(1)` at `0x456F2D`; client `Startup(1, …)` at `0x4570C2`.
  - FCM2 auto-mesh disabled at `0x45795D`.
  - Session (`malloc(0x36058)`, ctor `0x4B1AA0`) embeds `slots[2]` at `+0x38` (24 bytes each: RakNetGUID, state at `+0x10`, flags `+0x14..0x17`), followed by `+0x68` flag, `+0x6C` local slot, `+0x70` player count. Loops bound with `cmp r,2` and `cmp r,1; ja`.
  - Unrolled slot resets in `0x4B1CE0` and `0x4B2A50` (client deserializer; wire format already supports up to 127 slots).
  - Gameplay layer: `Net_OtherIndex` (`0x457C30`) returns "the other player"; it has 5 callers (HUD etc.), which is Phase 2 work.
- Rebalance levers:
  - `NetMoneyFactor` (settable in `game.cfg`; default 0.5 loaded at `0x458042` in the gold scaler `0x458020`) scales gold in multiplayer. Never patch the 0.5f at `0x857810` itself: it is a pooled constant shared by the HUD, camera and ragdoll code.
  - 2,087 `Coop=""` wave entries in the Workshop Tool's `SourceXML/onslaughts`.
  - Official data mods load from `%LOCALAPPDATA%\Robot Entertainment\OMD2\MyMods\<name>\mod\game\{ensembles,onslaughts}` via the Mod Launcher (`EnableSteamWorkshop="yes" SteamWorkshopModsCRC=…`).
- Script/UI binding table: 374 natives, dumped to `research/bindings.tsv` (`tools/re/bindings.py`). Useful for character/ability modding.

## Tools
Python with `pefile`, `capstone`, `numpy` (the venv in `Desktop/rea/venv` works).
REA (`rea-agents` 6.8.0) was used for PE import/export evidence; no native decompiler backend is configured.

```
python tests/run_tests.py
cmake -S . -B build/msvc -G "Visual Studio 16 2019" -A Win32 && cmake --build build/msvc --config Release
python mods/rebalance/build_rebalance.py --players 4
python tools/install.py            # copies version.dll + omd2mp.ini into <Steam>/Orcs Must Die 2/build/game
python tools/package.py            # tests + build + zip for human testers -> build/package/omd2mp-<git>.zip
python tools/devcon.py soak --until-wave 40 --ready-up --log soak.jsonl   # needs [dev] enabled=1
python tools/devcon.py skip --to 10 ; python tools/devcon.py cheat god on # cheats: CONTRIBUTING §12.6
python tools/devcon.py mock                                  # mock players ([mpdebug] enabled=1): CONTRIBUTING §13
python tools/re/pe32.py strings|xref|dis|func|calls|rtti|vtable ...
python tools/re/bindings.py [regex]
python tools/re/slotscan.py
python tools/patchgen/build_patches.py
```

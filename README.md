# omd2mp — Orcs Must Die! 2 multiplayer (4+ players) mod

Work in progress. Goal: raise OMD2 co-op from 2 to 4 players (designed for N up to 8),
rebalance for player count, and document the game so future mods can add characters and abilities.

## Status
| Area | State |
|---|---|
| Reverse-engineering toolkit (`tools/re/`) | done |
| Network/lobby limits mapped | done (see findings) |
| Player-slot array inventory (`research/slot_sites.tsv`) | done, 100 session sites |
| Patch set generator (`tools/patchgen/build_patches.py`) | written, not verified in-game |
| Loader DLL, offline verifier | **not started** |
| Rebalance data mod | not started |
| Contributor guide | not started |
| Co-op disconnect fix | proposal: `docs/DISCONNECT_FIX_PROPOSAL.md` |

Nothing has been tested in-game.

## Key findings (OrcsMustDie2.exe, TimeDateStamp 0x51DE1E29, fixed base 0x400000)
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
  - `NetMoneyFactor` (default 0.5 at `0x857810`, settable in `game.cfg`) scales gold in multiplayer.
  - 2,087 `Coop=""` wave entries in the Workshop Tool's `SourceXML/onslaughts`.
  - Official data mods load from `%LOCALAPPDATA%\Robot Entertainment\OMD2\MyMods\<name>\mod\game\{ensembles,onslaughts}` via the Mod Launcher (`EnableSteamWorkshop="yes" SteamWorkshopModsCRC=…`).
- Script/UI binding table: 374 natives, dumped to `research/bindings.tsv` (`tools/re/bindings.py`). Useful for character/ability modding.

## Tools
Python with `pefile`, `capstone`, `numpy` (the venv in `Desktop/rea/venv` works).
REA (`rea-agents` 6.8.0) was used for PE import/export evidence; no native decompiler backend is configured.

```
python tools/re/pe32.py strings|xref|dis|func|calls|rtti|vtable ...
python tools/re/bindings.py [regex]
python tools/re/slotscan.py
python tools/patchgen/build_patches.py
```

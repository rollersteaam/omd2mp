# Proposal: co-op disconnects at high rounds (esp. LAN)

Status: **proposal only, not applied.** Found by static analysis of `OrcsMustDie2.exe`
(TimeDateStamp `0x51DE1E29`). Not yet reproduced or tested in-game.

## Symptom
Co-op sessions drop at high wave counts (typically Endless), more often on LAN.

## How networking works (relevant parts)
- Transport is RakNet 4.0 (statically linked) running over Steam P2P. A
  `SocketLayerOverride` sends every RakNet datagram with
  `ISteamNetworking::SendP2PPacket(..., k_EP2PSendUnreliable, channel 0)` (`0x538040`)
  and reads them with `IsP2PPacketAvailable` / `ReadP2PPacket` into a 1200-byte buffer (`0x5380f0`).
- Net init (`0x4577f0`) calls `SetOccasionalPing(true)`, `SetUnreliableTimeout(50)` and
  `SetTimeoutTime(15000, UNASSIGNED)`, all hardcoded.
- `NetworkConnectionTimeout` / `NetworkKeepAliveTimeout` in `data/startup/game.cfg` are
  **dead config**: those key strings don't exist in the exe, so editing them changes nothing.
- RakNet's MTU table (`0x833438`) is `{1200, 1200, 576}`. Robot already capped it for
  Steam's 1200-byte unreliable limit, so MTU/fragmentation is **ruled out**.

## Root-cause candidate (high confidence from code, untested)
In `RakPeer::RunUpdateCycle` (`0x602b10`):

```
0x602b3f  if (socketLayerOverride) {
0x602b7b      len = slo->RakNetRecvFrom(...)        ; reads ONE datagram
0x602b81      if (len > 0) {
0x602be7          ProcessNetworkPacket(...)
0x602bef          jmp 0x604729                      ; <-- returns from RunUpdateCycle
              }
          }
0x602bf4  ... rest of the update: ACKs, resends, reliability layer, timeouts ...
```

The update thread loop (`0x6047a0`) calls `RunUpdateCycle`, then waits up to 10 ms
(`0x6047cb push 0xa`).

That causes two problems:
1. **Receive is throttled to about one datagram per cycle.**
2. **Every cycle that receives a datagram skips the send/ACK half of the update.**

Under sustained inbound traffic (lots of mobs replicated at high waves), the peer stops
sending ACKs. The remote side keeps resending, which adds more inbound traffic, and
eventually a side goes 15 s without ACKs and RakNet drops the connection.

LAN makes it worse. Low RTT lets the sender's congestion window grow large, so it pushes
more datagrams per second than this receive path can drain. Over the internet, higher RTT
throttles the sender below the ceiling. This early-RakNet-4.0 behaviour was later changed
upstream to a `do { ... } while (len > 0)` drain loop that falls through to the normal update.

It also matters for the 4-player mod: the host receives from three peers, which
multiplies inbound load, so this fix is effectively a prerequisite for >2 players.

## Proposed fix
Turn the early return into the upstream drain loop. Change the jump at `0x602bef` from
`jmp 0x604729` (`e9 35 1b 00 00`) to `jmp 0x602b4c` (`e9 58 ff ff ff`), which re-reads
the socket. When no datagram is pending, `len <= 0` takes the existing
`jle 0x602bf4` into the normal update.

Safety checks done:
- Stack depth at `0x602bef` equals depth at `0x602b4c`. The pushes between
  `0x602b83` and the call total `0x34`, matching `add esp,0x34` (`0x5f7220` is cdecl).
  The virtual recv call cleans its own 5 args (`ret 0x14`).
- `esi` (RakPeer `this`) is callee-saved across both calls. `edi` is re-derived at `0x602b6c`.

## Optional follow-ups (lower priority)
- Raise `SetTimeoutTime` from 15000 ms (imm `0x3a98` pushed at `0x45787a`) to about 30000 ms,
  so a short stall during a big wave doesn't drop the session.
- Shorten the update thread wait (`0x6047cb push 0xa`) from 10 ms to 1–2 ms.

## Test plan
1. Reproduce on LAN: Endless co-op, push past the wave where drops usually happen, and log time-to-drop.
2. Apply the fix only, repeat, and compare time-to-drop and in-game ping (`Net_GetPingMS`).
3. Repeat over the internet (Steam relay) to confirm no regression.
4. Then test with 3–4 players once the multiplayer patches land.

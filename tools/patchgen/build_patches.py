"""
build_patches.py - turn the research into a verified, data-driven patch set.

Inputs : OrcsMustDie2.exe (build stamp 0x51DD91A9 / 2013-07-11), research/slot_sites.tsv
Outputs: patches/patchset.json          (human readable, reviewed in PRs)
         loader/src/patchset_gen.h      (compiled into omd2mp's version.dll)

Every patch records the ORIGINAL bytes. The loader refuses to touch a site whose
bytes differ (different game build / another mod), so a wrong patch can never be
applied blindly.

Patch kinds
  imm      overwrite an immediate in place; value is an expression of N (players)
  float    overwrite a float constant; value is an expression of N
  tramp    replace >=5 bytes with `jmp cave`; the cave holds re-encoded
           instructions (+ optional call into C) then `jmp back`.

Groups (enable/disable in omd2mp.ini):
  lobby     Steam lobby + RakNet connection caps            (safe, tiny)
  mesh      client-side caps + FullyConnectedMesh2 auto-connect (optional)
  slots     relocate Session.slots[2] -> slots[N]            (EXPERIMENTAL)
  economy   NetMoneyFactor default scaled by player count
  netfix    RakNet receive-drain fix for co-op disconnects   (proposal, off)

Run:  python tools/patchgen/build_patches.py   (needs pefile, capstone, numpy, the game exe)
      python tools/patchgen/build_patches.py --emit-only   (JSON -> C header only)
"""
import os, re, sys, json, struct, bisect
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "re"))
from pe32 import Image
import capstone
from capstone import x86

EXPECTED_TIMESTAMP = 1373475241      # PE header TimeDateStamp of the supported build
OLD_BASE = 0x38                      # Session.slots offset in the original layout
SLOT_SIZE = 0x18
OLD_SESSION_SIZE = 0x36058
NEW_BASE = 0x36068                   # first offset past the original struct with NEW_BASE % 24 == OLD_BASE % 24
DELTA = NEW_BASE - OLD_BASE          # added to every slot address computation
assert NEW_BASE % SLOT_SIZE == OLD_BASE % SLOT_SIZE and NEW_BASE >= OLD_SESSION_SIZE
MAX_PLAYERS = 16                     # loader/src/patchcore.h OMD2MP_MAX_PLAYERS
# Two more per-slot arrays live at the start of the Session, 8 bytes per slot, sized for 2:
#   +0x00 u64 voiceTime[2]   last voice packet per slot   (msg 0x8C, 0x4B1310 / 0x4B31B0)
#   +0x10 u64 recvTime[2]    last packet of any kind      (net loop 0x457150, 0x4B2980)
# Slot 2's recvTime would land on +0x20..0x27, i.e. on the pointer at +0x24 that the Session
# calls through (e.g. 0x4B3DC0), so both move behind slots[MAX_PLAYERS].
VOICE_NEW = NEW_BASE + MAX_PLAYERS * SLOT_SIZE
RECV_NEW = VOICE_NEW + MAX_PLAYERS * 8
NEW_SESSION_SIZE = RECV_NEW + MAX_PLAYERS * 8
HOOK_REPLICATE_SLOTS = 1             # C: omd2mp_replicate_slots(Session*)
HOOK_SESSION_INIT = 2                # C: replicate slots + zero the moved u64 arrays
SYM_NETVARS = "SYM_NETVARS"          # expression: address of the loader's NetVars[MAX_PLAYERS]
NETVARS_OLD, NETVARS_SIZE = 0xBE7D90, 0x100C
# Hero* heroByPlayer[2] at 0xBA4468, indexed by player slot. Lua getters (hero position,
# health, ...) read it with any player index; entries >= 2 ran into the object at 0xBA4470
# and crashed on game start with 3+ players. Moved to the loader's Hero*[MAX_PLAYERS].
SYM_HEROES = "SYM_HEROES"
SYMBOLS = {SYM_NETVARS: "OMD2MP_SYM_NETVARS", SYM_HEROES: "OMD2MP_SYM_HEROES"}
HEROES_OLD = 0xBA4468
HEROES_SITES = [0x4250A7, 0x425185, 0x425277, 0x425357, 0x425435, 0x425527, 0x425607,
                0x4261A9, 0x4264BF,          # reads: mov reg, [idx*4+heroes]
                0x56F879, 0x56FCD7]          # hero spawn: mov [idx*4+heroes], edi
HEROES_CLEAR = 0x492FE9                      # level teardown: movq qword [heroes], xmm0

# Index bounds/loops the slot scanner misses because they use 8-bit registers (cmp bl,1 /
# cmp al,1) or guard an 8-byte-stride array. (va, original bytes, expression, what it guards)
SLOT_BOUNDS_EXTRA = [
    (0x4B1331, "80fb01", "N-1", "msg 0x8C voice: voiceTime[slot] write"),
    (0x4B13E5, "3c01", "N-1", "Net_IsReady(slot): slots[slot].state == 5"),
    (0x4B267A, "80fb01", "N-1", "Session::Join(slot): count++, joined flag, player create"),
    (0x4B26FA, "80fb01", "N-1", "Session::Leave(slot)"),
    (0x4B2791, "80fb01", "N-1", "Session::PlayerJoined(slot): recvTime + 'playerjoin' UI event"),
    (0x4B2984, "3c01", "N-1", "TimeSinceLastReceived(slot)"),
    (0x4B2D4D, "80fb01", "N-1", "msg 0x84 handler: slots[slot] loaded-ack flag"),
    (0x4B2D96, "80fb01", "N-1", "Session::MaybePlayerJoined(slot)"),
    (0x4B31B5, "80fb01", "N-1", "voiceTime[slot] write"),
    (0x4B3D70, "80fb01", "N-1", "Session::LocalReady: all other slots joined? (index bound)"),
    (0x4571F2, "80fb02", "N", "net loop: recvTime[slot] update (index < 2)"),
    (0x4B2A35, "80fb02", "N", "max TimeSinceLastReceived over slots (loop)"),
    (0x4B3DA0, "80fb02", "N", "Session::LocalReady loop over slots"),
]
# Instructions addressing voiceTime/recvTime: (va, old array offset)
SLOT_U64_SITES = [
    (0x45721C, 0x10), (0x457220, 0x10), (0x4B27B3, 0x10), (0x4B27B7, 0x10), (0x4B29AA, 0x10), (0x4B29AE, 0x10),
    (0x4B133E, 0x00), (0x4B1341, 0x00), (0x4B2F76, 0x00), (0x4B2F7C, 0x00),
    (0x4B31C2, 0x00), (0x4B31C5, 0x00), (0x4B31DD, 0x00), (0x4B31E0, 0x00),
]
# Per-remote-player net variables (Level_ReadyUp's "ready" flag, etc.) are a static
# NetVars[2] at 0xBE7D90 (0x100C bytes each); slot 2 would alias the local block at 0xBE9DA8.
NETVARS_BASE_SITES = [0x4927E6, 0x5012B7, 0x5012F9, 0x501D1D, 0x502598, 0x502648, 0x5026A8, 0x502729]
NETVARS_BOUNDS = [
    (0x50128F, "83f801", "N-1"), (0x501CF6, "83f801", "N-1"), (0x502570, "83f801", "N-1"),
    (0x502620, "83f801", "N-1"), (0x502680, "83f801", "N-1"), (0x502709, "83f801", "N-1"),
    (0x4927C2, "83fe01", "N-1"), (0x501305, "3c02", "N"),
]

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.detail = True


class Gen:
    def __init__(self):
        self.img = Image()
        ts = self.img.pe.FILE_HEADER.TimeDateStamp
        if ts != EXPECTED_TIMESTAMP:
            sys.exit(f"unsupported OrcsMustDie2.exe build (TimeDateStamp {ts:#x})")
        self.patches = []
        self.claimed = {}           # byte address -> patch id (overlap guard)
        self._branch_targets()

    # ------------------------------------------------------------ helpers
    def ins_at(self, va):
        return next(md.disasm(self.img.read(va, 16), va, 1))

    def expect(self, va, hexbytes, what):
        got = self.img.read(va, len(hexbytes) // 2).hex()
        if got != hexbytes:
            sys.exit(f"{what}: expected {hexbytes} at {va:#x}, found {got}")

    def claim(self, va, n, pid):
        for a in range(va, va + n):
            if a in self.claimed:
                sys.exit(f"patch {pid} overlaps {self.claimed[a]} at {a:#x}")
            self.claimed[a] = pid

    def _branch_targets(self):
        """Every direct branch/call target and every relocated pointer into .text.
        A trampoline may not steal bytes that something else jumps into."""
        lite = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        lite.skipdata = True
        t = set()
        self.starts = []
        self.memrefs = []           # (instruction va, absolute address it reads/writes)
        for a, sz, mn, ops in lite.disasm_lite(self.img.text, self.img.text_va):
            self.starts.append(a)
            if "[0x" in ops:
                self.memrefs += [(a, int(m, 16)) for m in re.findall(r"\[0x([0-9a-f]+)\]", ops)]
            if (mn.startswith("j") or mn == "call") and re.fullmatch(r"0x[0-9a-f]+", ops):
                t.add(int(ops, 16))
        import pefile
        self.img.pe.parse_data_directories([pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_BASERELOC"]])
        lo, hi = self.img.text_va, self.img.text_va + len(self.img.text)
        for blk in getattr(self.img.pe, "DIRECTORY_ENTRY_BASERELOC", []):
            for e in blk.entries:
                if e.type == 3:
                    v = self.img.u32(self.img.base + e.rva)
                    if v and lo <= v < hi:
                        t.add(v)
        self.targets = t

    # ------------------------------------------------------------ re-encoding
    @staticmethod
    def widen_disp(ins, add):
        """Re-encode a ModRM memory operand so its displacement is disp32 = old+add."""
        b = bytearray(ins.bytes)
        mo = ins.modrm_offset
        mod, reg, rm = ins.modrm >> 6, (ins.modrm >> 3) & 7, ins.modrm & 7
        if mod == 1:                                   # disp8 -> disp32
            do = ins.disp_offset
            old = struct.unpack("b", bytes([b[do]]))[0]
            nb = b[:mo] + bytes([(2 << 6) | (reg << 3) | rm]) + b[mo + 1:do] + struct.pack("<i", old + add) + b[do + 1:]
        elif mod == 0:                                 # no disp -> disp32 (SIB base must not be ebp)
            if rm == 5 or (rm == 4 and (ins.sib & 7) == 5):
                raise ValueError("absolute form")
            end = mo + 1 + (1 if rm == 4 else 0)
            nb = b[:mo] + bytes([(2 << 6) | (reg << 3) | rm]) + b[mo + 1:end] + struct.pack("<i", add) + b[end:]
        elif mod == 2:
            do = ins.disp_offset
            old = struct.unpack("<i", bytes(b[do:do + 4]))[0]
            nb = b[:do] + struct.pack("<i", old + add) + b[do + 4:]
        else:
            raise ValueError("register operand")
        chk = next(md.disasm(bytes(nb), ins.address, 1))
        assert chk.size == len(nb) and chk.mnemonic == ins.mnemonic, (ins.op_str, chk.op_str)
        return bytes(nb), chk

    @staticmethod
    def widen_add_imm(ins, add):
        """add r32, imm8 (83 /0 ib)  ->  add r32, imm32 (81 /0 id)."""
        b = bytes(ins.bytes)
        assert b[0] == 0x83 and (b[1] >> 3) & 7 == 0, ins.op_str
        nb = bytes([0x81, b[1]]) + struct.pack("<i", struct.unpack("b", b[2:3])[0] + add)
        chk = next(md.disasm(nb, ins.address, 1))
        assert chk.mnemonic == "add"
        return nb, chk

    # ------------------------------------------------------------ patch kinds
    def imm(self, pid, group, va, orig_hex, off, size, expr, why):
        self.expect(va, orig_hex, pid)
        for t in self.patches:                         # already stolen into a cave?
            if t["kind"] == "tramp" and t["va"] <= va < t["va"] + t["steal"]:
                for ova, coff, n in t["verbatim"]:
                    if ova <= va + off < ova + n:
                        t["cave_imms"].append(dict(id=pid, cave_off=coff + (va + off - ova), size=size, expr=expr, why=why))
                        return
                sys.exit(f"{pid}: immediate is inside rewritten bytes of {t['id']}")
        self.claim(va + off, size, pid)
        self.patches.append(dict(id=pid, group=group, kind="imm", va=va, orig=orig_hex,
                                 off=off, size=size, expr=expr, why=why))

    def flt(self, pid, group, va, orig_hex, expr, why):
        self.expect(va, orig_hex, pid)
        readers = sorted(a for a, tgt in self.memrefs if va - 15 <= tgt < va + 4)
        if len(readers) > 1:
            # MSVC pools float constants: one 0.5f is shared by every `x * 0.5` in the game.
            # Rewriting it changes all of them (this broke the HUD, aiming and ragdolls).
            sys.exit(f"{pid}: {va:#x} is read by {len(readers)} instructions "
                     f"({', '.join(hex(a) for a in readers[:6])}...); patch the one reader instead")
        self.claim(va, 4, pid)
        self.patches.append(dict(id=pid, group=group, kind="float", va=va, orig=orig_hex,
                                 off=0, size=4, expr=expr, why=why))

    def tramp(self, pid, group, va, rewrite, why, hook=None, hook_reg=None, extra_rewrites=None, back=0):
        """Steal whole instructions from `va` until >=5 bytes; rewrite(ins)->bytes|None
        for the instruction at `va` (and for any other stolen VA listed in extra_rewrites).
        back=k starts the stolen region k instructions *before* va (used when the bytes
        after va are a loop head / branch target)."""
        extra_rewrites = dict(extra_rewrites or {})
        site = va
        if back:
            i = bisect.bisect_left(self.starts, va)
            assert self.starts[i] == va, "site not on a sweep boundary"
            va = self.starts[i - back]
            if rewrite:
                extra_rewrites[site] = rewrite
            rewrite = None
        stolen, n, cur = [], 0, va
        while n < 5 or cur <= site:
            ins = self.ins_at(cur)
            if back and cur > site:
                raise ValueError(f"{pid}: backward window too small")
            if cur != va and cur in self.targets:
                raise ValueError(f"{pid}: branch target inside stolen region at {cur:#x}")
            if ins.mnemonic in ("ret", "jmp") and n + ins.size < 5 and ins.mnemonic == "ret":
                raise ValueError(f"{pid}: region ends in ret before 5 bytes")
            stolen.append(ins)
            n += ins.size
            cur += ins.size
        cave, fix, verbatim, rimms, rewritten = bytearray(), [], [], [], []
        if hook:
            cave += b"\x60\x9c"                                   # pushad; pushfd
            cave += bytes([{"ebx": 0x53, "esi": 0x56, "edi": 0x57}[hook_reg]])  # push session
            fix.append((len(cave) + 1, -hook))                    # call <C hook>
            cave += b"\xe8\0\0\0\0"
            cave += b"\x83\xc4\x04\x9d\x61"                       # add esp,4; popfd; popad
        for k, ins in enumerate(stolen):
            rw = rewrite if k == 0 else extra_rewrites.get(ins.address)
            if rw:
                res = rw(ins)
                code = res[0] if isinstance(res, tuple) else res
                imms = res[1] if isinstance(res, tuple) and isinstance(res[1], list) else []
                for im in imms:     # dict(off=<offset in code>, size, expr, float?, why)
                    rimms.append(dict(id=pid, cave_off=len(cave) + im["off"], size=im["size"], expr=im["expr"],
                                      isfloat=bool(im.get("float")), why=im.get("why", why)))
                rewritten.append((ins.address, len(cave), len(code)))
                cave += code
                continue
            if ins.mnemonic in ("call", "jmp") and ins.operands[0].type == x86.X86_OP_IMM:
                tgt = ins.operands[0].imm & 0xffffffff
                cave += (b"\xe8" if ins.mnemonic == "call" else b"\xe9")
                fix.append((len(cave), tgt))
                cave += b"\0\0\0\0"
            elif ins.mnemonic.startswith("j") and ins.operands[0].type == x86.X86_OP_IMM:
                assert (ins.size == 2 and 0x70 <= ins.bytes[0] <= 0x7f) or (ins.bytes[0] == 0x0f and 0x80 <= ins.bytes[1] <= 0x8f), ins.mnemonic
                tgt = ins.operands[0].imm & 0xffffffff
                cc = ins.bytes[0] & 0x0f if ins.size == 2 else ins.bytes[1] & 0x0f
                cave += bytes([0x0f, 0x80 | cc])
                fix.append((len(cave), tgt))
                cave += b"\0\0\0\0"
            elif any(op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_EIP for op in ins.operands):
                raise ValueError("eip-relative")
            else:
                verbatim.append((ins.address, len(cave), ins.size))
                cave += bytes(ins.bytes)
        fix.append((len(cave) + 1, va + n))                       # jmp back
        cave += b"\xe9\0\0\0\0"
        self.claim(va, n, pid)
        self.patches.append(dict(id=pid, group=group, kind="tramp", va=va,
                                 orig=self.img.read(va, n).hex(), steal=n,
                                 cave=bytes(cave).hex(), fixups=fix, why=why, verbatim=verbatim, rewritten=rewritten,
                                 cave_imms=rimms,
                                 asm=[f"{i.address:#x}: {i.mnemonic} {i.op_str}" for i in stolen]))
        return n

    # ------------------------------------------------------------ the patch set
    def build(self):
        # ---------------------------------------------------------------- lobby
        self.imm("lobby.steam_member_limit", "lobby", 0x45704D, "c7474002000000", 3, 4, "N",
                 "Lobby2 Console_CreateRoom msg: maxMembers=2 -> ISteamMatchmaking::CreateLobby(type, maxMembers)")
        self.imm("lobby.host_startup_maxconn", "lobby", 0x456F1E, "6a01", 1, 1, "N-1",
                 "host RakPeer::Startup(maxConnections=1,...)")
        self.imm("lobby.host_max_incoming", "lobby", 0x456F2D, "6a01", 1, 1, "N-1",
                 "host RakPeer::SetMaximumIncomingConnections(1)")
        # ----------------------------------------------------------------- mesh
        self.imm("mesh.client_startup_maxconn", "mesh", 0x4570C2, "6a01", 1, 1, "N-1",
                 "client RakPeer::Startup(maxConnections=1,...) - needed only for client<->client links")
        # 0x45795D is `push ebx` (1 byte, ebx==0); `push 1` is 2 bytes, so use a trampoline.
        self.tramp("mesh.fcm2_connect_on_new_remote", "mesh", 0x45795D,
                   lambda ins: b"\x6a\x01",
                   "FullyConnectedMesh2::SetConnectOnNewRemoteConnection(false,\"\") -> true")
        # ---------------------------------------------------------------- slots
        self._slot_sites()
        self._slot_extras()
        for va in (0x4B18B5, 0x4B19B7, 0x4B1BF8):
            self.imm(f"slots.session_alloc_{va:x}", "slots", va, "6858600300", 1, 4,
                     f"{NEW_SESSION_SIZE:#x}",
                     "operator new(sizeof(Session)) grown to hold slots[16] + voiceTime[16] + recvTime[16]")
        self.imm("slots.ctor_loop_count", "slots", 0x4B1AEC, "bf01000000", 1, 4, "N-1",
                 "Session ctor: for (i=1; i>=0; --i) RakNetGUID() over slots")
        self.imm("slots.invalid_slot_sentinel", "slots", 0x4B1D05, "c7050cd1b90002000000", 6, 4, "N",
                 "g_pendingSlot = 2 (== old MAX_SLOTS, used as 'none')")
        self.tramp("slots.hook_session_init", "slots", 0x4B1DE2, None,
                   "after Session::Reset unrolled slot init: copy reset slot0 into new slots[0..N), "
                   "zero the relocated voiceTime/recvTime arrays",
                   hook=HOOK_SESSION_INIT, hook_reg="esi")
        self.tramp("slots.hook_session_deserialize", "slots", 0x4B2BF6, None,
                   "after Session::Deserialize unrolled slot reset: copy reset slot0 into new slots[0..N)",
                   hook=HOOK_REPLICATE_SLOTS, hook_reg="ebx")
        # -------------------------------------------------------------- economy
        # 0x458020 scales gold by NetMoneyFactor, with `movss xmm0, [0x857810]` as the default
        # before the config lookup. 0x857810 is MSVC's pooled 0.5f shared by ~60 instructions
        # (screen centre, HUD layout, ragdolls), so it must not be rewritten. Load 1/N from an
        # immediate instead: push eax; mov eax, <float 1/N>; movd xmm0, eax; pop eax.
        self.expect(0x458042, "f30f100510788500", "economy.net_money_factor")
        self.tramp("economy.net_money_factor", "economy", 0x458042,
                   lambda ins: (b"\x50\xb8\0\0\0\x3f\x66\x0f\x6e\xc0\x58",      # template holds 0.5f
                                [dict(off=2, size=4, expr="1.0/N", float=True,
                                      why="default NetMoneyFactor; vanilla 0.5 == 1/2")]),
                   "NetMoneyFactor default (vanilla 0.5) -> 1/N, without touching the shared 0.5f constant")
        # --------------------------------------------------------------- netfix
        self.imm("netfix.recv_drain_loop", "netfix", 0x602BEF, "e9351b0000", 1, 4,
                 f"{0x602B4C - (0x602BEF + 5):#x}",
                 "RunUpdateCycle: after one socket-override datagram, loop back to read the next "
                 "instead of returning early (skipping ACK/resend processing). See docs/DISCONNECT_FIX_PROPOSAL.md")
        return self

    def _slot_sites(self):
        rows = [l.rstrip("\n").split("\t") for l in open(os.path.join(ROOT, "research", "slot_sites.tsv"))][1:]
        rows = [r for r in rows if r[3] == "session"]
        site_vas = {int(r[0], 16): r for r in rows}
        skipped = []
        for va in sorted(site_vas):
            r = site_vas[va]
            kind = r[2]
            if any(a in self.claimed for a in range(va, va + int(r[1]))):
                continue                                  # already rewritten inside an earlier cave
            ins = self.ins_at(va)
            pid = f"slots.{kind.split(':')[0]}_{va:x}"
            if kind.startswith("L:loop<2"):
                self.imm(pid, "slots", va, bytes(ins.bytes).hex(), ins.imm_offset, 1, "N",
                         f"slot loop bound: {ins.mnemonic} {ins.op_str}")
                continue
            if kind.startswith("L:bound<=1"):
                self.imm(pid, "slots", va, bytes(ins.bytes).hex(), ins.imm_offset, 1, "N-1",
                         f"slot index bound: {ins.mnemonic} {ins.op_str}; ja invalid")
                continue
            if kind.startswith("K") and ins.mnemonic == "add":
                rw = lambda i: self.widen_add_imm(i, DELTA)
            else:
                rw = lambda i: self.widen_disp(i, DELTA)
            # other slot sites that fall inside this steal window are rewritten in the cave too
            extra = {a: (lambda i: self.widen_add_imm(i, DELTA) if i.mnemonic == "add" else self.widen_disp(i, DELTA))
                     for a in site_vas if va < a < va + 16 and not site_vas[a][2].startswith("L")}
            err = None
            for back in (0, 1, 2, 3):
                try:
                    self.tramp(pid, "slots", va, rw, f"{kind}: {r[5]} (+{DELTA:#x})", extra_rewrites=extra, back=back)
                    err = None
                    break
                except (ValueError, AssertionError) as e:
                    err = e
            if err:
                skipped.append((va, kind, r[5], str(err)))
        self.skipped = skipped

    def _steal(self, pid, va, rw, why, extra=None):
        """Trampoline with the same backward-window fallback as the scanned slot sites."""
        err = None
        for back in (0, 1, 2, 3):
            try:
                return self.tramp(pid, "slots", va, rw, why, extra_rewrites=extra, back=back)
            except (ValueError, AssertionError) as e:
                err = e
        sys.exit(f"{pid}: no safe trampoline window: {err}")

    def _slot_extras(self):
        """Everything else that assumes 2 slots: 8-bit bounds, the u64 arrays, NetVars[2]."""
        for va, orig, expr, why in SLOT_BOUNDS_EXTRA:
            ins = self.ins_at(va)
            self.imm(f"slots.L8_{va:x}", "slots", va, orig, ins.imm_offset, 1, expr, f"{ins.mnemonic} {ins.op_str}: {why}")
        sites = dict(SLOT_U64_SITES)
        new_of = {0x00: VOICE_NEW, 0x10: RECV_NEW}
        for va in sorted(sites):
            if va in self.claimed:
                continue                                  # rewritten inside the previous cave
            rw = lambda i, old=sites[va]: self.widen_disp(i, new_of[old] - old)
            extra = {a: (lambda i, old=sites[a]: self.widen_disp(i, new_of[old] - old))
                     for a in sites if va < a < va + 16}
            name = "voiceTime" if sites[va] == 0 else "recvTime"
            self._steal(f"slots.U64_{va:x}", va, rw, f"Session.{name}[slot] moved +{sites[va]:#x} -> +{new_of[sites[va]]:#x}", extra)
        for va in NETVARS_BASE_SITES:
            ins = self.ins_at(va)
            assert ins.mnemonic == "add" and (ins.operands[1].imm & 0xffffffff) == NETVARS_OLD, (hex(va), ins.op_str)
            self.imm(f"slots.netvars_base_{va:x}", "slots", va, bytes(ins.bytes).hex(), ins.imm_offset, 4, SYM_NETVARS,
                     f"{ins.mnemonic} {ins.op_str}: &NetVars[slot] -> loader's NetVars[{MAX_PLAYERS}]")
        for va, orig, expr in NETVARS_BOUNDS:
            ins = self.ins_at(va)
            self.imm(f"slots.netvars_bound_{va:x}", "slots", va, orig, ins.imm_offset, 1, expr,
                     f"{ins.mnemonic} {ins.op_str}: NetVars[slot] index bound")
        self._heroes()

    def _heroes(self):
        for va in HEROES_SITES:
            ins = self.ins_at(va)
            mem = [o for o in ins.operands if o.type == x86.X86_OP_MEM]
            assert ins.mnemonic == "mov" and len(mem) == 1 and mem[0].mem.scale == 4 \
                and (mem[0].mem.disp & 0xffffffff) == HEROES_OLD and ins.disp_size == 4, (hex(va), ins.op_str)
            self.imm(f"slots.heroes_{va:x}", "slots", va, bytes(ins.bytes).hex(), ins.disp_offset, 4, SYM_HEROES,
                     f"{ins.mnemonic} {ins.op_str}: heroByPlayer[slot] -> loader's Hero*[{MAX_PLAYERS}]")
        orig = b"\x66\x0f\xd6\x05" + struct.pack("<I", HEROES_OLD)
        ins = self.ins_at(HEROES_CLEAR)
        assert bytes(ins.bytes) == orig, (hex(HEROES_CLEAR), ins.op_str)
        # xmm0 is live after this, so no C call: clear all MAX_PLAYERS entries with rep stosd.
        code = (b"\x9c\x57\x51\x50"                       # pushfd; push edi; push ecx; push eax
                b"\xbf\0\0\0\0"                           # mov edi, <heroes>
                b"\x31\xc0" + b"\xb9" + struct.pack("<I", MAX_PLAYERS) +   # xor eax,eax; mov ecx,N
                b"\xfc\xf3\xab"                           # cld; rep stosd
                b"\x58\x59\x5f\x9d")                      # pop eax; pop ecx; pop edi; popfd
        why = f"level teardown: clear heroByPlayer[{MAX_PLAYERS}] (was movq [{HEROES_OLD:#x}], xmm0)"
        self.tramp("slots.heroes_clear", "slots", HEROES_CLEAR,
                   lambda i: (code, [dict(off=5, size=4, expr=SYM_HEROES, why=why)]), why)

    # ------------------------------------------------------------ emit
    def write(self):
        os.makedirs(os.path.join(ROOT, "patches"), exist_ok=True)
        meta = dict(exe_timestamp=EXPECTED_TIMESTAMP, slot_old_base=OLD_BASE, slot_new_base=NEW_BASE,
                    slot_size=SLOT_SIZE, delta=DELTA, max_players=MAX_PLAYERS, voice_time_new=VOICE_NEW,
                    recv_time_new=RECV_NEW, session_size=NEW_SESSION_SIZE, unpatched_slot_sites=[
                        dict(va=hex(v), kind=k, ins=s, reason=e) for v, k, s, e in self.skipped])
        json.dump(dict(meta=meta, patches=self.patches), open(os.path.join(ROOT, "patches", "patchset.json"), "w"),
                  indent=1, default=lambda o: o)
        self._emit_c()
        from collections import Counter
        print(Counter(p["group"] for p in self.patches), "skipped:", len(self.skipped))
        for s in self.skipped:
            print("  SKIPPED", hex(s[0]), s[1], s[2], "->", s[3])

    def _emit_c(self):
        open(os.path.join(ROOT, "loader", "src", "patchset_gen.h"), "w").write(emit_c(self.patches))


def emit_c(patches):
    """Render the patch list as C data for loader/src/patchset.h. Pure function (unit-tested)."""
    exprs = sorted({p["expr"] for p in patches if "expr" in p} |
                   {ci["expr"] for p in patches for ci in p.get("cave_imms", [])})
    L = ["// GENERATED by tools/patchgen/build_patches.py - do not edit by hand.",
         "#pragma once", "#include \"patchset.h\"", "",
         f"#define OMD2MP_EXE_TIMESTAMP {EXPECTED_TIMESTAMP:#x}u",
         f"#define OMD2MP_SLOT_OLD_BASE {OLD_BASE:#x}u", f"#define OMD2MP_SLOT_NEW_BASE {NEW_BASE:#x}u",
         f"#define OMD2MP_SLOT_SIZE {SLOT_SIZE:#x}u",
         f"#define OMD2MP_VOICE_TIME_NEW {VOICE_NEW:#x}u   /* u64[{MAX_PLAYERS}], was Session+0x00 */",
         f"#define OMD2MP_RECV_TIME_NEW {RECV_NEW:#x}u   /* u64[{MAX_PLAYERS}], was Session+0x10 */",
         f"#define OMD2MP_SESSION_SIZE {NEW_SESSION_SIZE:#x}u",
         f"#define OMD2MP_NETVARS_SIZE {NETVARS_SIZE:#x}u   /* one NetVars block; the game's NetVars[2] was at {NETVARS_OLD:#x} */",
         "",
         "static double omd2mp_eval(int expr, int N) {", "    switch (expr) {"]
    for k, e in enumerate(exprs):
        c = f"(double)omd2mp_symbol({SYMBOLS[e]})" if e in SYMBOLS else e.replace('N', '(double)N')
        L.append(f"    case {k}: return {c};   /* {e} */")
    L += ["    }", "    return 0;", "}", ""]

    def cb(h):
        return ",".join(f"0x{h[i:i+2]}" for i in range(0, len(h), 2))
    rows = []
    for k, p in enumerate(patches):
        L.append(f"static const uint8_t P{k}_orig[] = {{{cb(p['orig'])}}};")
        cave, fx, ci = "NULL", "NULL", "NULL"
        if p["kind"] == "tramp":
            L.append(f"static const uint8_t P{k}_cave[] = {{{cb(p['cave'])}}};")
            cave = f"P{k}_cave"
            if p["fixups"]:
                L.append(f"static const Fixup P{k}_fix[] = {{" + ",".join(
                    f"{{{o}, {t if t >= 0 else 0}u, {-t if t < 0 else 0}}}" for o, t in p["fixups"]) + "};")
                fx = f"P{k}_fix"
            if p.get("cave_imms"):
                L.append(f"static const CaveImm P{k}_imm[] = {{" + ",".join(
                    f"{{{c['cave_off']}, {c['size']}, {exprs.index(c['expr'])}, {int(c.get('isfloat', False))}}}"
                    for c in p["cave_imms"]) + "};")
                ci = f"P{k}_imm"
        expr = exprs.index(p["expr"]) if "expr" in p else -1
        rows.append(f'  {{"{p["id"]}", "{p["group"]}", PK_{p["kind"].upper()}, {p["va"]:#x}u, '
                    f'{len(p["orig"])//2}, P{k}_orig, {p.get("off", 0)}, {p.get("size", 0)}, {expr}, '
                    f'{len(p["cave"])//2 if p["kind"] == "tramp" else 0}, {cave}, '
                    f'{len(p.get("fixups", []))}, {fx}, {len(p.get("cave_imms", []))}, {ci}}},')
    L += ["", "static const Patch OMD2MP_PATCHES[] = {"] + rows + ["};",
          "#define OMD2MP_PATCH_COUNT (sizeof(OMD2MP_PATCHES)/sizeof(OMD2MP_PATCHES[0]))", ""]
    return "\n".join(L)


if __name__ == "__main__":
    os.makedirs(os.path.join(ROOT, "loader", "src"), exist_ok=True)
    if "--emit-only" in sys.argv:      # re-render the C header from patches/patchset.json (no exe needed)
        ps = json.load(open(os.path.join(ROOT, "patches", "patchset.json")))["patches"]
        open(os.path.join(ROOT, "loader", "src", "patchset_gen.h"), "w").write(emit_c(ps))
    else:
        Gen().build().write()

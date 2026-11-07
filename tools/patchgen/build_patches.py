"""
build_patches.py - turn the research into a verified, data-driven patch set.

Inputs : OrcsMustDie2.exe (build stamp 0x51DE1E29 / 2013-07-11), research/slot_sites.tsv
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
HOOK_REPLICATE_SLOTS = 1             # C: omd2mp_replicate_slots(Session*)

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
        for a, sz, mn, ops in lite.disasm_lite(self.img.text, self.img.text_va):
            self.starts.append(a)
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
        cave, fix, verbatim = bytearray(), [], []
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
                cave += res[0] if isinstance(res, tuple) else res
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
                                 cave=bytes(cave).hex(), fixups=fix, why=why, verbatim=verbatim, cave_imms=[],
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
        for va in (0x4B18B5, 0x4B19B7, 0x4B1BF8):
            self.imm(f"slots.session_alloc_{va:x}", "slots", va, "6858600300", 1, 4,
                     f"{NEW_BASE:#x}+N*{SLOT_SIZE:#x}", "operator new(sizeof(Session)) grown to hold slots[N]")
        self.imm("slots.ctor_loop_count", "slots", 0x4B1AEC, "bf01000000", 1, 4, "N-1",
                 "Session ctor: for (i=1; i>=0; --i) RakNetGUID() over slots")
        self.imm("slots.invalid_slot_sentinel", "slots", 0x4B1D05, "c7050cd1b90002000000", 6, 4, "N",
                 "g_pendingSlot = 2 (== old MAX_SLOTS, used as 'none')")
        self.tramp("slots.hook_session_init", "slots", 0x4B1DE2, None,
                   "after Session::Reset unrolled slot init: copy reset slot0 into new slots[0..N)",
                   hook=HOOK_REPLICATE_SLOTS, hook_reg="esi")
        self.tramp("slots.hook_session_deserialize", "slots", 0x4B2BF6, None,
                   "after Session::Deserialize unrolled slot reset: copy reset slot0 into new slots[0..N)",
                   hook=HOOK_REPLICATE_SLOTS, hook_reg="ebx")
        # -------------------------------------------------------------- economy
        self.flt("economy.net_money_factor", "economy", 0x857810, struct.pack("<f", 0.5).hex(),
                 "1.0/N", "default NetMoneyFactor (gold multiplier when connected); vanilla 0.5 == 1/2")
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

    # ------------------------------------------------------------ emit
    def write(self):
        os.makedirs(os.path.join(ROOT, "patches"), exist_ok=True)
        meta = dict(exe_timestamp=EXPECTED_TIMESTAMP, slot_old_base=OLD_BASE, slot_new_base=NEW_BASE,
                    slot_size=SLOT_SIZE, delta=DELTA, unpatched_slot_sites=[
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
         f"#define OMD2MP_SLOT_SIZE {SLOT_SIZE:#x}u", "",
         "static double omd2mp_eval(int expr, int N) {", "    switch (expr) {"]
    for k, e in enumerate(exprs):
        L.append(f"    case {k}: return {e.replace('N', '(double)N')};   /* {e} */")
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
                    f"{{{c['cave_off']}, {c['size']}, {exprs.index(c['expr'])}}}" for c in p["cave_imms"]) + "};")
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

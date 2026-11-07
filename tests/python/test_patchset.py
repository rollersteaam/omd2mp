"""
Consistency tests for patches/patchset.json and the generated C header.

These read only the JSON in this repo, never the game files. They check that each
record makes sense on its own terms: sites don't overlap, trampolines jump back to the
right place, every rel32 fixup sits in a branch operand, and relocated slot accesses
moved by exactly `delta`. The capstone-based tests are skipped if capstone isn't installed.
"""
import json, os, re, struct, sys, unittest
ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
PS = json.load(open(os.path.join(ROOT, "patches", "patchset.json")))
META, PATCHES = PS["meta"], PS["patches"]
GROUPS = {"lobby", "mesh", "slots", "economy", "netfix"}

try:
    import capstone
    MD = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
except ImportError:
    capstone = None


def render(p, cave_va=0x7F000000, hook_va=0x10000000):
    """Python mirror of omd2mp_render_cave (fixups only), for disassembly."""
    b = bytearray.fromhex(p["cave"])
    for off, tgt in p["fixups"]:
        dest = hook_va if tgt < 0 else tgt
        struct.pack_into("<I", b, off, (dest - (cave_va + off + 4)) & 0xFFFFFFFF)
    return bytes(b)


class Structure(unittest.TestCase):
    def test_ids_unique_and_groups_known(self):
        ids = [p["id"] for p in PATCHES]
        self.assertEqual(len(ids), len(set(ids)))
        self.assertTrue({p["group"] for p in PATCHES} <= GROUPS)

    def test_meta(self):
        self.assertEqual(META["delta"], META["slot_new_base"] - META["slot_old_base"])
        self.assertEqual(META["slot_new_base"] % META["slot_size"], META["slot_old_base"] % META["slot_size"])
        self.assertEqual(META["unpatched_slot_sites"], [])

    def test_slot_loops_relocate_their_base(self):
        """A slot loop whose bound is raised to N but whose base still points at the old
        array walks past slots[1] into Session +0x68.. (dirty flag, localSlot, playerCount).
        The host's 0x86 slot-state broadcast (0x4B3700) did this: it read dirty flags from
        the stale array, never sent slot states, and real 2-player games hung on
        'Waiting for other player...' / 'Connecting...'. So did the host's 0x83 session-data
        writer (0x4B38D0): it keeps the Session in ebp (not a frame pointer) and walked the
        old, empty array, so a joining client never found its own slot."""
        vas = {p["va"] for p in PATCHES}
        spans = [(p["va"], p["va"] + len(p["orig"]) // 2) for p in PATCHES]
        covered = lambda a: any(lo <= a < hi for lo, hi in spans)
        for base, bound in [(0x4B3756, 0x4B379B), (0x4B1F3F, 0x4B1F9C), (0x4B259A, 0x4B25FB),
                            (0x4B3A14, 0x4B3A8C)]:
            self.assertIn(bound, vas, hex(bound))
            self.assertTrue(covered(base), f"loop bound {bound:#x} patched but base {base:#x} is not")
        for a in (0x4B2600, 0x4B2608, 0x4B2631, 0x4B2639, 0x4B2643, 0x4B2652, 0x4B2E0A, 0x4B2E20):
            self.assertTrue(covered(a), f"slots[0] access {a:#x} not relocated")

    def test_no_overlapping_sites(self):
        spans = sorted((p["va"], p["va"] + len(p["orig"]) // 2, p["id"]) for p in PATCHES)
        for (a0, a1, i), (b0, b1, j) in zip(spans, spans[1:]):
            self.assertLessEqual(a1, b0, f"{i} overlaps {j}")

    def test_value_patches_fit(self):
        for p in PATCHES:
            if p["kind"] in ("imm", "float"):
                self.assertLessEqual(p["off"] + p["size"], len(p["orig"]) // 2, p["id"])
                self.assertIn(p["size"], (1, 4), p["id"])

    def test_trampolines(self):
        for p in (p for p in PATCHES if p["kind"] == "tramp"):
            cave = bytes.fromhex(p["cave"])
            self.assertGreaterEqual(p["steal"], 5, p["id"])
            self.assertEqual(p["steal"], len(p["orig"]) // 2, p["id"])
            for off, _ in p["fixups"]:
                self.assertLessEqual(off + 4, len(cave), p["id"])
            last_off, last_tgt = p["fixups"][-1]
            self.assertEqual(cave[last_off - 1], 0xE9, p["id"])               # ends with jmp back
            self.assertEqual(last_off + 4, len(cave), p["id"])
            self.assertEqual(last_tgt, p["va"] + p["steal"], p["id"])
            for ci in p["cave_imms"]:                       # imms only in copied or rewritten bytes
                spans = p["verbatim"] + p.get("rewritten", [])
                self.assertTrue(any(c <= ci["cave_off"] and ci["cave_off"] + ci["size"] <= c + n
                                    for _, c, n in spans), p["id"])
                if ci.get("isfloat"):
                    self.assertEqual(ci["size"], 4, p["id"])

    def test_shared_float_constants_untouched(self):
        """0x857810 is MSVC's pooled 0.5f (screen centre, HUD, ragdolls). Rewriting it once
        moved the crosshair to the top-left, the minimap to the bottom and stretched ragdolls."""
        for p in PATCHES:
            if p["kind"] in ("imm", "float"):
                lo = p["va"] + p["off"]
                self.assertFalse(lo < 0x857820 and 0x857810 < lo + p["size"], p["id"])
            self.assertNotEqual(p["kind"], "float", f"{p['id']}: patch the reader, not a pooled constant")

    @unittest.skipIf(capstone is None, "capstone not installed")
    def test_economy_loads_one_over_n(self):
        p = next(p for p in PATCHES if p["id"] == "economy.net_money_factor")
        self.assertEqual(p["va"], 0x458042)
        self.assertEqual(p["asm"], ["0x458042: movss xmm0, dword ptr [0x857810]"])
        b = bytearray(render(p))
        ci, = p["cave_imms"]
        self.assertTrue(ci["isfloat"])
        struct.pack_into("<f", b, ci["cave_off"], 1.0 / 4)
        asm = [f"{i.mnemonic} {i.op_str}" for i in MD.disasm(bytes(b), 0x7F000000)]
        self.assertEqual(asm[:4], ["push eax", f"mov eax, {struct.unpack('<I', struct.pack('<f', 0.25))[0]:#x}",
                                   "movd xmm0, eax", "pop eax"])
        self.assertTrue(asm[4].startswith("jmp "))
        self.assertEqual(p["fixups"][-1][1], 0x458042 + 8)               # back to `mov ecx, [eax]`

    def test_no_stack_relative_slot_rewrites(self):
        """[esp+0x38]/[ebp+0x38] are stack locals (0x4B1ACD is the SEH frame), never the Session."""
        for p in PATCHES:
            if p["group"] == "slots" and p["kind"] == "tramp":
                for a in p["asm"]:
                    self.assertNotRegex(a, r"\[e[sb]p \+ 0x38\]", p["id"])

    def test_hooks_preserve_state(self):
        for p in PATCHES:
            if any(t < 0 for _, t in p.get("fixups", [])):
                cave = bytes.fromhex(p["cave"])
                self.assertEqual(cave[:2], b"\x60\x9c", p["id"])            # pushad; pushfd
                self.assertIn(b"\x83\xc4\x04\x9d\x61", cave, p["id"])       # add esp,4; popfd; popad

    def test_vanilla_values(self):
        """At N=2 the value expressions give back the original immediates."""
        N = 2
        for p in PATCHES:
            if p["kind"] != "imm" or p["group"] == "netfix" or "session_alloc" in p["id"] or "SYM_" in p["expr"]:
                continue
            orig = bytes.fromhex(p["orig"])[p["off"]:p["off"] + p["size"]]
            want = int.from_bytes(orig, "little", signed=True)
            self.assertEqual(eval(p["expr"], {"N": N}), want, p["id"])

    def test_session_alloc_grows(self):
        n = META["max_players"]
        self.assertLessEqual(META["slot_new_base"] + n * META["slot_size"], META["voice_time_new"])
        self.assertLessEqual(META["voice_time_new"] + n * 8, META["recv_time_new"])
        self.assertLessEqual(META["recv_time_new"] + n * 8, META["session_size"])
        allocs = [p for p in PATCHES if "session_alloc" in p["id"]]
        self.assertEqual(len(allocs), 3)
        for p in allocs:
            self.assertEqual(eval(p["expr"], {"N": 4}), META["session_size"])

    def test_two_slot_limits_all_covered(self):
        """Every 8-bit slot bound, u64 per-slot array access and NetVars[2] site found by hand
        (build_patches.SLOT_BOUNDS_EXTRA etc.) is in the patch set, inside a cave or on its own."""
        want = {0x4B1331, 0x4B13E5, 0x4B267A, 0x4B26FA, 0x4B2791, 0x4B2984, 0x4B2D4D, 0x4B2D96, 0x4B31B5,
                0x4B3D70, 0x4571F2, 0x4B2A35, 0x4B3DA0,                                   # 8-bit bounds
                0x45721C, 0x4B27B3, 0x4B29AA, 0x4B133E, 0x4B2F76, 0x4B2F7C, 0x4B31C2, 0x4B31DD,  # u64 arrays
                0x4927E6, 0x5012B7, 0x5012F9, 0x501D1D, 0x502598, 0x502648, 0x5026A8, 0x502729,  # NetVars base
                0x50128F, 0x501CF6, 0x502570, 0x502620, 0x502680, 0x502709, 0x4927C2, 0x501305}  # NetVars bounds
        covered = set()
        for p in PATCHES:
            n = len(p["orig"]) // 2
            covered |= {a for a in want if p["va"] <= a < p["va"] + n}
        self.assertEqual(want - covered, set())

    def test_u64_arrays_point_past_slots(self):
        for p in PATCHES:
            if p["id"].startswith("slots.U64_"):
                self.assertTrue(p["rewritten"], p["id"])
                if capstone:
                    txt = " ".join(f"{i.mnemonic} {i.op_str}" for i in MD.disasm(render(p), 0x7F000000))
                    bases = (META["voice_time_new"], META["recv_time_new"])
                    self.assertTrue(any(hex(b + hi) in txt for b in bases for hi in (0, 4)), p["id"])  # lo or hi dword


@unittest.skipIf(capstone is None, "capstone not installed")
class Disassembly(unittest.TestCase):
    def decode(self, p):
        code = render(p)
        ins = list(MD.disasm(code, 0x7F000000))
        self.assertEqual(sum(i.size for i in ins), len(code), f"{p['id']}: cave does not decode cleanly")
        return ins

    def test_fixups_are_branch_operands(self):
        for p in (p for p in PATCHES if p["kind"] == "tramp"):
            ins = self.decode(p)
            ends = {i.address - 0x7F000000 + i.size: i for i in ins}
            for off, tgt in p["fixups"]:
                i = ends.get(off + 4)
                self.assertIsNotNone(i, f"{p['id']}: fixup {off} not at an instruction end")
                self.assertTrue(i.mnemonic == "call" or i.mnemonic.startswith("j"), p["id"])
                if tgt >= 0:
                    self.assertEqual(int(i.op_str, 16), tgt, p["id"])

    def test_slot_relocation_moves_by_delta(self):
        disp = re.compile(r"\+ (0x[0-9a-f]+)\]")
        for p in (p for p in PATCHES if p["kind"] == "tramp" and p["group"] == "slots" and "hook" not in p["id"]
                  and not p["id"].startswith(("slots.U64_", "slots.heroes_"))):
            orig = {int(x.group(1), 16) for a in p["asm"] for x in [disp.search(a)] if x}
            new = {int(x.group(1), 16) for i in self.decode(p) for x in [disp.search(i.op_str)] if x}
            imm = re.compile(r"(?:add|lea) \w+, (?:\[\w+ \+ )?(0x[0-9a-f]+)")
            orig |= {int(m.group(1), 16) for a in p["asm"] for m in [imm.search(a)] if m}
            new |= {int(m.group(1), 16) for i in self.decode(p) for m in [imm.search(f"{i.mnemonic} {i.op_str}")] if m}
            orig.add(0)                                   # shapes B/C: index pre-scaled, disp was 0
            moved = {d + META["delta"] for d in orig if d < 0x68}
            self.assertTrue(moved & new, f"{p['id']}: no displacement moved by delta ({orig} -> {new})")


class Header(unittest.TestCase):
    def test_header_matches_json(self):
        try:
            sys.path.insert(0, os.path.join(ROOT, "tools", "patchgen"))
            import build_patches
        except ImportError as e:
            self.skipTest(f"generator deps missing: {e}")
        have = open(os.path.join(ROOT, "loader", "src", "patchset_gen.h")).read()
        self.assertEqual(build_patches.emit_c(PATCHES), have,
                         "patchset_gen.h is stale: run build_patches.py --emit-only")


if __name__ == "__main__":
    unittest.main()

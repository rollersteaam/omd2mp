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
            for ci in p["cave_imms"]:                                       # imms only in copied bytes
                self.assertTrue(any(c <= ci["cave_off"] and ci["cave_off"] + ci["size"] <= c + n
                                    for _, c, n in p["verbatim"]), p["id"])

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
            if p["kind"] != "imm" or p["group"] == "netfix" or "session_alloc" in p["id"]:
                continue
            orig = bytes.fromhex(p["orig"])[p["off"]:p["off"] + p["size"]]
            want = int.from_bytes(orig, "little", signed=True)
            self.assertEqual(eval(p["expr"], {"N": N}), want, p["id"])

    def test_session_alloc_grows(self):
        for p in PATCHES:
            if "session_alloc" in p["id"]:
                for N in range(2, 17):
                    self.assertGreaterEqual(eval(p["expr"], {"N": N}),
                                            META["slot_new_base"] + N * META["slot_size"])


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
        for p in (p for p in PATCHES if p["kind"] == "tramp" and p["group"] == "slots" and "hook" not in p["id"]):
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

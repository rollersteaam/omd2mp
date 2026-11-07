"""
pe32.py - tiny static-analysis toolkit for OrcsMustDie2.exe (32-bit MSVC 2008 build).

Used together with REA (https://rea.tools) which provides the evidence-backed
PE import/export/layout inspection. REA had no native decompiler backend on the
machine this mod was developed on, so code-level work is done here with
capstone + pefile.

Usage (from repo root, with a python that has pefile+capstone+numpy):
    python tools/re/pe32.py strings  <regex>          # find strings + their VAs
    python tools/re/pe32.py xref     <va|"string">     # code that references a VA / string
    python tools/re/pe32.py dis      <va> [count]      # disassemble
    python tools/re/pe32.py func     <va>              # disassemble enclosing function
    python tools/re/pe32.py calls    <va>              # who calls function at va
    python tools/re/pe32.py rtti     [regex]           # MSVC RTTI classes -> vtables
    python tools/re/pe32.py vtable   <ClassName>       # list vtable slots for a class

Set OMD2_EXE to override the exe path.
"""
import os, re, sys, bisect, struct, functools
import pefile, capstone
import numpy as np

EXE = os.environ.get("OMD2_EXE",
    r"C:\Program Files (x86)\Steam\steamapps\common\Orcs Must Die 2\build\game\OrcsMustDie2.exe")


class Image:
    def __init__(self, path=EXE):
        self.path = path
        self.raw = open(path, "rb").read()
        self.pe = pefile.PE(data=self.raw, fast_load=True)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.secs = []
        for s in self.pe.sections:
            name = s.Name.rstrip(b"\0").decode(errors="replace")
            va = self.base + s.VirtualAddress
            self.secs.append((name, va, max(s.Misc_VirtualSize, s.SizeOfRawData), s.PointerToRawData, s.SizeOfRawData))
        t = self.sec(".text")
        self.text_va, self.text = t[1], self.raw[t[3]:t[3] + t[4]]
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True

    # ---------------------------------------------------------------- mapping
    def sec(self, name):
        return next(s for s in self.secs if s[0] == name)

    def off(self, va):
        for n, sva, size, praw, rsize in self.secs:
            if sva <= va < sva + rsize:
                return praw + (va - sva)
        return None

    def va(self, off):
        for n, sva, size, praw, rsize in self.secs:
            if praw <= off < praw + rsize:
                return sva + (off - praw)
        return None

    def read(self, va, n):
        o = self.off(va)
        return self.raw[o:o + n] if o is not None else None

    def u32(self, va):
        b = self.read(va, 4)
        return struct.unpack("<I", b)[0] if b and len(b) == 4 else None

    def cstr(self, va, maxlen=512):
        b = self.read(va, maxlen) or b""
        return b.split(b"\0", 1)[0].decode("latin1")

    def section_of(self, va):
        for n, sva, size, praw, rsize in self.secs:
            if sva <= va < sva + size:
                return n
        return None

    # --------------------------------------------------------------- strings
    def find_strings(self, pattern, wide=False):
        rx = re.compile(pattern.encode() if not wide else pattern.encode("utf-16le"))
        out = []
        for n, sva, size, praw, rsize in self.secs:
            if n not in (".rdata", ".data"):
                continue
            blob = self.raw[praw:praw + rsize]
            for m in re.finditer(rb"[\x20-\x7e]{4,}", blob):
                s = m.group().decode()
                if re.search(pattern, s):
                    out.append((sva + m.start(), s))
        return out

    # ----------------------------------------------------------------- xrefs
    @functools.lru_cache(None)
    def _text_np(self):
        return np.frombuffer(self.text, dtype=np.uint8)

    def imm_refs(self, value):
        """All .text addresses whose bytes contain `value` as LE imm32/disp32,
        validated by decoding an instruction that contains those bytes."""
        needle = struct.pack("<I", value)
        hits, start = [], 0
        while True:
            i = self.text.find(needle, start)
            if i < 0:
                break
            start = i + 1
            # try instruction starts 1..7 bytes before the immediate
            for back in range(1, 8):
                s = i - back
                if s < 0:
                    continue
                try:
                    ins = next(self.md.disasm(self.text[s:s + 16], self.text_va + s, 1))
                except StopIteration:
                    continue
                if s + ins.size > i + 3 and s <= i:
                    if needle in bytes(ins.bytes):
                        hits.append(ins)
                        break
        return hits

    @functools.lru_cache(None)
    def _call_targets(self):
        d = self._text_np()
        pos = np.nonzero(d == 0xE8)[0]
        pos = pos[pos + 5 <= len(d)]
        rel = (d[pos + 1].astype(np.int64) | (d[pos + 2].astype(np.int64) << 8) |
               (d[pos + 3].astype(np.int64) << 16) | (d[pos + 4].astype(np.int64) << 24))
        rel = np.where(rel >= 2**31, rel - 2**32, rel)
        tgt = self.text_va + pos + 5 + rel
        return pos, tgt

    def callers(self, target):
        pos, tgt = self._call_targets()
        return [self.text_va + int(p) for p in pos[tgt == target]]

    # ------------------------------------------------------------- functions
    def func_start(self, va, limit=0x4000):
        """Heuristic: walk back to the nearest CC/90 padding boundary that is
        followed by a plausible prologue and is a call target."""
        pos, tgt = self._call_targets()
        targets = self._targets_sorted()
        i = bisect.bisect_right(targets, va) - 1
        if i >= 0 and va - targets[i] < limit:
            return targets[i]
        return None

    @functools.lru_cache(None)
    def _targets_sorted(self):
        pos, tgt = self._call_targets()
        lo, hi = self.text_va, self.text_va + len(self.text)
        t = np.unique(tgt[(tgt >= lo) & (tgt < hi)])
        # keep only those preceded by padding or a ret (real function entries)
        keep = []
        for a in t.tolist():
            o = a - self.text_va
            prev = self.text[o - 1] if o > 0 else 0xCC
            if prev in (0xCC, 0x90, 0xC3) or self.text[o - 3] == 0xC2:
                keep.append(a)
        return keep

    def func_end(self, start):
        t = self._targets_sorted()
        i = bisect.bisect_right(t, start)
        return t[i] if i < len(t) else start + 0x400

    def dis(self, va, count=40, stop_at_ret=False):
        o = self.off(va)
        out = []
        for ins in self.md.disasm(self.raw[o:o + count * 16], va):
            out.append(ins)
            if len(out) >= count or (stop_at_ret and ins.mnemonic == "ret"):
                break
        return out

    def dis_func(self, start):
        end = self.func_end(start)
        o = self.off(start)
        return list(self.md.disasm(self.raw[o:o + (end - start)], start))

    def annotate(self, ins):
        note = ""
        for op in ins.operands:
            v = None
            if op.type == capstone.x86.X86_OP_IMM:
                v = op.imm & 0xffffffff
            elif op.type == capstone.x86.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0:
                v = op.mem.disp & 0xffffffff
            if v is None:
                continue
            sec = self.section_of(v)
            if sec in (".rdata", ".data"):
                s = self.cstr(v, 80)
                if len(s) >= 3 and all(32 <= ord(c) < 127 for c in s):
                    note = repr(s)
                else:
                    cls = self.rtti_by_vtable().get(v)
                    note = f"vtable {cls}" if cls else f"[{sec}]"
        return note

    def fmt(self, ins):
        return f"{ins.address:08x}  {bytes(ins.bytes).hex():<16} {ins.mnemonic:6} {ins.op_str:40} {self.annotate(ins)}"

    # ------------------------------------------------------------------ RTTI
    @functools.lru_cache(None)
    def rtti_classes(self):
        """Return {type_descriptor_va: demangled-ish name} from '.?AV...@@' strings."""
        res = {}
        d = self.sec(".data")
        blob = self.raw[d[3]:d[3] + d[4]]
        for m in re.finditer(rb"\.\?A[VU]([\w@?$]+?)@@\0", blob):
            name_va = d[1] + m.start()
            td = name_va - 8  # TypeDescriptor: vftable ptr, spare, name[]
            parts = m.group(1).decode().split("@")
            res[td] = "::".join(reversed([p for p in parts if p]))
        return res

    @functools.lru_cache(None)
    def rtti_by_vtable(self):
        """Map vtable VA -> class name via CompleteObjectLocator (COL)."""
        tds = self.rtti_classes()
        r = self.sec(".rdata")
        blob = self.raw[r[3]:r[3] + r[4]]
        arr = np.frombuffer(blob[:len(blob) // 4 * 4], dtype="<u4")
        out = {}
        # COL: sig(0), offset, cdOffset, pTypeDescriptor, pClassHierarchy
        for off in range(4):
            a = np.frombuffer(blob[off:off + (len(blob) - off) // 4 * 4], dtype="<u4")
            idx = np.nonzero(np.isin(a, np.fromiter(tds.keys(), dtype=np.uint32)))[0]
            for i in idx.tolist():
                col_va = r[1] + off + (i - 3) * 4
                if i < 3 or a[i - 3] != 0 or a[i - 2] != 0:
                    continue
                cls = tds[int(a[i])]
                # vtable[-1] == col_va : find pointer to COL in .rdata
                needle = struct.pack("<I", col_va)
                j = blob.find(needle)
                while j >= 0:
                    vt = r[1] + j + 4
                    out[vt] = cls
                    j = blob.find(needle, j + 1)
        return out

    def vtables_of(self, cls):
        return [vt for vt, c in self.rtti_by_vtable().items() if c == cls]

    def vtable_slots(self, vt, maxn=200):
        slots = []
        for k in range(maxn):
            p = self.u32(vt + 4 * k)
            if p is None or self.section_of(p) != ".text":
                break
            if k and vt + 4 * k in self.rtti_by_vtable():
                break
            slots.append(p)
        return slots


def parse_va(img, s):
    if s.startswith("0x") or re.fullmatch(r"[0-9a-fA-F]{6,8}", s):
        return int(s, 16)
    hits = [v for v, t in img.find_strings(re.escape(s)) if t == s]
    if not hits:
        raise SystemExit(f"string not found: {s!r}")
    return hits[0]


def main(argv):
    img = Image()
    cmd, args = argv[0], argv[1:]
    if cmd == "strings":
        for va, s in img.find_strings(args[0]):
            print(f"{va:08x}  {s}")
    elif cmd == "xref":
        v = parse_va(img, args[0])
        for ins in img.imm_refs(v):
            f = img.func_start(ins.address)
            print(f"{ins.address:08x}  func={f and hex(f)}  {ins.mnemonic} {ins.op_str}")
    elif cmd == "dis":
        for ins in img.dis(int(args[0], 16), int(args[1]) if len(args) > 1 else 40):
            print(img.fmt(ins))
    elif cmd == "func":
        va = int(args[0], 16)
        s = img.func_start(va) or va
        print(f"; function {s:08x} .. {img.func_end(s):08x}")
        for ins in img.dis_func(s):
            print(img.fmt(ins))
    elif cmd == "calls":
        for c in img.callers(int(args[0], 16)):
            print(f"{c:08x}  in func {img.func_start(c) and hex(img.func_start(c))}")
    elif cmd == "rtti":
        pat = args[0] if args else ""
        for vt, c in sorted(img.rtti_by_vtable().items(), key=lambda x: x[1]):
            if re.search(pat, c):
                print(f"{vt:08x}  {c}  ({len(img.vtable_slots(vt))} slots)")
    elif cmd == "vtable":
        for vt in img.vtables_of(args[0]):
            print(f"vtable {vt:08x}")
            for k, p in enumerate(img.vtable_slots(vt)):
                print(f"  [{k:3}] +{4*k:#05x} -> {p:08x}")
    else:
        print(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:] or ["help"])

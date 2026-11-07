"""
slotscan.py - inventory every instruction that addresses the Session player-slot array.

Background (docs/RESEARCH.md §3):
  Session  (global ptr at 0xB9D108, malloc(0x36058), ctor 0x4B1AA0)
    +0x38  Slot slots[2]     ; 0x18 bytes each:
                             ;   +0x00 RakNetGUID guid (8 bytes g + u16 systemIndex)
                             ;   +0x10 ptr/state (non-zero == occupied)
                             ;   +0x16/+0x17 flags
    +0x68  u8   dirty flag
    +0x6C  int  localSlot
    +0x70  int  playerCount

MSVC computes &slots[i] as  S + (i*3)*8 + 0x38.  Seen shapes:
  A  lea r,[i+i*2] ... [S + r*8 + d]        d in 0x38..0x67   -> slot field (d-0x38)
  B  add r,3 ; lea r,[r+r*2] ... [S + r*8]                    -> slots[i].+0x10
  C  lea r,[i+i*2+9] ... [S + r*8]                            -> slots[i].+0x10
  D  lea r,[i+i*2] ; lea p,[S + r*8] ; ... [p + d]            -> p is slot base - 0x38
  L  loops: add p,0x18 ... cmp i,2   /  bounds: cmp i,1 ; ja
  K  ctor/dtor style: lea p,[S+0x38] / add p,0x38  then stride 0x18

Relocating the array to S+NEW_BASE means adding DELTA=NEW_BASE-0x38 to the
displacement of the instruction that forms the address (A,B,C: the memory operand;
D: the `lea p,[S+r*8]`; K: the `+0x38`). Loop/bounds constants become N / N-1.

Each site is tagged `session` when its function provably works on the session
(references the 0xB9D108 global, or lives in the Session method cluster) and
`review` otherwise. Only `session` sites are emitted into the patch set.

Output: research/slot_sites.tsv
"""
import os, re, sys, bisect
sys.path.insert(0, os.path.dirname(__file__))
from pe32 import Image
import capstone

SESSION_GLOBAL = 0xB9D108
SESSION_CLUSTER = (0x4B1000, 0x4B4000)   # Session methods are emitted contiguously here
SEND_TO_SLOT = 0x4B20F0                  # Session::SendToSlot(Slot*, BitStream*)
SLOT_LO, SLOT_HI = 0x38, 0x68
MEM = re.compile(r"\[(e\w\w) \+ (e\w\w)\*8(?: \+ (0x[0-9a-f]+))?\]")


def sweep(img):
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.skipdata = True
    return list(md.disasm_lite(img.text, img.text_va))


def session_functions(img):
    fs = set()
    for r in img.imm_refs(SESSION_GLOBAL):
        f = img.func_start(r.address)
        if f:
            fs.add(f)
    return fs


def classify(ins, k, idx):
    """Return slot-index provenance of register `idx` looking back <=8 instrs."""
    for w in reversed(ins[max(0, k - 8):k]):
        mn, ops = w[2], w[3]
        if not ops.startswith(idx + ","):
            continue
        if mn == "lea":
            if re.fullmatch(rf"{idx}, \[(e\w\w) \+ \1\*2\]", ops):
                # was the source pre-incremented by 3 (shape B)?
                return "x3"
            if re.fullmatch(rf"{idx}, \[(e\w\w) \+ \1\*2 \+ 9\]", ops):
                return "x3+9"
        return None
    return None


def main():
    img = Image()
    ins = sweep(img)
    sess = session_functions(img)
    starts = img._targets_sorted()

    def fstart(a):
        i = bisect.bisect_right(starts, a) - 1
        return starts[i] if i >= 0 else None

    def tag(a):
        f = fstart(a)
        ok = (f in sess) or (SESSION_CLUSTER[0] <= a < SESSION_CLUSTER[1])
        return ("session" if ok else "review"), f

    rows = []
    for k, (a, size, mn, ops) in enumerate(ins):
        m = MEM.search(ops)
        if m:
            base, idx, disp = m.group(1), m.group(2), int(m.group(3) or "0", 16)
            prov = classify(ins, k, idx)
            if prov:
                # was idx (+3)'d just before the lea? (shape B)
                plus3 = any(w[2] == "add" and w[3] == f"{idx}, 3" for w in ins[max(0, k - 8):k])
                if prov == "x3" and SLOT_LO <= disp < SLOT_HI:
                    kind = f"A:slot+{disp - 0x38:#x}"
                elif prov == "x3" and disp == 0 and plus3:
                    kind = "B:slot+0x10"
                elif prov == "x3+9" and disp == 0:
                    kind = "C:slot+0x10"
                elif prov == "x3" and disp == 0 and mn == "lea":
                    kind = "D:slot-0x38 base"
                else:
                    kind = None
                if kind:
                    t, f = tag(a)
                    rows.append((a, size, kind, t, f, f"{mn} {ops}"))
                    continue
        if mn == "cmp" and re.fullmatch(r"e\w\w, 2", ops):
            if any(w[2] == "add" and w[3].endswith(", 0x18") for w in ins[max(0, k - 5):k]):
                t, f = tag(a)
                rows.append((a, size, "L:loop<2", t, f, f"{mn} {ops}"))
        if mn == "cmp" and re.fullmatch(r"e\w\w, 1", ops) and k + 1 < len(ins) and ins[k + 1][2] == "ja":
            nxt = ins[k + 2:k + 5]
            if any(w[2] == "lea" and re.search(r"\[(e\w\w) \+ \1\*2", w[3]) for w in nxt):
                t, f = tag(a)
                rows.append((a, size, "L:bound<=1", t, f, f"{mn} {ops}"))
        # an esp/ebp base is a stack local (e.g. the SEH frame at 0x4B1ACD), never the Session
        # The stride add can be far away: Session::Broadcast (0x4B2150) forms the base at
        # 0x4B2161 and steps it 47 instructions later, after the send call. So look ahead to
        # the end of the function for `add <same register>, 0x18`.
        km = None
        if mn == "lea":
            km = re.fullmatch(r"(e\w\w), \[e(?!sp|bp)\w\w \+ 0x38\]", ops)
        elif mn == "add":
            km = re.fullmatch(r"(e(?!sp|bp)\w\w), 0x38", ops)
        # `add r, 0x38; push r; call Session::SendToSlot (0x4B20F0)` passes &slots[0] (the
        # host, seen from a client) with no stride step anywhere: e.g. Level_Load's 0x89 ack
        # at 0x4A75F9. Without this, real clients answer the host at the stale old slot.
        if km and mn == "add":
            nxt = ins[k + 1:k + 6]
            if any(w[2] == "push" and w[3] == km.group(1) for w in nxt[:3]) and \
               any(w[2] == "call" and w[3] == hex(SEND_TO_SLOT) for w in nxt):
                t, f = tag(a)
                rows.append((a, size, "K:slot0+0x38 send", t, f, f"{mn} {ops}"))
                continue
        if km:
            for w in ins[k + 1:k + 80]:
                if w[2] == "ret":
                    break
                if w[2] == "add" and w[3] == f"{km.group(1)}, 0x18":
                    t, f = tag(a)
                    rows.append((a, size, "K:base+0x38", t, f, f"{mn} {ops}"))
                    break
    rows.sort()
    out = os.path.join(os.path.dirname(__file__), "..", "..", "research", "slot_sites.tsv")
    with open(out, "w") as fh:
        fh.write("va\tsize\tkind\ttag\tfunction\tinstruction\n")
        for a, sz, kd, t, f, s in rows:
            fh.write(f"{a:#010x}\t{sz}\t{kd}\t{t}\t{f and hex(f)}\t{s}\n")
    from collections import Counter
    print(Counter((r[2].split(':')[0], r[3]) for r in rows))
    print(len(rows), "sites ->", os.path.normpath(out))


if __name__ == "__main__":
    main()

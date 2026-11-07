"""
bindings.py - dump the game's script/UI binding table.

OMD2 registers every script-callable native ("Net_PlayerCount", "Gear_EquipAtSlot",
"Level_GetCurrentCoopContributionPercent", ...) from tiny static initializers:

    push  <handler VA>
    mov   eax, <name string VA>
    call  0x4103c0            ; RegisterBinding(name=eax, handler=[esp])

This script scans .text for that exact shape and writes research/bindings.tsv
(name, handler VA). Pass a regex to filter the printed output.
"""
import os, re, sys, struct
sys.path.insert(0, os.path.dirname(__file__))
from pe32 import Image

REGISTER = 0x4103C0


def scan(img):
    t, base = img.text, img.text_va
    out = []
    for m in re.finditer(rb"\x68(....)\xb8(....)\xe8(....)", t, re.S):
        handler, name_va, rel = struct.unpack("<IIi", m.group(1) + m.group(2) + m.group(3))
        call_end = base + m.start() + 15
        if call_end + rel != REGISTER:
            continue
        out.append((img.cstr(name_va), handler))
    return sorted(out)


if __name__ == "__main__":
    img = Image()
    rows = scan(img)
    root = os.path.join(os.path.dirname(__file__), "..", "..", "research")
    os.makedirs(root, exist_ok=True)
    with open(os.path.join(root, "bindings.tsv"), "w") as f:
        f.write("name\thandler_va\n")
        for n, h in rows:
            f.write(f"{n}\t{h:#010x}\n")
    pat = sys.argv[1] if len(sys.argv) > 1 else ""
    for n, h in rows:
        if re.search(pat, n):
            print(f"{h:08x}  {n}")
    print(f"# {len(rows)} bindings -> research/bindings.tsv", file=sys.stderr)

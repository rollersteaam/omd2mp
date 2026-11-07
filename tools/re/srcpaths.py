"""List the source-file paths (assert/log strings) embedded in a binary's strings dump.
Usage: python tools/re/srcpaths.py strings.txt"""
import re, sys

BS = chr(92)
lines = open(sys.argv[1], encoding="latin1").read().splitlines()
paths = set()
for x in lines:
    if BS in x and re.search(r"(?i)\.(cpp|h|inl)$", x):
        i = x.lower().rfind(BS + "src" + BS)
        paths.add(x[i + 5:] if i >= 0 else x)
print(len(paths))
print("\n".join(sorted(paths)))

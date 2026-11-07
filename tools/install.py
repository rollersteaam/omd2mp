"""
install.py - copy the built loader + ini into the game folder, or remove them.

  python tools/install.py [--game "<Steam>/steamapps/common/Orcs Must Die 2"] [--uninstall]

--game may be the Steam install root or the folder holding the game exe. The real game is
build/game/OrcsMustDie2.exe (6 MB); build/release/OrcsMustDie2.exe is a small Qt launcher
and is NOT the exe we patch. We pick the folder whose exe has the supported build stamp.

Only ever writes/removes version.dll and omd2mp.ini (and reads omd2mp.log). Never touches
game files. Refuses to overwrite a version.dll it did not install (checked by export of
the omd2mp marker string).
"""
import argparse, os, shutil, struct, sys
ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
DEFAULT_GAME = r"C:\Program Files (x86)\Steam\steamapps\common\Orcs Must Die 2"
MARKER = b"omd2mp loader"
GAME_STAMP = 0x51DD91A9          # PE TimeDateStamp of the supported game build (see dllmain.c)


def pe_stamp(exe):
    """PE header TimeDateStamp, read with struct (no pefile needed)."""
    with open(exe, "rb") as f:
        h = f.read(0x400)
    e = struct.unpack_from("<I", h, 0x3C)[0]
    return struct.unpack_from("<I", h, e + 8)[0] if h[e:e + 4] == b"PE" + bytes(2) else None


def find_game_dir(path):
    """Return (dir, problem). Tries path itself, then path/build/game."""
    seen = []
    for d in (path, os.path.join(path, "build", "game")):
        exe = os.path.join(d, "OrcsMustDie2.exe")
        if os.path.exists(exe):
            st = pe_stamp(exe)
            if st == GAME_STAMP:
                return d, None
            seen.append(f"{exe} (build stamp {st and hex(st)}, want {GAME_STAMP:#x})")
    if seen:
        return None, "found OrcsMustDie2.exe but not the supported game build:\n  " + "\n  ".join(seen)
    return None, f"OrcsMustDie2.exe not found in {path} or {os.path.join(path, 'build', 'game')}"


def ours(path):
    return os.path.exists(path) and MARKER in open(path, "rb").read()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", default=DEFAULT_GAME)
    ap.add_argument("--uninstall", action="store_true")
    a = ap.parse_args()
    game, problem = find_game_dir(a.game)
    if problem:
        sys.exit(problem)
    a.game = game
    dll = os.path.join(a.game, "version.dll")
    if os.path.exists(dll) and not ours(dll):
        sys.exit(f"{dll} exists and is not omd2mp's; refusing to touch it")
    if a.uninstall:
        for f in ("version.dll", "omd2mp.ini"):
            p = os.path.join(a.game, f)
            if os.path.exists(p):
                os.remove(p)
                print("removed", p)
        return
    built = os.path.join(ROOT, "build", "msvc", "Release", "version.dll")
    if not os.path.exists(built):
        sys.exit("build the loader first (see CMakeLists.txt)")
    shutil.copy2(built, dll)
    ini = os.path.join(a.game, "omd2mp.ini")
    if not os.path.exists(ini):
        shutil.copy2(os.path.join(ROOT, "dist", "omd2mp.ini"), ini)
    print("installed", dll, "and", ini, "- results go to omd2mp.log")


if __name__ == "__main__":
    main()

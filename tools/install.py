"""
install.py - copy the built loader + ini into the game folder, or remove them.

  python tools/install.py [--game "<Steam>/steamapps/common/Orcs Must Die 2"] [--uninstall]

Only ever writes/removes version.dll and omd2mp.ini (and reads omd2mp.log). Never touches
game files. Refuses to overwrite a version.dll it did not install (checked by export of
the omd2mp marker string).
"""
import argparse, os, shutil, sys
ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
DEFAULT_GAME = r"C:\Program Files (x86)\Steam\steamapps\common\Orcs Must Die 2"
MARKER = b"omd2mp loader"


def ours(path):
    return os.path.exists(path) and MARKER in open(path, "rb").read()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", default=DEFAULT_GAME)
    ap.add_argument("--uninstall", action="store_true")
    a = ap.parse_args()
    dll = os.path.join(a.game, "version.dll")
    if not os.path.exists(os.path.join(a.game, "OrcsMustDie2.exe")):
        sys.exit(f"OrcsMustDie2.exe not found in {a.game}")
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

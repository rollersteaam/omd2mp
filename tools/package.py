"""
package.py - build the loader and zip everything a human tester needs.

  python tools/package.py [--skip-tests] [--skip-build] [--allow-dirty] [--out build/package]

Steps:
  1. runs tests/run_tests.py (unless --skip-tests)
  2. configures + builds the Win32 Release loader with CMake (unless --skip-build)
  3. stages build/package/omd2mp-<version>/ and zips it to build/package/omd2mp-<version>.zip

<version> is `git describe --always --dirty`. A dirty tree is refused unless --allow-dirty,
so every zip you send out maps to a commit.

The zip needs no Python on the tester's PC:
  install.bat / uninstall.bat / collect-logs.bat  (wrappers around install.ps1)
  files/version.dll, files/omd2mp.ini             (ini with [dev] and [mpdebug] forced off)
  README.txt                                      (tester instructions + version)
"""
import argparse, datetime, os, re, shutil, subprocess, sys, zipfile
ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
BUILD_DIR = os.path.join(ROOT, "build", "msvc")
DLL = os.path.join(BUILD_DIR, "Release", "version.dll")
TESTER = os.path.join(ROOT, "dist", "tester")
MARKER = b"omd2mp loader"

README = """omd2mp {version} - Orcs Must Die! 2 4-player co-op (TEST BUILD)
Built {date}

This is an experimental mod. It changes nothing in the game's own files: it only adds
version.dll and omd2mp.ini next to OrcsMustDie2.exe, and uninstall.bat removes them.

INSTALL
  1. Close Orcs Must Die! 2.
  2. Unzip this folder anywhere (do not run it from inside the zip).
  3. Double-click install.bat. It finds the game through Steam.
     If it can't: in Steam right-click the game > Manage > Browse local files, copy the
     folder path, open a command prompt here and run:  install.bat "<that folder>"
  4. Start the game from Steam as normal.

PLAYING
  * EVERY player in the party must install this exact version ({version}).
  * One player hosts ("Create party"), the others join through Steam as usual.
  * Party size is set in omd2mp.ini (players=4). Everyone must use the same value.

AFTER A SESSION (especially after a crash, desync or disconnect)
  Double-click collect-logs.bat. It puts omd2mp-logs-<date>.zip on your Desktop.
  Send that zip back with a short note: what happened, which wave, who was hosting.

UNINSTALL
  Double-click uninstall.bat. The game is back to vanilla.
"""


def run(cmd, **kw):
    print(">", " ".join(cmd), flush=True)
    r = subprocess.run(cmd, cwd=ROOT, **kw)
    if r.returncode:
        sys.exit(f"failed ({r.returncode}): {' '.join(cmd)}")


def git(*args):
    return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True).stdout.strip()


def tester_ini(template):
    """dist/omd2mp.ini with every `enabled=` in [dev] and [mpdebug] forced to 0."""
    out, sec = [], None
    for line in template.splitlines():
        m = re.match(r"^\s*\[([^\]]+)\]", line)
        if m:
            sec = m.group(1).strip().lower()
        elif sec in ("dev", "mpdebug") and re.match(r"^\s*(enabled|allow_write)\s*=", line):
            line = line.split("=", 1)[0] + "=0"
        out.append(line)
    return "\r\n".join(out) + "\r\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--skip-tests", action="store_true")
    ap.add_argument("--skip-build", action="store_true", help="package the existing build/msvc DLL")
    ap.add_argument("--allow-dirty", action="store_true", help="package uncommitted changes")
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "package"))
    a = ap.parse_args()

    version = git("describe", "--always", "--dirty") or "unknown"
    if version.endswith("-dirty") and not a.allow_dirty:
        sys.exit("working tree has uncommitted changes; commit first or pass --allow-dirty")

    if not a.skip_tests:
        run([sys.executable, os.path.join("tests", "run_tests.py")])
    if not a.skip_build:
        if not os.path.exists(os.path.join(BUILD_DIR, "CMakeCache.txt")):
            run(["cmake", "-S", ".", "-B", BUILD_DIR, "-G", "Visual Studio 16 2019", "-A", "Win32"])
        run(["cmake", "--build", BUILD_DIR, "--config", "Release", "--target", "omd2mp_loader"])
    if not os.path.exists(DLL):
        sys.exit(f"{DLL} missing; build the loader first")
    if MARKER not in open(DLL, "rb").read():
        sys.exit(f"{DLL} has no '{MARKER.decode()}' marker; the installer would refuse to update it")

    name = f"omd2mp-{version}"
    stage = os.path.join(a.out, name)
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(os.path.join(stage, "files"))
    shutil.copy2(DLL, os.path.join(stage, "files", "version.dll"))
    with open(os.path.join(ROOT, "dist", "omd2mp.ini")) as f:
        ini = tester_ini(f.read())
    with open(os.path.join(stage, "files", "omd2mp.ini"), "w", newline="") as f:
        f.write(ini)
    for fn in ("install.ps1", "install.bat", "uninstall.bat", "collect-logs.bat"):
        shutil.copy2(os.path.join(TESTER, fn), os.path.join(stage, fn))
    date = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")
    with open(os.path.join(stage, "README.txt"), "w", newline="") as f:
        f.write(README.format(version=version, date=date).replace("\n", "\r\n"))

    zpath = os.path.join(a.out, name + ".zip")
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for d, _, files in os.walk(stage):
            for fn in sorted(files):
                p = os.path.join(d, fn)
                z.write(p, os.path.join(name, os.path.relpath(p, stage)))
    print(f"\npackaged {zpath} ({os.path.getsize(zpath) // 1024} KB)")


if __name__ == "__main__":
    main()

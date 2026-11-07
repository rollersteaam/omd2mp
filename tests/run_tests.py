"""
run_tests.py - run every unit test in the repo.

  python tests/run_tests.py

* Python: unittest discovery over tests/python (stdlib only; capstone/pefile tests skip
  themselves when those packages are missing - use the rea venv to run everything).
* C: builds each tests/c/test_*.c (see C_TESTS) against its loader/src sources with gcc if present,
  otherwise uses an existing CMake build in build/msvc.
"""
import os, shutil, subprocess, sys, unittest
ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))


def python_tests():
    suite = unittest.defaultTestLoader.discover(os.path.join(ROOT, "tests", "python"))
    return unittest.TextTestRunner(verbosity=1).run(suite).wasSuccessful()


C_TESTS = {   # test name -> sources under loader/src
    "test_patchcore": ["patchcore.c"],
    "test_devproto": ["devproto.c"],
}


def c_tests():
    os.makedirs(os.path.join(ROOT, "build"), exist_ok=True)
    src = os.path.join(ROOT, "loader", "src")
    ok = True
    for name, libs in C_TESTS.items():
        exe = os.path.join(ROOT, "build", name + ".exe")
        if shutil.which("gcc"):
            cmd = ["gcc", "-std=c99", "-Wall", "-Wextra", "-Wno-unused-function", "-I" + src]
            cmd += [os.path.join(src, f) for f in libs]
            cmd += [os.path.join(ROOT, "tests", "c", name + ".c"), "-o", exe, "-lm"]
            if subprocess.call(cmd):
                ok = False
                continue
        else:
            exe = os.path.join(ROOT, "build", "msvc", "Release", name + ".exe")
            if not os.path.exists(exe):
                print("C tests: no gcc and no CMake build; see CMakeLists.txt")
                return False
        ok = subprocess.call([exe]) == 0 and ok
    return ok


if __name__ == "__main__":
    ok = python_tests()
    ok = c_tests() and ok
    print("ALL TESTS PASSED" if ok else "TESTS FAILED")
    sys.exit(0 if ok else 1)

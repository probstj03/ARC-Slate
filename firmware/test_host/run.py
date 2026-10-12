"""Builds and runs the host tests of the pure ARC-Clock modules.

    py test_host/run.py

Compiler: $ARC_CXX, else g++ / clang++ / c++ from PATH, else `zig c++` (pip install ziglang;
set $ARC_ZIG_DIR to the directory containing zig.exe if it is not on PATH).
ArduinoJson comes from .pio/libdeps (run `pio run -e clock` once).
"""
import glob
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.dirname(HERE)
CORE = os.path.join(FW, "src", "clock", "core")


def find_compiler():
    if os.environ.get("ARC_CXX"):
        return os.environ["ARC_CXX"].split()
    for name in ("g++", "clang++", "c++"):
        if shutil.which(name):
            return [name]
    zig_dir = os.environ.get("ARC_ZIG_DIR")
    zig = os.path.join(zig_dir, "zig.exe") if zig_dir else shutil.which("zig")
    if zig and os.path.exists(zig):
        return [zig, "c++"]
    sys.exit("Kein C++-Compiler gefunden (g++, clang++ oder ziglang).")


def find_arduinojson():
    hits = glob.glob(os.path.join(FW, ".pio", "libdeps", "*", "ArduinoJson", "src"))
    if not hits:
        sys.exit("ArduinoJson nicht gefunden: einmal `pio run -e clock` ausfuehren.")
    return hits[0]


def main():
    out = os.path.join(HERE, "test_core.exe" if os.name == "nt" else "test_core")
    sources = [os.path.join(HERE, "test_core.cpp")] + sorted(glob.glob(os.path.join(CORE, "*.cpp")))
    cmd = find_compiler() + ["-std=c++17", "-O1", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-nullability-completeness",
                             "-I", CORE, "-I", find_arduinojson(), "-o", out] + sources
    print(" ".join(cmd[:2]), "...", len(sources), "files")
    if subprocess.call(cmd) != 0:
        sys.exit(1)
    rc = subprocess.call([out])
    os.remove(out)
    sys.exit(rc)


if __name__ == "__main__":
    main()

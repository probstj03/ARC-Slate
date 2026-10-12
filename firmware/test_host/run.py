"""Builds and runs the host tests of the pure ARC-Clock modules and renders a preview of the e-ink.

    py test_host/run.py            tests + preview (test_host/out/epd_preview*.png)
    py test_host/run.py --tests    tests only
    py test_host/run.py --preview  preview only

Compiler: $ARC_CXX, else g++ / clang++ / c++ from PATH, else `zig c++` (pip install ziglang;
set $ARC_ZIG_DIR to the directory containing zig.exe if it is not on PATH).
ArduinoJson and Adafruit GFX come from .pio/libdeps (run `pio run -e clock` once).
The preview needs Pillow (pip install pillow) for the PNG conversion.
"""
import glob
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.dirname(HERE)
CORE = os.path.join(FW, "src", "clock", "core")
LAYOUT = os.path.join(FW, "src", "clock", "layout")
OUT = os.path.join(HERE, "out")

COMMON_FLAGS = ["-std=c++17", "-O1", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-nullability-completeness"]


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


def libdep(name, sub="src"):
    hits = glob.glob(os.path.join(FW, ".pio", "libdeps", "*", name, sub))
    if not hits:
        sys.exit("%s nicht gefunden: einmal `pio run -e clock` ausfuehren." % name)
    return hits[0]


def exe(name):
    return os.path.join(OUT, name + (".exe" if os.name == "nt" else ""))


def build(cmd):
    print(" ".join(cmd[:2]), "...")
    if subprocess.call(cmd) != 0:
        sys.exit(1)


def run_tests():
    os.makedirs(OUT, exist_ok=True)
    sources = [os.path.join(HERE, "test_core.cpp")] + sorted(glob.glob(os.path.join(CORE, "*.cpp")))
    build(find_compiler() + COMMON_FLAGS + ["-I", CORE, "-I", libdep("ArduinoJson"), "-o", exe("test_core")] + sources)
    rc = subprocess.call([exe("test_core")])
    if rc != 0:
        sys.exit(rc)


def run_preview():
    os.makedirs(OUT, exist_ok=True)
    gfx = libdep("Adafruit GFX Library", ".")
    sources = ([os.path.join(HERE, "render_epd.cpp"), os.path.join(gfx, "Adafruit_GFX.cpp"), os.path.join(LAYOUT, "epd_layout.cpp")] +
               sorted(glob.glob(os.path.join(CORE, "*.cpp"))))
    build(find_compiler() + COMMON_FLAGS + ["-DARDUINO=10819", "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0", "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0",
                                            "-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0", "-DARDUINOJSON_ENABLE_PROGMEM=0", "-I", os.path.join(HERE, "stubs"), "-I", gfx, "-I", CORE,
                                            "-I", libdep("ArduinoJson"), "-o", exe("render_epd")] + sources)
    try:
        from PIL import Image
    except ImportError:
        Image = None
        print("Pillow fehlt: es bleiben nur .pgm-Dateien (pip install pillow)")
    for variant, name in ((0, "epd_preview"), (1, "epd_preview_offline"), (2, "epd_preview_snow")):
        pgm = os.path.join(OUT, name + ".pgm")
        if subprocess.call([exe("render_epd"), pgm, str(variant)]) != 0:
            sys.exit(1)
        if Image:
            Image.open(pgm).convert("L").save(os.path.join(OUT, name + ".png"))
            os.remove(pgm)
    print("Vorschau:", OUT)


def main():
    only_tests = "--tests" in sys.argv
    only_preview = "--preview" in sys.argv
    if not only_preview:
        run_tests()
    if not only_tests:
        run_preview()


if __name__ == "__main__":
    main()

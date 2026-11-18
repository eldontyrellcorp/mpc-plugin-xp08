#!/usr/bin/env python3
"""Build XP-08: native build, the skin (tools/make_skin.py), the full MPC build, then roms/.

    python3 tools/build.py [--no-roms] [SDK build.py options...]

1. The MPC Plugin SDK's tools/build.py --target native (the skin script needs build/params.json).
2. tools/make_skin.py: skins/ (the SDK build copies it as Plugin Skins/).
3. The SDK's tools/build.py with your options (default: native + arm32 + arm64 + validator).
4. A roms/ folder in the output folder, with roms/README.txt and the dumps you put in this
   project's roms/ (unless --no-roms: then only the README, for a folder you may share).

The SDK is looked for in $SDK, then ../mpc-plugin-builder-sdk.
"""
import glob
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SDK = os.path.abspath(os.environ.get("SDK", os.path.join(ROOT, "..", "mpc-plugin-builder-sdk")))


def sdk_build(args):
    cmd = [sys.executable, os.path.join(SDK, "tools", "build.py"), ROOT] + args
    if subprocess.run(cmd).returncode != 0:
        sys.exit("ERROR: the SDK build failed (messages above)")


def main():
    args = [a for a in sys.argv[1:] if a != "--no-roms"]
    with_roms = "--no-roms" not in sys.argv[1:]
    sdk_build(["--target", "native"])
    if subprocess.run([sys.executable, os.path.join(ROOT, "tools", "make_skin.py")]).returncode != 0:
        sys.exit("ERROR: make_skin.py failed")
    if "native" not in args:
        sdk_build(args)
    out = os.path.join(ROOT, "build", "Stephane Schmitz - VST - XP-08")
    if os.path.isdir(out):
        roms = os.path.join(out, "roms")
        os.makedirs(roms, exist_ok=True)
        shutil.copyfile(os.path.join(ROOT, "roms", "README.txt"), os.path.join(roms, "README.txt"))
        n = 0
        if with_roms:
            for f in sorted(glob.glob(os.path.join(ROOT, "roms", "*"))):
                if os.path.isfile(f) and not f.endswith("README.txt"):
                    shutil.copyfile(f, os.path.join(roms, os.path.basename(f)))
                    n += 1
        print("roms/: README.txt%s" % (" and %d dump%s" % (n, "" if n == 1 else "s") if n else
                                       " (no dumps: put them in roms/ here or in the plugin folder on the MPC)"))


if __name__ == "__main__":
    main()

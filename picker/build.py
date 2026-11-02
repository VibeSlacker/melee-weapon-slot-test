#!/usr/bin/env python3
"""Build Melee Picker: fill the signature table into the Lua, pack it as a Bingus patch, and zip it for HellForge.

  build.py   -> build/melee_picker.built.lua, build/9ba626afa44a3aa3.patch_0 (+ two empty side files), Melee-Picker-<version>.zip
"""
import json, os, sys, zipfile
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pack

VERSION = "1.0.9"
GUID = "6b0c4f1e-2d7a-4e5b-9a3c-7f1d8e2b5c90"

def lua_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'

def main():
    sigs = json.load(open(os.path.join(HERE, "data", "sigs.json")))
    sig_lua = "{\n" + "".join(
        "\t{ name = %s, symbol = %s, anchor_offset = %d, extract_offset = %d, extract_end = %d, optional = %s,\n"
        "\t  hex = %s,\n\t  mask = %s,\n\t  anchor = %s },\n" % (
            lua_str(s["name"]), lua_str(s["symbol"]), s["anchor_offset"], s["extract_offset"], s["extract_end"],
            "true" if s.get("optional") else "false", lua_str(s["hex"]), lua_str(s["mask"]), lua_str(s["anchor"]))
        for s in sigs) + "}"
    src = open(os.path.join(HERE, "src", "melee_picker.lua"), encoding="utf-8").read()
    if src.count("--@SIGS@") != 1:
        sys.exit("placeholder --@SIGS@ must appear exactly once")
    src = src.replace("--@SIGS@", sig_lua)
    out = os.path.join(HERE, "build")
    os.makedirs(out, exist_ok=True)
    open(os.path.join(out, "melee_picker.built.lua"), "w", encoding="utf-8", newline="\n").write(src)
    name, blob = pack.build(src.encode("utf-8"))
    base = os.path.join(out, pack.ARCHIVE + ".patch_0")
    open(base, "wb").write(blob)
    for ext in (".gpu_resources", ".stream"):
        open(base + ext, "wb").close()
    desc = ("Pick the extra melee weapon of Dedicated Melee Slot (Machete, Stun Lance, Saber, Stun Baton or Combat Hatchet) "
            "from one small box on the player card. Requires Bingus Shared Loader v15+, loaded last, and Dedicated Melee Slot 1.5.0+.")
    manifest = {"Version": 1, "Guid": GUID, "Name": "Melee Picker " + VERSION, "Description": desc,
                "Options": [{"Name": "Melee Picker", "Description": desc, "Include": ["Addon"]}]}
    zpath = os.path.join(HERE, "Melee-Picker-%s.zip" % VERSION)
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("manifest.json", json.dumps(manifest, indent=2))
        for ext in ("", ".gpu_resources", ".stream"):
            z.write(base + ext, "Addon/" + pack.ARCHIVE + ".patch_0" + ext)
    print("built %s: %d bytes of Lua -> %s" % (name, len(src.encode()), zpath))

if __name__ == "__main__":
    main()

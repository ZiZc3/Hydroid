# make_app.py — one game's PS5 app: the Hydroid engine build + the game's settings and art.
#
#   python tools/make_app.py games/subway-surfers --engine <the engine's title folder>
#   -> dist/<title id>/  (install it as a homebrew title; the game's own file goes in its game folder)
#
# Needs Pillow and etcpak (pip install pillow etcpak).
import argparse
import json
import os
import shutil
import struct
import sys

from PIL import Image


def write_bc7_dds(img, path):
    """The PS5's background picture: 3840x2160 BC7 in a DDS file."""
    import etcpak
    w, h = img.size
    data = etcpak.compress_bc7(img.convert("RGBA").tobytes(), w, h)
    hdr = struct.pack("<4sIIIIIII44x", b"DDS ", 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000, h, w, len(data), 1, 1)
    pf = struct.pack("<II4s5I", 32, 0x4, b"DX10", 0, 0, 0, 0, 0)
    caps = struct.pack("<5I", 0x1000, 0, 0, 0, 0)
    dx10 = struct.pack("<5I", 98, 3, 0, 1, 1)  # BC7_UNORM, 2D, one picture
    with open(path, "wb") as f:
        f.write(hdr + pf + caps + dx10 + data)


def background(path):
    """Any picture as the 16:9 background (cut to 16:9 in the middle, then 3840x2160)."""
    img = Image.open(path).convert("RGB")
    w, h = img.size
    if w / h > 16 / 9:
        nw = round(h * 16 / 9)
        img = img.crop(((w - nw) // 2, 0, (w - nw) // 2 + nw, h))
    else:
        nh = round(w * 9 / 16)
        img = img.crop((0, (h - nh) // 2, w, (h - nh) // 2 + nh))
    return img.resize((3840, 2160), Image.LANCZOS)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("game", help="the game's folder, e.g. games/subway-surfers")
    ap.add_argument("--engine", required=True, help="Hydroid's built title folder (dist/PPSA26369)")
    ap.add_argument("--out", default="dist")
    a = ap.parse_args()
    with open(os.path.join(a.game, "game.json")) as f:
        g = json.load(f)
    if len(g["label"]) != 16 or not g["label"].isalnum():
        sys.exit("game.json label: 16 letters/digits")
    if not os.path.isfile(os.path.join(a.engine, "eboot.bin")):
        sys.exit("--engine: no eboot.bin there")
    out = os.path.join(a.out, g["titleId"])
    shutil.rmtree(out, ignore_errors=True)
    # the engine and its files, not its games, logs or settings
    skip = {"games", "game", "data", "android.log", "android-old.log", "trace.txt", ".uploaded",
            "game.apk", "game.xapk", "game.apks"}
    shutil.copytree(a.engine, out, ignore=lambda d, names: [n for n in names if d == a.engine and n in skip])
    os.makedirs(os.path.join(out, "game"), exist_ok=True)
    with open(os.path.join(out, "single_game.txt"), "w", newline="\n") as f:
        f.write(g["package"] + "\n")  # the engine starts this game, no launcher
    with open(os.path.join(a.engine, "sce_sys", "param.json")) as f:
        p = json.load(f)
    p["titleId"] = g["titleId"]
    p["contentId"] = f"UP9000-{g['titleId']}_00-{g['label']}"
    p["conceptId"] = g["titleId"][4:]
    p["localizedParameters"]["en-US"]["titleName"] = g["name"]
    with open(os.path.join(out, "sce_sys", "param.json"), "w", newline="\n") as f:
        json.dump(p, f, indent=4)
    Image.open(os.path.join(a.game, "icon0.png")).convert("RGBA").resize((512, 512), Image.LANCZOS).save(
        os.path.join(out, "sce_sys", "icon0.png"))
    bg = background(os.path.join(a.game, "background.png"))
    write_bc7_dds(bg, os.path.join(out, "sce_sys", "pic0.dds"))
    write_bc7_dds(bg, os.path.join(out, "sce_sys", "pic1.dds"))
    print(f"{out}: {g['name']} ({g['titleId']}, {g['package']})")


if __name__ == "__main__":
    main()

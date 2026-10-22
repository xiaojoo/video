"""Regenerate the icon archive from the two SVGs.

    python player/icons/make.py

app.ico carries 16 and 24 px from the simplified cut (app_small.svg) and 32/48/64/128/256
from app.svg. Every frame is rastered natively at its own size - not downscaled from a
master - because the small sizes are the whole point. This one file is both the exe's file
icon (appPlayerui.rc) and Qt's window icon, so the two surfaces cannot drift.

Needs a Chromium-family browser for the SVG raster (this machine has no cairosvg, rsvg or
ImageMagick). The screenshot path has to use forward slashes and the transparent
background is 00000000; the throwaway profile directory is removed at the end (Edge keeps
handles on it, so the removal ignores errors).
"""
import io
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
BROWSERS = [
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
]
SMALL = {16, 24}
SIZES = [16, 24, 32, 48, 64, 128, 256]


def browser():
    for b in BROWSERS:
        if os.path.exists(b):
            return b
    sys.exit("no Edge or Chrome found for the SVG raster")


def raster(svg, px, out, profile):
    page = HERE / ".raster.html"
    page.write_text(
        '<html><head><style>html,body{margin:0;padding:0;background:transparent}'
        'img{display:block;width:%dpx;height:%dpx}</style></head>'
        '<body><img src="%s"></body></html>' % (px, px, svg), encoding="utf-8")
    subprocess.run([browser(), "--headless=new", "--disable-gpu", "--hide-scrollbars",
                    "--no-first-run", "--user-data-dir=" + profile,
                    "--default-background-color=00000000", "--window-size=%d,%d" % (px, px),
                    "--screenshot=" + str(out).replace("\\", "/"),
                    "file:///" + str(page).replace("\\", "/")],
                   check=True, capture_output=True)
    page.unlink()
    if not out.exists():
        sys.exit("nothing was written for %s at %d px" % (svg, px))


def main():
    profile = tempfile.mkdtemp(prefix="svgshot-").replace("\\", "/")
    tmp = Path(tempfile.gettempdir())
    frames = {}
    for n in SIZES:
        src = "app_small.svg" if n in SMALL else "app.svg"
        out = tmp / ("icon_%s_%d.png" % (src[:-4], n))
        raster(src, n, out, profile)
        img = Image.open(out).convert("RGBA")
        if img.size != (n, n):
            sys.exit("%s came back as %s, expected %dx%d" % (src, img.size, n, n))
        frames[n] = img
    order = sorted(SIZES, reverse=True)
    frames[order[0]].save(HERE / "app.ico", format="ICO", sizes=[(n, n) for n in order],
                          append_images=[frames[n] for n in order[1:]])
    d = open(HERE / "app.ico", "rb").read()
    count = struct.unpack_from("<H", d, 4)[0]
    got = sorted({(struct.unpack_from("<BBBBHHII", d, 6 + 16 * i)[0] or 256)
                for i in range(count)})
    print("wrote app.ico: %d frames %s, %d bytes" % (count, got, len(d)))
    if got != sorted(SIZES):
        shutil.rmtree(profile, ignore_errors=True)
        sys.exit("the .ico does not carry every size it should")
    shutil.rmtree(profile, ignore_errors=True)


main()

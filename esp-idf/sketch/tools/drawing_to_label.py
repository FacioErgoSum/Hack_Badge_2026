#!/usr/bin/env python3
"""Turn a drawing from the gallery website into the badge gallery's title card.

Draw the label on the badge, erase to upload it, then:

    python tools/drawing_to_label.py https://YOUR-SITE.netlify.app            # the newest drawing
    python tools/drawing_to_label.py https://YOUR-SITE.netlify.app "d/0001..." # a specific one

The key of a drawing is the part after # in the website's address while it's
showing. Writes src/gallery_label.h; rebuild and flash to use it. Delete that
file to go back to the plain "GALLERY" title.
"""
import base64
import json
import pathlib
import struct
import sys
import urllib.parse
import urllib.request

OUT = pathlib.Path(__file__).resolve().parent.parent / "src" / "gallery_label.h"


def get(url):
    with urllib.request.urlopen(url, timeout=20) as r:
        return json.load(r)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    site = sys.argv[1].rstrip("/")
    if len(sys.argv) > 2:
        key = urllib.parse.unquote(sys.argv[2].lstrip("#"))
    else:
        keys = get(site + "/api/drawings?keys=1")["keys"]
        if not keys:
            sys.exit("The gallery is empty - draw the label and erase first.")
        key = keys[0]["key"]
    d = get(site + "/api/drawings?key=" + urllib.parse.quote(key))
    raw = base64.b64decode(d["p"])
    pts = struct.unpack("<%dh" % (len(raw) // 2), raw)
    n = len(pts) // 2

    rows = []
    for i in range(0, len(pts), 16):
        rows.append("  " + ", ".join(str(v) for v in pts[i:i + 16]) + ",")
    OUT.write_text(
        "// The gallery's title card: drawing %s from %s (%s), %d points.\n"
        "// Made by tools/drawing_to_label.py - run it again to replace it.\n"
        "#pragma once\n#include <stdint.h>\n\n"
        "static const int16_t k_gallery_label[] = {\n%s\n};\n"
        "static const int k_gallery_label_n = %d;\n" % (key, site, d.get("t", ""), n, "\n".join(rows), n)
    )
    print("Wrote %s: %d points from the drawing of %s" % (OUT, n, d.get("t", key)))


if __name__ == "__main__":
    main()

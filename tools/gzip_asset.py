#!/usr/bin/env python3
"""Gzip one embedded web-UI asset at build time.

Invoked from main/CMakeLists.txt, once per HTML asset. The .gz output is
embedded alongside the plain copy; the HTTP handlers pick between them per
request from Accept-Encoding.

mtime is forced to 0 and the originating filename is left out of the gzip
header so identical input always produces a byte-identical .gz
(CONFIG_APP_REPRODUCIBLE_BUILD=y).
"""

import gzip
import os
import sys


def main():
    if len(sys.argv) != 3:
        sys.stderr.write("usage: gzip_asset.py <input> <output.gz>\n")
        return 2

    src = sys.argv[1]
    dst = sys.argv[2]

    out_dir = os.path.dirname(os.path.abspath(dst))
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)

    with open(src, "rb") as f:
        data = f.read()

    # Write to a temp file and rename, so an interrupted build cannot leave a
    # truncated .gz behind that a later incremental build would treat as
    # up to date (stale/corrupt gz is silent -- the browser just fails).
    tmp = dst + ".tmp"
    with open(tmp, "wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw,
                           compresslevel=9, mtime=0) as gz:
            gz.write(data)
    os.replace(tmp, dst)
    return 0


if __name__ == "__main__":
    sys.exit(main())

"""Refuses files that must never be committed. Usage: python -I tools/check_files.py --staged | --tracked

--staged checks what is about to be committed (the pre-commit hook in .githooks/), --tracked every tracked file (CI).
Blocked: anything under assets/, generated/, src/recomp/gen/ or build/; game, image and binary file types; files over
5 MB; and, when assets/default.xbe is present, any file containing that XBE's certificate keys, in hex or raw. The
keys themselves are never printed.
"""
import os
import re
import struct
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BLOCKED_DIRS = re.compile(r"^(assets|generated|src/recomp/gen|build)/", re.I)
BLOCKED_TYPES = re.compile(
    r"\.(iso|xiso|xbe|xpr|xwb|xmv|xsb|xvs|xap|u|ukx|uix|utx|usx|lin|spa|int|deu|fra|ita|ttf|bin"
    r"|bmp|png|jpe?g|gif|dds|tga|ppm|exe|dll|pdb|obj|lib)$", re.I)
MAX_BYTES = 5 * 1024 * 1024


def git(*args):
    return subprocess.run(["git", "-C", REPO, *args], check=True, capture_output=True).stdout


def cert_keys():
    """The LAN, signature and alternate signature keys from the local dump's certificate, or none."""
    path = os.path.join(REPO, "assets", "default.xbe")
    if not os.path.exists(path):
        return []
    data = open(path, "rb").read()
    base, cert = struct.unpack_from("<I", data, 0x104)[0], struct.unpack_from("<I", data, 0x118)[0]
    off = cert - base
    keys = [data[off + 176:off + 192], data[off + 192:off + 208]]
    # Alternate signature keys: 0xD0 is the documented place, and the toolkit's xbe_parser reads them at 0x1AC.
    for alt in (0xD0, 0x1AC):
        keys += [data[off + alt + 16 * i:off + alt + 16 * (i + 1)] for i in range(16)]
    found = []
    for k in keys:
        if len(k) == 16 and any(k):
            found += [k, k.hex().encode(), k.hex().upper().encode()]
    return found


def main(argv):
    mode = argv[1] if len(argv) > 1 else ""
    if mode not in ("--staged", "--tracked"):
        print(__doc__.strip())
        return 2
    # Index entries: "<mode> <sha> <stage>\t<path>". Submodules (mode 160000) are not files and are skipped.
    entries = {}
    for line in git("ls-files", "-s", "-z").split(b"\0"):
        if line:
            meta, path = line.split(b"\t", 1)
            entries[path.decode()] = meta.split()[0]
    if mode == "--staged":
        names = [n.decode() for n in git("diff", "--cached", "--name-only", "--diff-filter=ACMR", "-z").split(b"\0") if n]
    else:
        names = list(entries)
    keys = cert_keys()

    bad = []
    for name in names:
        if entries.get(name) == b"160000":
            continue
        if BLOCKED_DIRS.search(name):
            bad.append((name, "is in a folder of game data or generated code"))
            continue
        if BLOCKED_TYPES.search(name):
            bad.append((name, "is a game, image or binary file type"))
            continue
        content = git("cat-file", "blob", ":" + name)
        if len(content) > MAX_BYTES:
            bad.append((name, f"is {len(content) // (1024 * 1024)} MB, over the 5 MB limit"))
        elif any(k in content for k in keys):
            bad.append((name, "contains a certificate key from your default.xbe"))

    gh = os.environ.get("GITHUB_ACTIONS") == "true"
    for name, why in bad:
        print(f"::error file={name}::{name} {why}" if gh else f"blocked: {name} {why}")
    if bad:
        if not gh:
            print("Nothing was committed. Unstage these with: git restore --staged <file>")
        return 1
    print(f"checked {len(names)} files" + (", including certificate keys" if keys else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

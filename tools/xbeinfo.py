"""XBE parser. Usage: python -I tools/xbeinfo.py <default.xbe> [--json out.json]

Dumps header, certificate, sections, XDK library versions, TLS and kernel imports.
Importable: `from xbeinfo import Xbe`.
"""
import json
import struct
import sys

RETAIL_ENTRY_KEY = 0xA8FC57AB
DEBUG_ENTRY_KEY = 0x94859D4B
RETAIL_THUNK_KEY = 0x5B6D40B6
DEBUG_THUNK_KEY = 0xEFB1F152

SECTION_FLAGS = [(0x1, "WRITABLE"), (0x2, "PRELOAD"), (0x4, "EXECUTABLE"),
                 (0x8, "INSERTED_FILE"), (0x10, "HEAD_PAGE_RO"), (0x20, "TAIL_PAGE_RO")]


def _u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def _cstr(b, o):
    e = b.index(0, o)
    return b[o:e].decode("ascii", "replace")


class Xbe:
    def __init__(self, data: bytes):
        if data[:4] != b"XBEH":
            raise ValueError("not an XBE (missing XBEH magic)")
        self.data = data
        self.base = _u32(data, 0x104)
        self.header_size = _u32(data, 0x108)
        self.image_size = _u32(data, 0x10C)
        self.timestamp = _u32(data, 0x114)
        cert_addr = _u32(data, 0x118)
        n_sec = _u32(data, 0x11C)
        sec_addr = _u32(data, 0x120)
        self.init_flags = _u32(data, 0x124)
        raw_entry = _u32(data, 0x128)
        self.tls_addr = _u32(data, 0x12C)
        raw_thunk = _u32(data, 0x158)
        n_lib = _u32(data, 0x160)
        lib_addr = _u32(data, 0x164)

        # Entry/thunk are XOR-encoded; the key that lands inside the image is the right one.
        self.retail = True
        for ek, tk, retail in ((RETAIL_ENTRY_KEY, RETAIL_THUNK_KEY, True),
                               (DEBUG_ENTRY_KEY, DEBUG_THUNK_KEY, False)):
            e = raw_entry ^ ek
            if self.base <= e < self.base + self.image_size:
                self.entry, self.thunk, self.retail = e, raw_thunk ^ tk, retail
                break
        else:
            raise ValueError("entry point did not decode under retail or debug key")

        self.cert = self._parse_cert(cert_addr)
        self.sections = self._parse_sections(sec_addr, n_sec)
        self.libs = self._parse_libs(lib_addr, n_lib)
        self.tls = self._parse_tls()
        self.kernel_imports = self._parse_kernel_imports()

    def off(self, va):
        """File offset of a header-resident VA (headers are mapped at base)."""
        return va - self.base

    def va_to_file(self, va):
        for s in self.sections:
            if s["va"] <= va < s["va"] + s["vsize"]:
                return s["raw"] + (va - s["va"]) if va - s["va"] < s["rsize"] else None
        return None

    def _parse_cert(self, va):
        b, o = self.data, self.off(va)
        title = b[o + 0x0C:o + 0x0C + 80].decode("utf-16le", "replace").split("\0")[0]
        regions = _u32(b, o + 0xA0)
        return {"title_id": f"{_u32(b, o + 0x08):08X}", "title": title,
                "allowed_media": f"0x{_u32(b, o + 0x9C):08X}",
                "region_flags": f"0x{regions:X}", "version": _u32(b, o + 0xAC)}

    def _parse_sections(self, va, n):
        b, out = self.data, []
        for i in range(n):
            o = self.off(va) + i * 56
            flags, sva, vsz, raw, rsz, name_va = struct.unpack_from("<6I", b, o)
            out.append({"index": i, "name": _cstr(b, self.off(name_va)), "va": sva, "vsize": vsz,
                        "raw": raw, "rsize": rsz, "flags": flags,
                        "flag_names": [n_ for m, n_ in SECTION_FLAGS if flags & m]})
        return out

    def _parse_libs(self, va, n):
        b, out = self.data, []
        for i in range(n):
            o = self.off(va) + i * 16
            name = b[o:o + 8].rstrip(b"\0").decode("ascii", "replace")
            maj, mnr, build, fl = struct.unpack_from("<4H", b, o + 8)
            out.append({"name": name, "version": f"{maj}.{mnr}.{build}",
                        "qfe": fl & 0x1FFF, "approved": (fl >> 13) & 3,
                        "debug": bool(fl & 0x8000)})
        return out

    def _parse_tls(self):
        if not self.tls_addr:
            return None
        o = self.va_to_file(self.tls_addr)
        if o is None:
            return None
        d_start, d_end, idx, cb, zero, chars = struct.unpack_from("<6I", self.data, o)
        return {"data_start": d_start, "data_end": d_end, "index_addr": idx,
                "callbacks": cb, "zero_fill": zero, "characteristics": chars}

    def _parse_kernel_imports(self):
        f = self.va_to_file(self.thunk)
        if f is None:
            return []
        out = []
        while True:
            v = _u32(self.data, f + 4 * len(out))
            if v == 0:
                return out
            out.append(v & 0x7FFFFFFF)

    def to_dict(self):
        return {"base": self.base, "image_size": self.image_size, "entry": self.entry,
                "kernel_thunk": self.thunk, "retail": self.retail, "cert": self.cert,
                "sections": self.sections, "libs": self.libs, "tls": self.tls,
                "kernel_import_ordinals": self.kernel_imports}


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    x = Xbe(open(argv[1], "rb").read())
    print(f"Title      : {x.cert['title']} (ID {x.cert['title_id']}), "
          f"regions {x.cert['region_flags']}, {'retail' if x.retail else 'debug'} keys")
    print(f"Base       : 0x{x.base:08X}  image 0x{x.image_size:X}  headers 0x{x.header_size:X}")
    print(f"Entry      : 0x{x.entry:08X}   kernel thunk table: 0x{x.thunk:08X} "
          f"({len(x.kernel_imports)} imports)")
    if x.tls:
        print(f"TLS        : data 0x{x.tls['data_start']:08X}-0x{x.tls['data_end']:08X} "
              f"index 0x{x.tls['index_addr']:08X} zero_fill {x.tls['zero_fill']} "
              f"callbacks 0x{x.tls['callbacks']:08X}")
    print("\nSections:")
    for s in x.sections:
        print(f"  {s['index']:2d} {s['name']:<10} va=0x{s['va']:08X} vsize=0x{s['vsize']:07X} "
              f"raw=0x{s['raw']:07X} rsize=0x{s['rsize']:07X} {'|'.join(s['flag_names'])}")
    print("\nXDK libraries:")
    for l in x.libs:
        print(f"  {l['name']:<8} {l['version']}{'  (debug)' if l['debug'] else ''}")
    print("\nKernel import ordinals:", " ".join(str(o) for o in sorted(x.kernel_imports)))
    if "--json" in argv:
        with open(argv[argv.index("--json") + 1], "w") as f:
            json.dump(x.to_dict(), f, indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

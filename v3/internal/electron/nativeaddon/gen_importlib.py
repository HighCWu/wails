#!/usr/bin/env python3
# Generate a PE import library (.lib) in the Microsoft short-import archive
# format directly from a list of export names. Used because dlltool's
# def-file import libraries proved unreliable here (machine spellings vary
# across binutils builds; invalid ones silently degrade to 32-bit imports
# or produce archives ld cannot use). GNU ld natively links this format —
# it is what MSVC-style import libs contain.
#
#   gen_importlib.py <names.txt> <out.lib> <dll-name>
#
# Each symbol becomes an IMPORT_OBJECT_CODE entry with the plain name type;
# ld synthesizes the __imp_ pointers and jmp thunks at link time. The
# archive carries the first linker member ("/", big-endian armap) with
# real file offsets — without it ld fails with "Archive has no index".
import struct
import sys

def short_import(dll, sym):
    data = dll.encode() + b"\0" + sym.encode() + b"\0"
    hdr = struct.pack(
        "<HHHHIIHH",
        0x0000,          # Sig1
        0xFFFF,          # Sig2
        0,               # Version
        0x8664,          # Machine (x64)
        0,               # TimeDateStamp
        len(data),       # SizeOfData
        0,               # OrdinalHint
        0,               # Type:2=CODE, NameType:3=IMPORT_OBJECT_NAME
    )
    return hdr + data

def member_header(name, size):
    # the linker member is literally named "/" — no trailing-slash marker
    nm = "/" if name == "/" else name + "/"
    return nm.ljust(16) + \
        "0".ljust(12) + "0".ljust(6) + "0".ljust(6) + "0".ljust(8) + \
        str(size).ljust(10) + "`\n"

def member(name, blob):
    if len(blob) % 2:
        blob += b"\n"
    return member_header(name, len(blob)).encode() + blob

def main():
    names_path, out_path, dll = sys.argv[1], sys.argv[2], sys.argv[3]
    names = [l.strip() for l in open(names_path) if l.strip()]

    bodies = [short_import(dll, sym) for sym in names]

    # lay out: "!<arch>\n" + armap member + import members; compute member
    # file offsets for the armap first.
    armap_data_len = 4 + 4 * len(names) + sum(len(s) + 1 for s in names)
    armap_pad = b"\n" if armap_data_len % 2 else b""
    head = 8 + 60 + armap_data_len + len(armap_pad)
    offsets, off = [], head
    for b in bodies:
        offsets.append(off)
        off += 60 + len(b) + (len(b) % 2)

    strings = b"".join(s.encode() + b"\0" for s in names)
    armap = member("/", struct.pack(">I", len(names)) +
                   b"".join(struct.pack(">I", o) for o in offsets) + strings)

    # members must be written in the SAME order the armap offsets were
    # computed in
    ar = b"!<arch>\n" + armap
    for i, b in enumerate(bodies):
        ar += member("i%d.o" % i, b)
    open(out_path, "wb").write(ar)
    print("import library: %d imports from %s -> %s" % (len(names), dll, out_path))

main()

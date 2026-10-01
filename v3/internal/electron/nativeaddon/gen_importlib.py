#!/usr/bin/env python3
# Generate a PE import library (.lib) in the Microsoft short-import archive
# format directly from a list of export names. Used because dlltool's
# def-file import libraries proved unreliable here (machine spellings vary
# across binutils builds; invalid ones silently degrade to 32-bit imports
# or produce archives ld cannot use). GNU ld natively links this format —
# it is what MSVC-style import libs contain — and scans the members to
# build its own symbol index, so no linker members are needed.
#
#   gen_importlib.py <names.txt> <out.lib> <dll-name>
#
# Each symbol becomes an IMPORT_OBJECT_CODE entry with the plain name type;
# ld synthesizes the __imp_ pointers and jmp thunks at link time.
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

def member(name, blob):
    if len(blob) % 2:
        blob += b"\n"
    hdr = (name + "/").ljust(16) + \
        "0".ljust(12) + "0".ljust(6) + "0".ljust(6) + "0".ljust(8) + \
        str(len(blob)).ljust(10) + "`\n"
    return hdr.encode() + blob

def main():
    names_path, out_path, dll = sys.argv[1], sys.argv[2], sys.argv[3]
    names = [l.strip() for l in open(names_path) if l.strip()]
    ar = b"!<arch>\n"
    for i, sym in enumerate(names):
        ar += member("i%d.o" % i, short_import(dll, sym))
    open(out_path, "wb").write(ar)
    print("import library: %d imports from %s -> %s" % (len(names), dll, out_path))

main()

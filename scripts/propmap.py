"""Reflection properties of game classes (Darksiders2.exe PC): property name -> field offset.
Every property is a small object with its own vtable: slot 0x3C = init (pushes the name string),
slot 0x38 = copy (mov eax,[ebp+8]; mov eax/ecx..,[eax+OFF]; ... [ecx+OFF] - copies the field between two objects).
Usage: propmap.py [name-substring]   (all properties whose name contains the substring; no arg = all)"""
import struct, sys, re
import pefile

import sys as _sys, os as _os; _sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
from ds2paths import game_exe
EXE = game_exe()
pe = pefile.PE(EXE, fast_load=True)
B = pe.OPTIONAL_HEADER.ImageBase
IMG = pe.get_memory_mapped_image()
text = [s for s in pe.sections if s.Name.startswith(b".text")][0]
rdata = [s for s in pe.sections if s.Name.startswith(b".rdata")][0]
T0, T1 = B + text.VirtualAddress, B + text.VirtualAddress + text.Misc_VirtualSize
R0, R1 = B + rdata.VirtualAddress, B + rdata.VirtualAddress + rdata.Misc_VirtualSize


def u32(va):
    return struct.unpack_from("<I", IMG, va - B)[0]


def cstr(va):
    if not (R0 <= va < R1):
        return None
    e = IMG.find(b"\0", va - B)
    s = IMG[va - B:e]
    return s.decode("latin1") if 0 < len(s) < 80 and all(32 <= c < 127 for c in s) else None


def init_name(fn):
    """init: ...; push 0; push <name>; lea ecx,[ebp-0x14]; call ..."""
    code = IMG[fn - B:fn - B + 40]
    m = re.search(rb"\x6a\x00\x68(....)\x8d\x4d", code, re.S)
    return cstr(struct.unpack("<I", m.group(1))[0]) if m else None


def copy_off(fn):
    """copy: 55 8B EC 8B 45 08 <load [eax+off]> 8B 4D 0C <store [ecx+off]>; return (offset, size hint)"""
    code = IMG[fn - B:fn - B + 48]
    if not code.startswith(b"\x55\x8b\xec"):
        return None
    m = re.search(rb"\x8b\x45\x08(?:\x8b\x40(.)|\x8b\x80(....)|\x8a\x40(.)|\x8a\x80(....)|\xd9\x40(.)|\xd9\x80(....)|\x0f\xb6\x40(.)|\x8d\x50(.)|\x83\xc0(.)|\x05(....)|\x8d\x70(.)|\x8d\x48(.))", code, re.S)
    if not m:
        return None
    for g in m.groups():
        if g is not None:
            return g[0] if len(g) == 1 else struct.unpack("<I", g)[0]
    return None


if __name__ == "__main__":
    sub = sys.argv[1].lower() if len(sys.argv) > 1 else ""
    seen = set()
    for va in range(R0, R1 - 0x40, 4):
        init, cp = u32(va + 0x3C), u32(va + 0x38)
        if not (T0 <= init < T1 and T0 <= cp < T1):
            continue
        name = init_name(init)
        if not name or sub not in name.lower():
            continue
        off = copy_off(cp)
        key = (va, name)
        if key in seen:
            continue
        seen.add(key)
        print(f"vt {va:08X}  {name:40s} off={'?' if off is None else hex(off)}  (copy {cp:08X}, init {init:08X})")

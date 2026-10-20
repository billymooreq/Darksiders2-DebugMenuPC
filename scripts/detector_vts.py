"""Vtables of the PC exe that pass the trigger filter of task 07 (vt[0x40] == getRotation 0x00A24AFB and
vt[0x48] == getScale 0x00A24B3D, the SimpleShapeObject getters), with onAdd vt[0x50] / onRemove vt[0x54] and the
class name string that the game registers next to the vtable, when one is found.
  detector_vts.py            table to stdout (used for dumps/detector_vts_pc.txt and the class names in triggers.cpp)
"""
import re, struct
import pefile

import sys as _sys, os as _os; _sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
from ds2paths import game_exe
EXE = game_exe()
pe = pefile.PE(EXE, fast_load=True)
BASE = pe.OPTIONAL_HEADER.ImageBase
IMG = pe.get_memory_mapped_image()
GETROT, GETSCALE = 0x00A24AFB, 0x00A24B3D
DET_ADD, TRIG_ADD, DET_REM = 0x008B84FE, 0x008C8E47, 0x008B888B


def d(va):
    return struct.unpack_from("<I", IMG, va - BASE)[0]


def cstr(va):
    o = va - BASE
    if not (0 <= o < len(IMG)):
        return None
    e = IMG.find(b"\0", o, o + 80)
    s = IMG[o:e] if e > o else b""
    return s.decode() if re.fullmatch(rb"[A-Za-z_][A-Za-z0-9_]{2,60}", s) else None


rd = [s for s in pe.sections if s.Name.startswith(b".rdata")][0]
r0, r1 = BASE + rd.VirtualAddress, BASE + rd.VirtualAddress + rd.Misc_VirtualSize
rows = []
for va in range(r0, r1 - 0x60, 4):
    if d(va + 0x40) == GETROT and d(va + 0x48) == GETSCALE:
        # a vtable starts where the previous dword is not a code pointer (RTTI is absent): keep the first match only
        if rows and va - rows[-1][0] < 0x50:
            continue
        after = va  # the class name string often lies right after the vtable (like "StateCamera")
        while d(after) and BASE <= d(after) < BASE + len(IMG):
            after += 4
        tail = IMG[after - BASE:after - BASE + 64]
        m = re.match(rb"\0*([A-Za-z_][A-Za-z0-9_]{2,60})\0", tail)
        rows.append((va, d(va + 0x50), d(va + 0x54), m.group(1).decode() if m else ""))

print("vtable    onAdd     onRemove  kind          name-after-vtable")
for va, add, rem, name in rows:
    kind = "trigger" if add == TRIG_ADD else ("detector" if add == DET_ADD else "other")
    if rem == DET_REM:
        kind += "+rem"
    print("%08X  %08X  %08X  %-12s  %s" % (va, add, rem, kind, name))
print("total", len(rows))

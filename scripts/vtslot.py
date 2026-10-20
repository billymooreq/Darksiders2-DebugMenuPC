"""Which vtables hold a function and at which slot (PC Darksiders2.exe).
  vtslot.py <func VA> [...]     for each dword == func: vtable start (walk back over .text pointers), slot, RTTI-less
  vtslot.py --vt <vt VA> <slot hex>   print the function in that slot
A vtable start = the first of a run of .text pointers whose address is referenced by code (ctor writes it).
"""
import struct, sys
import pefile
EXE = "<game dir>/Darksiders2.exe"
pe = pefile.PE(EXE, fast_load=True)
B = pe.OPTIONAL_HEADER.ImageBase
IMG = pe.get_memory_mapped_image()
TX = [s for s in pe.sections if s.Name.startswith(b".text")][0]
T0, T1 = B + TX.VirtualAddress, B + TX.VirtualAddress + TX.Misc_VirtualSize
d = lambda a: struct.unpack_from("<I", IMG, a - B)[0]
TEXT = IMG[TX.VirtualAddress:TX.VirtualAddress + TX.Misc_VirtualSize]


def code_refs(va):
    return TEXT.count(struct.pack("<I", va))


def vt_start(a):
    s = a
    while T0 <= d(s - 4) < T1:
        s -= 4
        if code_refs(s):
            best = s
    # choose the nearest start (walking back) that code references
    s = a
    while True:
        if code_refs(s):
            return s
        if not (T0 <= d(s - 4) < T1):
            return None
        s -= 4


if sys.argv[1] == "--vt":
    vt, sl = int(sys.argv[2], 16), int(sys.argv[3], 16)
    print("%08X[%X] = %08X" % (vt, sl, d(vt + sl)))
    sys.exit()
for arg in sys.argv[1:]:
    f = int(arg, 16)
    pat = struct.pack("<I", f)
    o = 0
    print("== %08X" % f)
    while True:
        o = IMG.find(pat, o)
        if o < 0:
            break
        a = B + o
        o += 1
        if a & 3:
            continue
        st = vt_start(a)
        print("  %08X  vt %s slot %s" % (a, "%08X" % st if st else "?", "0x%X" % (a - st) if st else "?"))

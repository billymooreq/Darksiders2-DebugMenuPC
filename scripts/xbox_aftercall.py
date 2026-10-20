"""Xbox DEBUG image: calls of a function and what the next instructions read from r3 (getter result).
Usage: python -I xbox_aftercall.py <func VA> [disp hex filter] [n=6]"""
import sys, os, struct, bisect
import capstone
import sys as _sys, os as _os; _sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
from ds2paths import xbox_dir
W = xbox_dir()  # Xbox reference project, read-only
IMG = open(os.path.join(W, "dumps", "DarksidersDebug.pe"), "rb").read()
B = 0x82000000
md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
tgt = int(sys.argv[1], 16)
flt = sys.argv[2].lower() if len(sys.argv) > 2 and sys.argv[2] != "-" else None
n = int(sys.argv[3]) if len(sys.argv) > 3 else 6
FU = sorted((int(l.split()[0], 16), int(l.split()[1], 16)) for l in open(os.path.join(W, "dumps", "xref_DarksidersDebug_funcs.tsv")))
def func_of(a):
    i = bisect.bisect_right(FU, (a, 0xFFFFFFFF)) - 1
    return FU[i][0] if i >= 0 and FU[i][0] <= a < FU[i][1] else 0
cnt = 0
for o in range(0, len(IMG) - 4, 4):
    w = struct.unpack_from(">I", IMG, o)[0]
    if w >> 26 != 18 or (w & 3) != 1:
        continue
    d = w & 0x03FFFFFC
    if d & 0x02000000:
        d -= 0x04000000
    a = B + o
    if a + d != tgt:
        continue
    cnt += 1
    ins = list(md.disasm(IMG[o + 4:o + 4 + n * 4], a + 4))
    txt = " | ".join("%s %s" % (i.mnemonic, i.op_str) for i in ins)
    if flt and flt not in txt:
        continue
    print("%08X func %08X: %s" % (a, func_of(a), txt))
print("calls:", cnt)

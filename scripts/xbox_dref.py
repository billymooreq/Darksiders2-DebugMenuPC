"""Xbox DEBUG image: who reads/writes a global data address (lis rX,hi + D-form load/store/addi with lo).

Usage: python -I xbox_dref.py <VA> [<VA>...]
Prints the instruction address, the function (from the xref funcs table) and the instruction.
Looks for `lis rA, ha(VA)` up to 64 instructions before a D-form instruction (lwz/stw/lbz/stb/lhz/sth/lfs/stfs/
lfd/stfd/addi/lwzu/...) whose rA and displacement give VA. Cheap and approximate (no data flow), good enough.
"""
import sys, os, struct, bisect
import capstone
W = "<xbox project>"  # Xbox project, read-only
IMG = open(os.path.join(W, "dumps", "DarksidersDebug.pe"), "rb").read()
B = 0x82000000
FU = []
for l in open(os.path.join(W, "dumps", "xref_DarksidersDebug_funcs.tsv")):
    a, b = l.split()[:2]
    FU.append((int(a, 16), int(b, 16)))
FU.sort()
md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
DFORM = {14: "addi", 32: "lwz", 33: "lwzu", 34: "lbz", 35: "lbzu", 36: "stw", 37: "stwu", 38: "stb", 39: "stbu",
         40: "lhz", 42: "lha", 44: "sth", 48: "lfs", 50: "lfd", 52: "stfs", 54: "stfd", 58: "ld", 62: "std"}


def func_of(a):
    i = bisect.bisect_right(FU, (a, 0xFFFFFFFF)) - 1
    return FU[i][0] if i >= 0 and FU[i][0] <= a < FU[i][1] else None


def scan(va):
    lo = va & 0xFFFF
    ha = ((va >> 16) + (1 if lo & 0x8000 else 0)) & 0xFFFF
    slo = lo - 0x10000 if lo & 0x8000 else lo
    out = []
    n = len(IMG) // 4
    words = struct.unpack(">%dI" % n, IMG[:n * 4])
    for i, w in enumerate(words):
        op = w >> 26
        if op not in DFORM or (w & 0xFFFF) != lo:
            continue
        ra = (w >> 16) & 31
        if ra == 0:
            continue
        if op in (58, 62) and (w & 3):
            continue
        for j in range(i - 1, max(-1, i - 64), -1):
            p = words[j]
            if p >> 26 == 15 and ((p >> 16) & 31) == 0 and ((p >> 21) & 31) == ra:
                if (p & 0xFFFF) == ha:
                    out.append(B + i * 4)
                break
    return out


for arg in sys.argv[1:]:
    va = int(arg, 16)
    print("== %08X" % va)
    for a in scan(va):
        f = func_of(a)
        ins = next(md.disasm(IMG[a - B:a - B + 4], a), None)
        print("%08X  func %s  %s %s" % (a, "%08X" % f if f else "?", ins.mnemonic if ins else "?",
                                        ins.op_str if ins else ""))

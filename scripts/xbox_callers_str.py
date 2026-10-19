"""Xbox DEBUG: functions that call <target> (bl), with the strings each function references. Usage: <VA> [filter]"""
import sys, os, struct, bisect
W = "<xbox project>"
IMG = open(os.path.join(W, "dumps", "DarksidersDebug.pe"), "rb").read()
B = 0x82000000
FU = sorted((int(l.split()[0], 16), int(l.split()[1], 16)) for l in open(os.path.join(W, "dumps", "xref_DarksidersDebug_funcs.tsv")))
def func_of(a):
    i = bisect.bisect_right(FU, (a, 0xFFFFFFFF)) - 1
    return FU[i] if i >= 0 and FU[i][0] <= a < FU[i][1] else None
def cstr(a):
    o = a - B
    if not (0 <= o < len(IMG)): return None
    e = IMG.find(b"\0", o, o + 120)
    if e <= o: return None
    t = IMG[o:e]
    return t.decode("latin-1") if all(32 <= c < 127 for c in t) else None
tgt = int(sys.argv[1], 16); flt = sys.argv[2] if len(sys.argv) > 2 else None
fs = set()
for o in range(0, len(IMG) - 4, 4):
    w = struct.unpack_from(">I", IMG, o)[0]
    if w >> 26 == 18 and (w & 3) == 1:
        d = w & 0x03FFFFFC
        if d & 0x02000000: d -= 0x04000000
        if B + o + d == tgt:
            f = func_of(B + o)
            if f: fs.add(f)
for f in sorted(fs):
    strs = []
    regs = {}
    for a in range(f[0], f[1], 4):
        w = struct.unpack_from(">I", IMG, a - B)[0]
        op = w >> 26
        if op == 15 and ((w >> 16) & 31) == 0:
            regs[(w >> 21) & 31] = (w & 0xFFFF) << 16
        elif op == 14 and ((w >> 16) & 31) in regs:
            lo = w & 0xFFFF
            if lo & 0x8000: lo -= 0x10000
            s = cstr((regs[(w >> 16) & 31] + lo) & 0xFFFFFFFF)
            if s and s not in strs: strs.append(s)
    line = "%08X: %s" % (f[0], " | ".join(strs[:12]))
    if not flt or flt.lower() in line.lower(): print(line)

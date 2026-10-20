"""Quick PPC disassembler over the unpacked image (capstone), with string / lis+addi annotation.

Usage:
  python ppcdis.py f <addr>            disassemble whole function (bounds from xref funcs.tsv)
  python ppcdis.py r <addr> <count>    disassemble <count> instructions
  python ppcdis.py x <addr>            who references <addr> (lis/addi refs) and who calls it
  python ppcdis.py s <substring>       find strings (data) containing substring, with referencing funcs
"""
import sys, os, bisect, struct
import capstone
import sys as _sys, os as _os; _sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
from ds2paths import xbox_dir
W = xbox_dir()  # Xbox reference project, read-only
IMG = open(os.path.join(W, "dumps", "DarksidersDebug.pe"), "rb").read()
B = 0x82000000
XP = os.path.join(W, "dumps", "xref_DarksidersDebug")
md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
md.detail = False
_funcs = None


def funcs():
    global _funcs
    if _funcs is None:
        _funcs = []
        for l in open(XP + "_funcs.tsv"):
            a, b = l.split()[:2]
            _funcs.append((int(a, 16), int(b, 16)))
        _funcs.sort()
    return _funcs


def func_of(a):
    fs = funcs()
    i = bisect.bisect_right(fs, (a, 0xFFFFFFFF)) - 1
    if i >= 0 and fs[i][0] <= a < fs[i][1]:
        return fs[i]
    return None


def cstr(a):
    o = a - B
    if not (0 <= o < len(IMG)):
        return None
    e = IMG.find(b"\0", o)
    if e - o > 120 or e == o:
        return None
    t = IMG[o:e]
    return t.decode("latin-1") if all(32 <= c < 127 or c in (9, 10) for c in t) else None


def u32(a):
    return struct.unpack_from(">I", IMG, a - B)[0]


def dis(start, end):
    hi = {}
    out = []
    code = IMG[start - B:end - B]
    for i in range(0, len(code), 4):
        a = start + i
        w = code[i:i + 4]
        ins = list(md.disasm(w, a))
        if not ins:
            out.append(f"{a:08X}: .long 0x{w.hex()}")
            continue
        x = ins[0]
        note = ""
        ops = x.op_str.replace(" ", "").split(",")
        try:
            if x.mnemonic == "lis":
                hi[ops[0]] = int(ops[1], 0) << 16
            elif x.mnemonic in ("addi", "addic") and ops[1] in hi:
                v = (hi[ops[1]] + int(ops[2], 0)) & 0xFFFFFFFF
                note = f"  ; ={v:08X}"
                s = cstr(v)
                if s: note += f' "{s}"'
                hi[ops[0]] = v if ops[0] != ops[1] else v
            elif x.mnemonic == "ori" and ops[1] in hi:
                v = hi[ops[1]] | int(ops[2], 0)
                note = f"  ; ={v:08X}"
            elif x.mnemonic in ("lwz", "lfs", "stw", "stfs", "lbz", "stb", "lhz", "lfd", "ld", "std") and "(" in x.op_str:
                disp, reg = x.op_str.split(",")[-1].strip().rstrip(")").split("(")
                if reg in hi:
                    v = (hi[reg] + int(disp, 0)) & 0xFFFFFFFF
                    note = f"  ; [{v:08X}]"
                    s = cstr(v)
                    if s: note += f' "{s}"'
            elif x.mnemonic in ("bl", "b"):
                t = int(ops[0], 0)
                f = func_of(t)
                note = "" if f and f[0] == t else ""
        except Exception:
            pass
        out.append(f"{a:08X}: {w.hex()}  {x.mnemonic:8s} {x.op_str}{note}")
    return out


def xrefs(t):
    t = t.upper().zfill(8)
    for l in open(XP + "_refs.tsv"):
        a, i, f = l.split()
        if a == t:
            print("ref", i, "in", f)
    for l in open(XP + "_calls.tsv"):
        c = l.rstrip("\n").split("\t")
        if c[2] == t:
            print("call", c[0], "in", c[1], "args", " ".join(c[3:]))


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "f":
        a = int(sys.argv[2], 16)
        f = func_of(a)
        if not f:
            print("no func"); f = (a, a + 0x200)
        print(f"; func {f[0]:08X}-{f[1]:08X}")
        print("\n".join(dis(*f)))
    elif cmd == "r":
        a = int(sys.argv[2], 16); n = int(sys.argv[3])
        print("\n".join(dis(a, a + 4 * n)))
    elif cmd == "x":
        xrefs(sys.argv[2])
    elif cmd == "s":
        sub = sys.argv[2].encode()
        o = 0
        hits = []
        while True:
            o = IMG.find(sub, o)
            if o < 0: break
            s0 = IMG.rfind(b"\0", 0, o) + 1
            hits.append(s0); o += 1
        for s0 in sorted(set(hits)):
            a = B + s0
            if a >= 0x82380000 and a < 0x836E0000: continue
            print(f"{a:08X} {cstr(a)!r}")
            for l in open(XP + "_refs.tsv"):
                t, i, f = l.split()
                if int(t, 16) == a:
                    print("    ref", i, "in", f)

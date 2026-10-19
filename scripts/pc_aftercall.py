"""PC exe: every direct call of a function and the next N instructions, filtered by a substring.
Usage: python -I pc_aftercall.py <func VA> [substring|-] [N=14]"""
import sys, struct, os
sys.argv[0] = os.path.join(os.path.dirname(os.path.abspath(__file__)), "pcfind.py")
import importlib.util
spec = importlib.util.spec_from_file_location("pcfind_mod", sys.argv[0])
EXE = "<game dir>/Darksiders2.exe"
import pefile, capstone
pe = pefile.PE(EXE, fast_load=True); B = pe.OPTIONAL_HEADER.ImageBase; IMG = pe.get_memory_mapped_image()
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
T = [s for s in pe.sections if s.Name.startswith(b".text")][0]
t0, t1 = T.VirtualAddress, T.VirtualAddress + T.Misc_VirtualSize
tgt = int(sys.argv[1], 16)
flt = sys.argv[2] if len(sys.argv) > 2 and sys.argv[2] != "-" else None
n = int(sys.argv[3]) if len(sys.argv) > 3 else 14
cnt = 0
o = t0
while True:
    o = IMG.find(b"\xe8", o, t1)
    if o < 0:
        break
    rel = struct.unpack_from("<i", IMG, o + 1)[0]
    if B + o + 5 + rel == tgt:
        cnt += 1
        ins = []
        for i in md.disasm(IMG[o + 5:o + 5 + n * 8], B + o + 5):
            ins.append("%s %s" % (i.mnemonic, i.op_str))
            if len(ins) >= n:
                break
        txt = " | ".join(ins)
        if not flt or flt in txt:
            print("%08X: %s" % (B + o, txt))
    o += 1
print("calls:", cnt)

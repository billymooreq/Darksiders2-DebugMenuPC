"""PC exe: places where a field load [r + OFF1] is followed (within N instructions) by an access [that reg + OFF2].
Usage: python -I pc_fieldpair.py <OFF1 hex> <OFF2 hex> [N=4]   (finds e.g. actor+0x330 -> stats+0x8C)"""
import sys, struct, re
import pefile, capstone
from capstone import x86
import sys as _sys, os as _os; _sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
from ds2paths import game_exe
EXE = game_exe()
pe = pefile.PE(EXE, fast_load=True); B = pe.OPTIONAL_HEADER.ImageBase; IMG = pe.get_memory_mapped_image()
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
T = [s for s in pe.sections if s.Name.startswith(b".text")][0]
t0, t1 = T.VirtualAddress, T.VirtualAddress + T.Misc_VirtualSize
o1, o2 = int(sys.argv[1], 16), int(sys.argv[2], 16)
N = int(sys.argv[3]) if len(sys.argv) > 3 else 4
d1 = struct.pack("<I", o1)
hits = 0
o = t0
# 8B modrm(10 reg r/m) disp32 : mov r32, [r/m + disp32]
while True:
    o = IMG.find(d1, o, t1)
    if o < 0:
        break
    for back in (2, 3):
        st = o - back
        ins = list(md.disasm(IMG[st:st + 80], B + st))
        if not ins or ins[0].address + ins[0].size != B + o + 4 or ins[0].mnemonic != "mov":
            continue
        m = re.match(r"(e[a-z]{2}), dword ptr \[e[a-z]{2} \+ 0x%x\]" % o1, ins[0].op_str)
        if not m:
            continue
        r = m.group(1)
        for i in ins[1:1 + N]:
            if re.search(r"\[%s \+ 0x%x\]" % (r, o2), i.op_str):
                print("%08X: %s %s  ->  %08X: %s %s" % (ins[0].address, ins[0].mnemonic, ins[0].op_str, i.address,
                                                       i.mnemonic, i.op_str))
                hits += 1
                break
            if re.match(r"%s," % r, i.op_str) and i.mnemonic in ("mov", "lea", "pop", "xor"):
                break
        break
    o += 1
print("hits:", hits)

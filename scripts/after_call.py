"""Rank functions called right after a given call target (e.g. after Darksiders::instance()).
Usage: after_call.py <target VA> [window_insns=4]"""
import struct, sys, collections
import capstone, pefile
import sys as _sys, os as _os; _sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
from ds2paths import game_exe
EXE = game_exe()
pe = pefile.PE(EXE, fast_load=True); B = pe.OPTIONAL_HEADER.ImageBase; IMG = pe.get_memory_mapped_image()
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
T = [s for s in pe.sections if s.Name.startswith(b".text")][0]
t0, t1 = T.VirtualAddress, T.VirtualAddress + T.Misc_VirtualSize
tgt = int(sys.argv[1], 16); win = int(sys.argv[2]) if len(sys.argv) > 2 else 4
cnt = collections.Counter(); ex = {}
for i in range(t0, t1 - 5):
    if IMG[i] == 0xE8 and (B + i + 5 + struct.unpack_from("<i", IMG, i + 1)[0]) & 0xFFFFFFFF == tgt:
        n = 0
        for ins in md.disasm(IMG[i + 5:i + 5 + 40], B + i + 5):
            if ins.mnemonic == "call":
                cnt[ins.op_str] += 1; ex.setdefault(ins.op_str, hex(B + i)); break
            n += 1
            if n >= win: break
for k, v in cnt.most_common(25):
    print(f"{v:5d}  {k}   e.g. {ex[k]}")

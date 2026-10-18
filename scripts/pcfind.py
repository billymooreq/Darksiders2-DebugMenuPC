"""Quick x86 search over Darksiders2.exe without Ghidra.
  pcfind.py float <value>        addresses of a float32 constant (exact bits) in .rdata/.data
  pcfind.py ref <VA hex>         .text places that contain VA as imm32/disp32, with the instruction
  pcfind.py str <text>           VA of NUL-terminated ASCII string, then its refs
  pcfind.py dis <VA hex> [n]     disassemble n instructions from VA
  pcfind.py dword <VA hex> [n]   dump n dwords
"""
import struct, sys
import capstone, pefile

EXE = "<game dir>/Darksiders2.exe"
pe = pefile.PE(EXE, fast_load=True)
BASE = pe.OPTIONAL_HEADER.ImageBase
IMG = pe.get_memory_mapped_image()
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.detail = True
TEXT = [s for s in pe.sections if s.Name.startswith(b".text")][0]
T0, T1 = BASE + TEXT.VirtualAddress, BASE + TEXT.VirtualAddress + TEXT.Misc_VirtualSize


def sect(va):
    for s in pe.sections:
        if s.VirtualAddress <= va - BASE < s.VirtualAddress + s.Misc_VirtualSize:
            return s.Name.rstrip(bytes(1)).decode()
    return "?"


def insn_at(va):
    """Instruction whose disp32/imm32 field starts exactly at va (try back-aligning, shortest exact match first)."""
    cands = []
    for back in range(1, 11):
        st = va - back
        for ins in md.disasm(IMG[st - BASE:st - BASE + 16], st):
            off = va - ins.address
            if ins.address + ins.size >= va + 4 and off in (ins.disp_offset, ins.imm_offset) and off:
                cands.append(ins)
            break
    if len(cands) <= 1:
        return cands[0] if cands else None
    # vote: x86 decoding self-synchronises, so linear decodes started earlier land on the true boundary
    starts = {c.address: 0 for c in cands}
    for s0 in range(va - 48, va - 10):
        for ins in md.disasm(IMG[s0 - BASE:va - BASE + 12], s0):
            if ins.address in starts:
                starts[ins.address] += 1
                break
            if ins.address > va:
                break
    best = max(cands, key=lambda c: starts[c.address])
    return best


def refs(target):
    pat = struct.pack("<I", target)
    out = []
    i = IMG.find(pat, T0 - BASE)
    while 0 <= i < T1 - BASE:
        ins = insn_at(BASE + i)
        out.append((BASE + i, ins))
        i = IMG.find(pat, i + 1)
    return out


def print_refs(target):
    for va, ins in refs(target):
        s = f"{ins.address:08X}: {ins.mnemonic} {ins.op_str}" if ins else f"{va:08X}: ?"
        print("   ", s)


cmd = sys.argv[1]
if cmd == "float":
    pat = struct.pack("<f", float(sys.argv[2]))
    i = IMG.find(pat)
    while i >= 0:
        va = BASE + i
        if sect(va) in (".rdata", ".data"):
            n = len(refs(va))
            print(f"{va:08X} {sect(va)} refs={n}")
        i = IMG.find(pat, i + 1)
elif cmd == "ref":
    print_refs(int(sys.argv[2], 16))
elif cmd == "str":
    pat = sys.argv[2].encode() + b"\0"
    i = IMG.find(pat)
    while i >= 0:
        if IMG[i - 1] == 0:
            print(f"{BASE + i:08X} {sect(BASE + i)}")
            print_refs(BASE + i)
        i = IMG.find(pat, i + 1)
elif cmd == "dis":
    va, n = int(sys.argv[2], 16), int(sys.argv[3]) if len(sys.argv) > 3 else 40
    for k, ins in enumerate(md.disasm(IMG[va - BASE:va - BASE + n * 15], va)):
        if k >= n:
            break
        print(f"{ins.address:08X}: {ins.bytes.hex():20s} {ins.mnemonic} {ins.op_str}")
elif cmd == "dword":
    va, n = int(sys.argv[2], 16), int(sys.argv[3]) if len(sys.argv) > 3 else 16
    for k in range(n):
        v = struct.unpack_from("<I", IMG, va - BASE + 4 * k)[0]
        print(f"{va + 4 * k:08X} +{4 * k:03X}: {v:08X}")
elif cmd == "calls":
    # direct E8 rel32 calls / E9 jmps to target
    tgt = int(sys.argv[2], 16)
    t = IMG[T0 - BASE:T1 - BASE]
    for i in range(len(t) - 5):
        if t[i] in (0xE8, 0xE9):
            va = T0 + i
            if (va + 5 + struct.unpack_from("<i", t, i + 1)[0]) & 0xFFFFFFFF == tgt:
                print(f"    {va:08X}: {'call' if t[i] == 0xE8 else 'jmp '} {tgt:08X}")

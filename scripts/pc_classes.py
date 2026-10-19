"""PC exe: the game's reflection class registry (gfc ClassManager 0x00405861, register 0x0048A5C8, setParent
0x0048A7BE). For every `call 0x48a5c8` near a pair of string pushes: class name, parent name, the global that keeps
the class descriptor. Writes dumps/classes_pc.txt. Usage: python -I pc_classes.py [name filter]"""
import sys, struct, os
import pefile, capstone
EXE = "<game dir>/Darksiders2.exe"
pe = pefile.PE(EXE, fast_load=True); B = pe.OPTIONAL_HEADER.ImageBase; IMG = pe.get_memory_mapped_image()
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
T = [s for s in pe.sections if s.Name.startswith(b".text")][0]
t0, t1 = T.VirtualAddress, T.VirtualAddress + T.Misc_VirtualSize
REG = 0x0048A5C8

def cstr(va):
    o = va - B
    if not (0 <= o < len(IMG)):
        return None
    e = IMG.find(b"\0", o, o + 100)
    if e <= o:
        return None
    s = IMG[o:e]
    return s.decode("latin-1") if all(32 <= c < 127 for c in s) else None

rows = []
o = t0
while True:
    o = IMG.find(b"\xe8", o, t1)
    if o < 0:
        break
    rel = struct.unpack_from("<i", IMG, o + 1)[0]
    if B + o + 5 + rel != REG:
        o += 1
        continue
    # strings pushed in the 0x140 bytes before; global stored right after
    names = []
    p = max(t0, o - 0x180)
    for i in md.disasm(IMG[p:o], B + p):
        if i.mnemonic == "push" and i.op_str.startswith("0x"):
            s = cstr(int(i.op_str, 16))
            if s:
                names.append(s)
    glob = None
    for i in md.disasm(IMG[o + 5:o + 40], B + o + 5):
        if i.mnemonic == "mov" and i.op_str.startswith("dword ptr [0x") and i.op_str.endswith("eax"):
            glob = int(i.op_str.split("[")[1].split("]")[0], 16)
            break
        if i.mnemonic in ("call", "ret"):
            break
    own = names[-2] if len(names) >= 2 else (names[-1] if names else "?")
    par = names[-1] if len(names) >= 2 else "?"
    rows.append((own, par, glob, B + o))
    o += 1
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "dumps", "classes_pc.txt")
with open(out, "w") as f:
    f.write("# class\tparent\tglobal(class descriptor)\tregister call  (scripts/pc_classes.py)\n")
    for r in sorted(rows):
        f.write("%s\t%s\t%s\t%08X\n" % (r[0], r[1], "%08X" % r[2] if r[2] else "-", r[3]))
flt = sys.argv[1] if len(sys.argv) > 1 else None
for r in sorted(rows):
    if not flt or flt.lower() in (r[0] + " " + r[1]).lower():
        print("%-36s %-28s %s  @%08X" % (r[0], r[1], "%08X" % r[2] if r[2] else "-", r[3]))
print(len(rows), "registrations")

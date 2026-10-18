"""MSVC x86 RTTI: class-name substring -> TypeDescriptor -> COL -> vtable, plus code refs to the vtable.
Usage: rtti_vt.py <substring of .?AV name>"""
import struct, sys, subprocess, pefile
EXE = "<game dir>/Darksiders2.exe"
pe = pefile.PE(EXE, fast_load=True); B = pe.OPTIONAL_HEADER.ImageBase; IMG = pe.get_memory_mapped_image()
def finds(pat):
    i = IMG.find(pat)
    while i >= 0:
        yield B + i
        i = IMG.find(pat, i + 1)
sub = sys.argv[1].encode()
for nm in finds(sub):
    s = IMG.rfind(b".?A", 0, nm - B + 1)
    name = IMG[s:IMG.find(b"\0", s)].decode()
    td = B + s - 8
    for ref in finds(struct.pack("<I", td)):
        col = ref - 12
        if struct.unpack_from("<I", IMG, col - B)[0] != 0:  # signature 0
            continue
        for vref in finds(struct.pack("<I", col)):
            vt = vref + 4
            print(f"{name}\n  TD {td:08X} COL {col:08X} vtable {vt:08X}")
            out = subprocess.run([sys.executable, "-I", __file__.replace("rtti_vt.py", "pcfind.py"), "ref", f"{vt:08X}"],
                                 capture_output=True, text=True).stdout
            print(out.rstrip())
    break

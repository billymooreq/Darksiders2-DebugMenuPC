"""Dump ASCII and UTF-16LE strings with VA. Usage: strings_dump.py <pe> <out_prefix> [minlen=4]
Writes <out_prefix>_ascii.txt and <out_prefix>_utf16.txt: lines 'VA<TAB>section<TAB>string'."""
import re, sys, pefile

path, pref = sys.argv[1], sys.argv[2]
minlen = int(sys.argv[3]) if len(sys.argv) > 3 else 4
pe = pefile.PE(path, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
data = open(path, "rb").read()

def va_of(off):
    for s in pe.sections:
        if s.PointerToRawData <= off < s.PointerToRawData + s.SizeOfRawData:
            return base + s.VirtualAddress + off - s.PointerToRawData, s.Name.rstrip(bytes(1)).decode()
    return None, "-"

ra = re.compile(rb"[\x20-\x7e\t]{%d,}" % minlen)
ru = re.compile(rb"(?:[\x20-\x7e\t]\x00){%d,}" % minlen)
with open(pref + "_ascii.txt", "w", encoding="utf-8") as f:
    for m in ra.finditer(data):
        va, sec = va_of(m.start())
        f.write(f"{va or 0:08X}\t{sec}\t{m.group().decode('ascii')}\n")
with open(pref + "_utf16.txt", "w", encoding="utf-8") as f:
    for m in ru.finditer(data):
        va, sec = va_of(m.start())
        f.write(f"{va or 0:08X}\t{sec}\t{m.group().decode('utf-16le')}\n")

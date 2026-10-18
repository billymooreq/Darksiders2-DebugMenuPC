"""PE summary: sections+entropy, imports, TLS, exports, overlay. Usage: pe_info.py <file.exe> <out.txt>"""
import math, sys, pefile

def entropy(b):
    if not b:
        return 0.0
    c = [0] * 256
    for x in b:
        c[x] += 1
    n = len(b)
    return -sum(v / n * math.log2(v / n) for v in c if v)

path, out = sys.argv[1], sys.argv[2]
pe = pefile.PE(path)
L = []
p = L.append
fh, oh = pe.FILE_HEADER, pe.OPTIONAL_HEADER
p(f"file: {path}")
p(f"Machine 0x{fh.Machine:04X}  TimeDateStamp 0x{fh.TimeDateStamp:08X}  NumberOfSections {fh.NumberOfSections}  Characteristics 0x{fh.Characteristics:04X}")
p(f"ImageBase 0x{oh.ImageBase:08X}  EntryPoint RVA 0x{oh.AddressOfEntryPoint:08X} (VA 0x{oh.ImageBase + oh.AddressOfEntryPoint:08X})  SizeOfImage 0x{oh.SizeOfImage:X}")
p(f"Linker {oh.MajorLinkerVersion}.{oh.MinorLinkerVersion}  OS {oh.MajorOperatingSystemVersion}.{oh.MinorOperatingSystemVersion}  Subsystem {oh.Subsystem}  DllCharacteristics 0x{oh.DllCharacteristics:04X}  Checksum 0x{oh.CheckSum:08X}")
p("")
p("sections: name, VA, vsize, rawoff, rawsize, chars, entropy")
for s in pe.sections:
    d = s.get_data()
    p(f"  {s.Name.rstrip(bytes(1)).decode(errors='replace'):8s} 0x{oh.ImageBase + s.VirtualAddress:08X} 0x{s.Misc_VirtualSize:08X} 0x{s.PointerToRawData:08X} 0x{s.SizeOfRawData:08X} 0x{s.Characteristics:08X} {entropy(d):.3f}")
ep = oh.AddressOfEntryPoint
sec = pe.get_section_by_rva(ep)
p(f"entry point in section: {sec.Name.rstrip(bytes(1)).decode() if sec else '?'}")
p(f"overlay offset: {pe.get_overlay_data_start_offset()}")
p("")
p("data directories:")
for i, d in enumerate(oh.DATA_DIRECTORY):
    if d.VirtualAddress:
        p(f"  {d.name:40s} RVA 0x{d.VirtualAddress:08X} size 0x{d.Size:X}")
p("")
if hasattr(pe, "DIRECTORY_ENTRY_TLS"):
    t = pe.DIRECTORY_ENTRY_TLS.struct
    p(f"TLS: callbacks @0x{t.AddressOfCallBacks:08X}")
    try:
        cbs = []
        va = t.AddressOfCallBacks
        while True:
            v = pe.get_dword_at_rva(va - oh.ImageBase)
            if not v:
                break
            cbs.append(v)
            va += 4
        p("  callbacks: " + ", ".join(f"0x{v:08X}" for v in cbs))
    except Exception as e:
        p(f"  callbacks read error {e}")
else:
    p("TLS: none")
p("")
if hasattr(pe, "DIRECTORY_ENTRY_EXPORT"):
    p("exports:")
    for e in pe.DIRECTORY_ENTRY_EXPORT.symbols:
        p(f"  {e.ordinal} {e.name} 0x{oh.ImageBase + e.address:08X}")
p("imports:")
for imp in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
    p(f"  [{imp.dll.decode()}] ({len(imp.imports)})")
    for i in imp.imports:
        nm = i.name.decode() if i.name else f"ord#{i.ordinal}"
        p(f"      0x{i.address:08X} {nm}")
if hasattr(pe, "DIRECTORY_ENTRY_DELAY_IMPORT"):
    p("delay imports:")
    for imp in pe.DIRECTORY_ENTRY_DELAY_IMPORT:
        p(f"  [{imp.dll.decode()}] ({len(imp.imports)})")
        for i in imp.imports:
            nm = i.name.decode() if i.name else f"ord#{i.ordinal}"
            p(f"      0x{i.address:08X} {nm}")
open(out, "w", encoding="utf-8").write("\n".join(L) + "\n")
print("\n".join(l for l in L if not l.startswith("      ")))

"""Xbox DEBUG image: where a dword value occurs (vtable slots, pointers). Usage: python -I xbox_data.py <VA>..."""
import sys, os, struct
W = "<xbox project>"
IMG = open(os.path.join(W, "dumps", "DarksidersDebug.pe"), "rb").read()
B = 0x82000000
for arg in sys.argv[1:]:
    v = struct.pack(">I", int(arg, 16))
    hits, o = [], IMG.find(v)
    while o >= 0 and len(hits) < 50:
        if o % 4 == 0:
            hits.append(B + o)
        o = IMG.find(v, o + 1)
    print("== %s: %s" % (arg, " ".join("%08X" % h for h in hits)))

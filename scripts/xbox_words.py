"""Read big-endian u32 words from the Xbox unpacked image. Usage: xbox_words.py <addr hex> <count>"""
import struct, sys
import sys as _sys, os as _os; _sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
from ds2paths import xbox_dir
IMG = open(_os.path.join(xbox_dir(), "dumps", "DarksidersDebug.pe"), "rb").read()
B = 0x82000000
a, n = int(sys.argv[1], 16), int(sys.argv[2])
for i in range(n):
    v = struct.unpack_from(">I", IMG, a - B + 4 * i)[0]
    print(f"{a + 4 * i:08X} +{4 * i:03X}: {v:08X}")

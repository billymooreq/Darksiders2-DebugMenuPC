"""Xbox DEBUG image: C strings at addresses. Usage: python -I xbox_str.py <VA>..."""
import sys, os
IMG = open("<xbox project>/dumps/DarksidersDebug.pe", "rb").read()
B = 0x82000000
for a in sys.argv[1:]:
    o = int(a, 16) - B
    e = IMG.find(b"\0", o)
    print("%s: %r" % (a, IMG[o:min(e, o + 200)].decode("latin-1")))

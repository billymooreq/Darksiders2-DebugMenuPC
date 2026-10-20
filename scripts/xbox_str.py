"""Xbox DEBUG image: C strings at addresses. Usage: python -I xbox_str.py <VA>..."""
import sys, os
import sys as _sys, os as _os; _sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
from ds2paths import xbox_dir
IMG = open(_os.path.join(xbox_dir(), "dumps", "DarksidersDebug.pe"), "rb").read()
B = 0x82000000
for a in sys.argv[1:]:
    o = int(a, 16) - B
    e = IMG.find(b"\0", o)
    print("%s: %r" % (a, IMG[o:min(e, o + 200)].decode("latin-1")))

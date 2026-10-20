"""Reflection property vtables (task 13): type id (vt[+4]), element type (vt[+0x10]), field offset (copy vt[+0x68]),
array count offset (vt[+0x34]). Property vtable start = slot such that +0x6C = init (pushes the name).
Usage: proptypes.py [name-substring] | proptypes.py --types   (histogram of type ids with example names)"""
import struct, sys, re, collections
sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])
from propmap import IMG, B, T0, T1, R0, R1, u32, init_name, copy_off


def const_ret(fn):
    c = IMG[fn - B:fn - B + 6]
    if c[:1] == b"\x6a" and c[2:4] == b"\x58\xc3": return c[1]
    if c[:3] == b"\x33\xc0\xc3": return 0
    if c[:1] == b"\xb0" and c[2:3] == b"\xc3": return c[1]
    if c[:1] == b"\xb8" and c[5:6] == b"\xc3": return struct.unpack_from("<I", c, 1)[0]
    return None


def count_off(fn):
    c = IMG[fn - B:fn - B + 16]
    m = re.match(rb"\x55\x8b\xec\x8b\x45\x08\x8b(?:\x40(.)|\x80(....))\x5d\xc2\x04\x00", c, re.S)
    if not m: return None
    g = m.group(1) or m.group(2)
    return g[0] if len(g) == 1 else struct.unpack("<I", g)[0]


def props():
    out = []
    for va in range(R0, R1 - 0x70, 4):
        init, cp = u32(va + 0x6C), u32(va + 0x68)
        if not (T0 <= init < T1 and T0 <= cp < T1): continue
        t = u32(va + 4)
        if not (T0 <= t < T1): continue
        name = init_name(init)
        if not name: continue
        out.append(dict(vt=va, name=name, type=const_ret(t), elem=const_ret(u32(va + 0x10)) if T0 <= u32(va + 0x10) < T1 else None,
                        off=copy_off(cp), cnt=count_off(u32(va + 0x34)) if T0 <= u32(va + 0x34) < T1 else None))
    return out


if __name__ == "__main__":
    ps = props()
    if len(sys.argv) > 1 and sys.argv[1] == "--types":
        h = collections.defaultdict(list)
        for p in ps: h[(p["type"], p["elem"])].append(p["name"])
        for k in sorted(h, key=lambda k: (str(k[0]), str(k[1]))):
            v = h[k]
            print(k, len(v), sorted(set(v))[:25])
    else:
        sub = sys.argv[1].lower() if len(sys.argv) > 1 else ""
        for p in ps:
            if sub in p["name"].lower():
                o = "?" if p["off"] is None else hex(p["off"])
                c = "" if p["cnt"] is None else f" count@{p['cnt']:#x}"
                print(f"vt {p['vt']:08X}  {p['name']:36s} type={p['type']} elem={p['elem']} off={o}{c}")

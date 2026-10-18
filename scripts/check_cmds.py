"""Check which Xbox console command names/descriptions exist in PC strings.
Usage: check_cmds.py <console_commands.md> <strings_ascii.txt> <out.md>"""
import sys
md, st, out = sys.argv[1:4]
strs = [l.rstrip("\n").split("\t", 2) for l in open(st, encoding="utf-8")]
exact = {}
for va, sec, s in strs:
    exact.setdefault(s, []).append((va, sec))
blob = "\n".join(s for _, _, s in strs)
rows = []
for l in open(md, encoding="utf-8"):
    if not l.startswith("| ") or "`" not in l:
        continue
    c = [x.strip() for x in l.strip().strip("|").split("|")]
    name = c[1].split("`")[1]
    rows.append((c[0], name, c[3], c[4]))
def look(s):
    if not s:
        return "-"
    if s in exact:
        return "exact " + ",".join(f"{va}({sec})" for va, sec in exact[s][:2])
    if s in blob:
        return "substr"
    return ""
L = ["| cat | name | name in PC | display | display in PC | description | descr in PC |", "|---|---|---|---|---|---|---|"]
n_name = n_desc = 0
for cat, name, disp, desc in rows:
    a, b, d = look(name), look(disp), look(desc)
    n_name += bool(a and a != "-"); n_desc += bool(d and d != "-")
    L.append(f"| {cat} | `{name}` | {a} | {disp} | {b} | {desc} | {d} |")
hdr = f"commands: {len(rows)}; name found (exact/substr): {n_name}; description found: {n_desc}\n\n"
open(out, "w", encoding="utf-8").write(hdr + "\n".join(L) + "\n")
print(hdr)
for r in L[2:]:
    if "exact" in r or "substr" in r:
        print(r)

"""Keep personal data out of the public repo.

  privacy.py check          scan the files staged for commit (index) for private strings -> exit 1 if found
  privacy.py check --all    scan every tracked file and every commit message / blob of the whole history
  privacy.py memory         copy ../CLAUDE.md (the agent's working memory, outside the repo) to CLAUDE.md.copy
                            with the private strings replaced, then check it

Private strings come from <repo>/local_private_patterns.txt (not in git): lines "literal==>replacement"
(the same format as git filter-repo --replace-text). The Windows user folder name is always checked too.
"""
import os, subprocess, sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def rules():
    r, p = [], os.path.join(REPO, "local_private_patterns.txt")
    if os.path.isfile(p):
        for line in open(p, encoding="utf-8"):
            line = line.rstrip("\r\n")
            if line and not line.startswith("#") and "==>" in line:
                a, b = line.split("==>", 1)
                r.append((a, b))
    home = os.path.basename(os.environ.get("USERPROFILE", "") or os.path.expanduser("~"))
    if home and len(home) >= 4:
        r.append((home, "user"))
    return r


def bad_words():
    words = {a for a, _ in rules() if not a.startswith(" ")}
    # report only the "leaf" strings: a long literal that contains a shorter one is covered by it
    return sorted(w for w in words if not any(o != w and o in w for o in words))


def git(*a, inp=None):
    return subprocess.run(["git", *a], cwd=REPO, capture_output=True, input=inp).stdout


def scan(name, data, words, hits):
    t = data.decode("utf-8", "replace").lower()
    for w in words:
        if w.lower() in t:
            hits.append("%s: %r" % (name, w))


def check(all_history):
    words, hits = bad_words(), []
    if all_history:
        for line in git("rev-list", "--all", "--objects").decode().splitlines():
            parts = line.split(" ", 1)
            if git("cat-file", "-t", parts[0]).strip() == b"blob":
                scan(parts[1] if len(parts) > 1 else parts[0], git("cat-file", "-p", parts[0]), words, hits)
        scan("commit messages", git("log", "--all", "--format=%B"), words, hits)
        scan("tag messages", git("for-each-ref", "refs/tags", "--format=%(contents)"), words, hits)
    else:
        for line in git("diff", "--cached", "--name-only", "--diff-filter=ACMR").decode().splitlines():
            scan(line, git("show", ":" + line), words, hits)
    for h in sorted(set(hits)):
        print("PRIVATE:", h)
    print("privacy check: %d hit(s), %d pattern(s)" % (len(set(hits)), len(words)))
    return 1 if hits else 0


def memory():
    src = os.path.join(os.path.dirname(REPO), "CLAUDE.md")
    s = open(src, encoding="utf-8").read()
    for a, b in sorted(rules(), key=lambda x: -len(x[0])):
        s = s.replace(a, b)
    dst = os.path.join(REPO, "CLAUDE.md.copy")
    open(dst, "w", encoding="utf-8", newline="").write(s)
    left = [w for w in bad_words() if w.lower() in s.lower()]
    print("CLAUDE.md.copy written;", "private strings left: %s" % left if left else "clean")
    return 1 if left else 0


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "check"
    sys.exit(memory() if cmd == "memory" else check("--all" in sys.argv))

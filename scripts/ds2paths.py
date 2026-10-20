"""Paths for the analysis scripts: no absolute paths in the repo.

Lookup order for each setting: environment variable -> <repo>/local_paths.ini (KEY=VALUE, not in git) -> default.
  DS2_GAME_DIR  folder with the PC Darksiders2.exe (default: <repo>/../Darksiders II)
  DS2_GAME_EXE  full path to Darksiders2.exe (overrides DS2_GAME_DIR)
  DS2_XBOX_DIR  Xbox reference project (folder with dumps/DarksidersDebug.pe and dumps/xref_DarksidersDebug*); no default
"""
import os, sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _local():
    d, p = {}, os.path.join(REPO, "local_paths.ini")
    if os.path.isfile(p):
        for line in open(p, encoding="utf-8"):
            line = line.strip()
            if line and not line.startswith(("#", ";")) and "=" in line:
                k, v = line.split("=", 1)
                d[k.strip()] = v.strip().strip('"')
    return d


def setting(name, default=None):
    v = os.environ.get(name) or _local().get(name) or default
    return os.path.normpath(v) if v else None


def _fail(msg):
    sys.stderr.write("ds2paths: " + msg + "\n")
    sys.exit(2)


def game_exe():
    exe = setting("DS2_GAME_EXE")
    if not exe:
        exe = os.path.join(setting("DS2_GAME_DIR", os.path.join(REPO, "..", "Darksiders II")), "Darksiders2.exe")
    if not os.path.isfile(exe):
        _fail("Darksiders2.exe not found at %s\n  set DS2_GAME_DIR (or DS2_GAME_EXE), e.g. in <repo>/local_paths.ini" % exe)
    return exe


def xbox_dir():
    d = setting("DS2_XBOX_DIR")
    if not d or not os.path.isfile(os.path.join(d, "dumps", "DarksidersDebug.pe")):
        _fail("Xbox reference dumps not found (%s)\n  set DS2_XBOX_DIR to the folder that has dumps/DarksidersDebug.pe" % d)
    return d

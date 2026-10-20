"""Pack the release zip: build/DS2-DebugMenu-v<ver>.zip with dinput8.dll (no pdb), INSTALL.txt, LICENSE,
THIRD_PARTY_LICENSES.txt. Build the DLL first (src/dbgmenu/build.bat). Usage: make_release.py <version, e.g. 0.13>"""
import hashlib, os, sys, zipfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ver = sys.argv[1]
dll = os.path.join(REPO, "build", "dinput8.dll")
data = open(dll, "rb").read()


def text(p):
    return open(os.path.join(REPO, p), encoding="utf-8-sig").read().replace("\r\n", "\n").replace("\n", "\r\n")


tp = ("Third-party code used in dinput8.dll\r\n====================================\r\n\r\n"
      "Dear ImGui 1.91.9 - https://github.com/ocornut/imgui\r\n----------------------------------------------------\r\n"
      + text("src/third_party/imgui/LICENSE.txt") +
      "\r\n\r\nMinHook 1.3.4 (includes Hacker Disassembler Engine 32) - https://github.com/TsudaKageyu/minhook\r\n"
      "----------------------------------------------------------------------------------------\r\n"
      + text("src/third_party/minhook/LICENSE.txt"))
out = os.path.join(REPO, "build", "DS2-DebugMenu-v%s.zip" % ver)
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("dinput8.dll", data)
    z.writestr("INSTALL.txt", text("release/INSTALL.txt"))
    z.writestr("LICENSE", text("LICENSE"))
    z.writestr("THIRD_PARTY_LICENSES.txt", tp)
print(out)
print("dinput8.dll SHA-1", hashlib.sha1(data).hexdigest(), len(data), "bytes")
print("zip SHA-1", hashlib.sha1(open(out, "rb").read()).hexdigest())

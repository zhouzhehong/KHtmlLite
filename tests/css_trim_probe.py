# css_trim_probe.py — runs the C++ unit test for simplifyUnsupportedCss().
#
# The C++ test (src/host/css_trim_probe.cpp) checks the four requested cases
# plus data:-URI/quoted-value semicolons, and asserts brace balance after
# removal.  This wrapper makes it part of the regression suite.
#
# Usage: python tests\css_trim_probe.py
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build", "host", "probe_css_trim.exe")


def main():
    if not os.path.exists(EXE):
        print(f"FAIL: {EXE} missing (build with BUILD_PROBE_TOOLS=ON)")
        print("CSS TRIM PROBE FAIL")
        return 1
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\mingw64\bin;" + env["PATH"]
    r = subprocess.run([EXE], capture_output=True, text=True, env=env)
    out = (r.stdout or "") + (r.stderr or "")
    print(out.rstrip())
    ok = (r.returncode == 0 and "CSS TRIM PROBE PASS" in out)
    print("CSS TRIM PROBE " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

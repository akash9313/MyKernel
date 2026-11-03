"""
AstraOS Phase AB: Bookmark Storage & Persistence Host Verification Runner
Compiles tests/test_bookmark_storage.c into a host DLL and runs it via ctypes.
"""

import os
import sys
import subprocess
import ctypes

BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def find_host_cc():
    candidates = [
        r'C:\mingw64\bin\gcc.exe',
        os.path.join(BASE_DIR, 'tools', 'llvm-toolchain', 'bin', 'clang.exe'),
        r'C:\Program Files\LLVM\bin\clang.exe'
    ]
    for c in candidates:
        if os.path.exists(c):
            try:
                res = subprocess.run([c, '--version'], capture_output=True, text=True)
                if res.returncode == 0:
                    return c
            except Exception:
                pass
    return candidates[0]

CC = find_host_cc()
BUILD_DIR = os.path.join(BASE_DIR, 'build')
DLL_PATH = os.path.join(BUILD_DIR, 'test_bookmark_storage.dll')
SRC_PATH = os.path.join(BASE_DIR, 'tests', 'test_bookmark_storage.c')

def main():
    os.makedirs(BUILD_DIR, exist_ok=True)
    if not os.path.exists(CC):
        print(f"[HOST TEST] ERROR: Host C compiler not found at {CC}")
        return 1

    print(f"[HOST TEST] Compiling bookmark storage test runner using {CC}...")
    cmd = [
        CC,
        '-Wall', '-Wextra', '-O2',
        '-shared',
        '-DHOST_DLL',
        '-I', os.path.join(BASE_DIR, 'kernel', 'include'),
        SRC_PATH,
        '-o', DLL_PATH
    ]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"[HOST TEST] Build FAILED:")
        print(res.stderr)
        return 1

    print(f"[HOST TEST] Built {DLL_PATH} successfully.")
    print(f"[HOST TEST] Running bookmark storage tests via ctypes...")
    try:
        lib = ctypes.CDLL(DLL_PATH)
        lib.run_all_bookmark_tests.restype = ctypes.c_int
        ret = lib.run_all_bookmark_tests()
        if ret != 0:
            print(f"[HOST TEST] Bookmark storage tests FAILED with code {ret}")
            return ret
    except Exception as e:
        print(f"[ERROR] Executing test DLL failed: {e}")
        return 1

    print("[PASS] All AstraOS Bookmark Storage & Persistence Host Tests Succeeded!")
    return 0

if __name__ == '__main__':
    sys.exit(main())



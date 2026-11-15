"""
AstraOS Phase AB: Bookmark Storage & Persistence Host Verification Runner
Compiles tests/test_bookmark_storage.c into a host DLL / standalone executable and runs it.
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
                res = subprocess.run([c, '--version'], capture_output=True, text=True, timeout=5)
                if res.returncode == 0:
                    return c
            except Exception:
                pass
    return candidates[0]

CC = find_host_cc()
BUILD_DIR = os.path.join(BASE_DIR, 'build')
DLL_PATH = os.path.join(BUILD_DIR, 'test_bookmark_storage.dll')
EXE_PATH = os.path.join(BUILD_DIR, 'test_bookmark_storage.exe')
SRC_PATH = os.path.join(BASE_DIR, 'tests', 'test_bookmark_storage.c')

def run_tests_exe():
    print(f"[HOST TEST] Compiling bookmark storage test executable using {CC}...")
    os.makedirs(BUILD_DIR, exist_ok=True)
    cmd = [
        CC,
        '-fno-lto',
        '-Wall', '-Wextra', '-O2',
        '-I', os.path.join(BASE_DIR, 'kernel', 'include'),
        SRC_PATH,
        '-o', EXE_PATH
    ]
    res = subprocess.run(cmd, capture_output=True, text=True, timeout=15)
    if res.returncode != 0:
        print("[ERROR] Compilation failed:")
        print(res.stderr)
        return 1
    print(f"[HOST TEST] Running {EXE_PATH}...")
    try:
        proc = subprocess.run([EXE_PATH], capture_output=True, text=True, timeout=15)
        print(proc.stdout)
        if proc.returncode != 0:
            print(f"[FAIL] Executable exited with code {proc.returncode}:\n{proc.stderr}")
            return 1
        return 0
    except OSError as e:
        if "4551" in str(e):
            print(f"[BLOCKED] Binary execution blocked by host OS security policy ({e})")
            return 2
        print(f"[FAIL] Failed to execute {EXE_PATH}: {e}")
        return 1

def build_test_dll():
    print(f"[HOST TEST] Compiling bookmark storage test runner using {CC}...")
    os.makedirs(BUILD_DIR, exist_ok=True)
    cmd = [
        CC,
        '-fno-lto',
        '-Wall', '-Wextra', '-O2',
        '-shared',
        '-DHOST_DLL',
        '-I', os.path.join(BASE_DIR, 'kernel', 'include'),
        SRC_PATH,
        '-o', DLL_PATH
    ]
    res = subprocess.run(cmd, capture_output=True, text=True, timeout=15)
    if res.returncode != 0:
        print(f"[HOST TEST] Build FAILED:\n{res.stderr}")
        return False
    print(f"[HOST TEST] Built {DLL_PATH} successfully.")
    return True

def main():
    if not os.path.exists(CC):
        print(f"[HOST TEST] ERROR: Host C compiler not found at {CC}")
        return 1

    if not build_test_dll():
        return run_tests_exe()

    print(f"[HOST TEST] Running bookmark storage tests via ctypes...")
    try:
        lib = ctypes.CDLL(DLL_PATH)
        lib.run_all_bookmark_tests.restype = ctypes.c_int
        ret = lib.run_all_bookmark_tests()
        if ret != 0:
            print(f"[HOST TEST] Bookmark storage tests FAILED with code {ret}")
            return ret
        print("[PASS] All AstraOS Bookmark Storage & Persistence Host Tests Succeeded!")
        return 0
    except Exception as e:
        if "4551" in str(e):
            print(f"[NOTE] Loading DLL directly blocked by host policy ({e}); trying standalone test binary.")
            return run_tests_exe()
        print(f"[ERROR] Executing test DLL failed: {e}")
        return 1

if __name__ == '__main__':
    sys.exit(main())

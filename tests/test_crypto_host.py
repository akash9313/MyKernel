"""
AstraOS Cryptographic Host Verification Suite
Compiles tests/test_crypto_host.c into a host DLL and runs it.
"""

import os
import sys
import subprocess
import ctypes

BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
def find_host_cc():
    candidates = [
        os.path.join(BASE_DIR, 'tools', 'llvm-toolchain', 'bin', 'clang.exe'),
        r'C:\mingw64\bin\gcc.exe',
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
DLL_PATH = os.path.join(BUILD_DIR, 'test_crypto.dll')
EXE_PATH = os.path.join(BUILD_DIR, 'test_crypto.exe')
TEST_SRC = os.path.join(BASE_DIR, 'tests', 'test_crypto_host.c')

def run_tests_exe():
    print(f"[HOST TEST] Compiling Cryptographic test executable using {CC}...")
    os.makedirs(BUILD_DIR, exist_ok=True)
    cmd = [
        CC,
        '-O2',
        '-DHOST_TEST',
        '-I', BASE_DIR,
        '-I', os.path.join(BASE_DIR, 'kernel', 'include'),
        '-I', os.path.join(BASE_DIR, 'kernel'),
        '-I', os.path.join(BASE_DIR, 'third_party', 'limine'),
        TEST_SRC,
        '-o', EXE_PATH
    ]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"[FAIL] Compilation failed with code {res.returncode}:")
        print(res.stderr)
        return False
    print(f"[HOST TEST] Running {EXE_PATH}...")
    res = subprocess.run([EXE_PATH])
    return res.returncode == 0

def build_test_dll():
    print(f"[HOST TEST] Compiling Cryptographic host test runner using {CC}...")
    os.makedirs(BUILD_DIR, exist_ok=True)
    cmd = [
        CC,
        '-O2',
        '-shared',
        '-DHOST_TEST',
        '-DHOST_DLL',
        '-I', BASE_DIR,
        '-I', os.path.join(BASE_DIR, 'kernel', 'include'),
        '-I', os.path.join(BASE_DIR, 'kernel'),
        '-I', os.path.join(BASE_DIR, 'third_party', 'limine'),
        TEST_SRC,
        '-o', DLL_PATH
    ]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"[FAIL] Compilation failed with code {res.returncode}:")
        print(res.stderr)
        return False
    print(f"[HOST TEST] test_crypto.dll built successfully.")
    return True

def run_tests():
    if not build_test_dll():
        return run_tests_exe()

    print(f"[HOST TEST] Loading {DLL_PATH}...")
    try:
        dll = ctypes.CDLL(DLL_PATH)
        run_fn = dll.run_crypto_tests
        run_fn.restype = ctypes.c_int
        code = run_fn()
        if code == 0:
            print("[HOST TEST] Cryptographic verification suite PASSED.")
            return True
        else:
            print(f"[HOST TEST] Cryptographic verification suite FAILED with code {code}.")
            return False
    except Exception as e:
        print(f"[NOTE] Loading DLL directly blocked by host policy ({e}); falling back to standalone test binary.")
        return run_tests_exe()

if __name__ == '__main__':
    if not run_tests():
        sys.exit(1)
    sys.exit(0)

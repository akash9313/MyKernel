"""
AstraOS Phase AD: Block Device Subsystem Host Verification Runner
Compiles tests/test_block_host.c into a host DLL and runs it via ctypes.
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
DLL_PATH = os.path.join(BUILD_DIR, 'test_block.dll')
SRC_PATH = os.path.join(BASE_DIR, 'tests', 'test_block_host.c')

def build_test_dll():
    print(f"[HOST TEST] Compiling block device host test runner using {CC}...")
    os.makedirs(BUILD_DIR, exist_ok=True)
    cmd = [
        CC,
        '-O2',
        '-shared',
        '-DHOST_TEST',
        '-I', BASE_DIR,
        '-I', os.path.join(BASE_DIR, 'kernel', 'include'),
        '-I', os.path.join(BASE_DIR, 'kernel'),
        '-I', os.path.join(BASE_DIR, 'third_party', 'limine'),
        SRC_PATH,
        '-o', DLL_PATH
    ]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"[FAIL] Compilation failed with code {res.returncode}:")
        print(res.stderr)
        return False
    print(f"[HOST TEST] test_block.dll built successfully.")
    return True

def run_tests():
    if not build_test_dll():
        return False

    try:
        block_lib = ctypes.CDLL(DLL_PATH)
        res = block_lib.run_all_block_host_tests()
        if res == 0:
            print("\n[PASS] All AstraOS Block Device Host Tests Succeeded!\n")
            return True
        else:
            print(f"\n[FAIL] Block device host tests failed with code {res}\n")
            return False
    except Exception as e:
        print(f"[ERROR] Exception running block host tests: {e}")
        return False

if __name__ == '__main__':
    ok = run_tests()
    sys.exit(0 if ok else 1)


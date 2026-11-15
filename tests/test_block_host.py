"""
AstraOS Phase AD: Block Device Subsystem Host Verification Runner
Compiles tests/test_block_host.c into a host DLL / standalone executable and runs it.
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
DLL_PATH = os.path.join(BUILD_DIR, 'test_block.dll')
EXE_PATH = os.path.join(BUILD_DIR, 'test_block.exe')
SRC_PATH = os.path.join(BASE_DIR, 'tests', 'test_block_host.c')

def run_tests_exe():
    print(f"[HOST TEST] Compiling Block Device host test executable using {CC}...")
    os.makedirs(BUILD_DIR, exist_ok=True)
    cmd = [
        CC,
        '-fno-lto',
        '-O2',
        '-DHOST_TEST',
        '-I', BASE_DIR,
        '-I', os.path.join(BASE_DIR, 'kernel', 'include'),
        '-I', os.path.join(BASE_DIR, 'kernel'),
        '-I', os.path.join(BASE_DIR, 'third_party', 'limine'),
        SRC_PATH,
        '-o', EXE_PATH
    ]
    res = subprocess.run(cmd, capture_output=True, text=True, timeout=15)
    if res.returncode != 0:
        print(f"[FAIL] Compilation failed with code {res.returncode}:\n{res.stderr}")
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
    print(f"[HOST TEST] Compiling block device host test runner using {CC}...")
    os.makedirs(BUILD_DIR, exist_ok=True)
    cmd = [
        CC,
        '-fno-lto',
        '-O2',
        '-shared',
        '-DHOST_TEST',
        '-DHOST_DLL',
        '-I', BASE_DIR,
        '-I', os.path.join(BASE_DIR, 'kernel', 'include'),
        '-I', os.path.join(BASE_DIR, 'kernel'),
        '-I', os.path.join(BASE_DIR, 'third_party', 'limine'),
        SRC_PATH,
        '-o', DLL_PATH
    ]
    res = subprocess.run(cmd, capture_output=True, text=True, timeout=15)
    if res.returncode != 0:
        print(f"[FAIL] Compilation failed with code {res.returncode}:\n{res.stderr}")
        return False
    print(f"[HOST TEST] test_block.dll built successfully.")
    return True

def run_tests():
    if not build_test_dll():
        return run_tests_exe()

    try:
        block_lib = ctypes.CDLL(DLL_PATH)
        res = block_lib.run_all_block_host_tests()
        if res == 0:
            print("\n[PASS] All AstraOS Block Device Host Tests Succeeded!\n")
            return 0
        else:
            print(f"\n[FAIL] Block device host tests failed with code {res}\n")
            return 1
    except Exception as e:
        if "4551" in str(e):
            print(f"[NOTE] Loading DLL directly blocked by host policy ({e}); trying standalone test binary.")
            return run_tests_exe()
        print(f"[ERROR] Exception running block host tests: {e}")
        return 1

if __name__ == '__main__':
    code = run_tests()
    sys.exit(code)

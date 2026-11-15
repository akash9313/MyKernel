"""
AstraOS VFS & Initramfs Host Verification Suite
Compiles tests/test_vfs_host.c into a host DLL and runs it.
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
DLL_PATH = os.path.join(BUILD_DIR, 'test_vfs.dll')
EXE_PATH = os.path.join(BUILD_DIR, 'test_vfs.exe')
TEST_SRC = os.path.join(BASE_DIR, 'tests', 'test_vfs_host.c')

def run_tests_exe():
    print(f"[HOST TEST] Compiling VFS test executable using {CC}...")
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
        TEST_SRC,
        '-o', EXE_PATH
    ]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"[FAIL] Compilation failed with code {res.returncode}:")
        print(res.stderr)
        return False
    print(f"[HOST TEST] Running {EXE_PATH}...")
    try:
        res = subprocess.run([EXE_PATH], capture_output=True, text=True, timeout=15)
        print(res.stdout)
        if res.returncode != 0:
            print(f"[FAIL] Executable exited with code {res.returncode}:\n{res.stderr}")
            return 1
        return 0
    except OSError as e:
        if "4551" in str(e):
            print(f"[BLOCKED] Binary execution blocked by host OS security policy ({e})")
            return 2
        print(f"[FAIL] Failed to execute {EXE_PATH}: {e}")
        return 1

def build_test_dll():
    print(f"[HOST TEST] Compiling VFS & Initramfs host test runner using {CC}...")
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
        TEST_SRC,
        '-o', DLL_PATH
    ]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"[FAIL] Compilation failed with code {res.returncode}:")
        print(res.stderr)
        return False
    print(f"[HOST TEST] test_vfs.dll built successfully.")
    return True

def run_tests():
    if not os.path.exists(DLL_PATH):
        if not build_test_dll():
            return run_tests_exe()
    else:
        if not build_test_dll():
            return run_tests_exe()

    try:
        vfs_lib = ctypes.CDLL(DLL_PATH)
        res = vfs_lib.run_all_vfs_host_tests()
        if res == 0:
            print("\n[PASS] All AstraOS VFS & Initramfs Host Tests Succeeded!\n")
            return 0
        else:
            print(f"\n[FAIL] VFS host tests failed with return code {res}\n")
            return 1
    except Exception as e:
        if "4551" in str(e):
            print(f"[NOTE] Loading DLL directly blocked by host policy ({e}); trying standalone test binary.")
        else:
            print(f"[NOTE] Loading DLL failed ({e}); trying standalone test binary.")
        return run_tests_exe()

if __name__ == '__main__':
    code = run_tests()
    sys.exit(code)

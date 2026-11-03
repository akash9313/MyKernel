"""
AstraOS Phase AK: ELF64 Loader Security Host Verification Runner
Compiles tests/test_elf_host.c into a host DLL and runs it via ctypes.
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
DLL_PATH = os.path.join(BUILD_DIR, 'test_elf.dll')
SRC_PATH = os.path.join(BASE_DIR, 'tests', 'test_elf_host.c')

def build_test_dll():
    print(f"[HOST TEST] Compiling ELF host test runner using {CC}...")
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
        SRC_PATH,
        '-o', DLL_PATH
    ]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        print("[ERROR] Compilation failed:")
        print(res.stderr)
        return False
    print(f"[HOST TEST] Successfully generated {DLL_PATH}")
    return True

EXE_PATH = os.path.join(BUILD_DIR, 'test_elf.exe')

def run_tests_exe():
    print(f"[HOST TEST] Compiling ELF host test executable using {CC}...")
    os.makedirs(BUILD_DIR, exist_ok=True)
    cmd = [
        CC,
        '-O2',
        '-DHOST_TEST',
        '-I', BASE_DIR,
        '-I', os.path.join(BASE_DIR, 'kernel', 'include'),
        '-I', os.path.join(BASE_DIR, 'kernel'),
        '-I', os.path.join(BASE_DIR, 'third_party', 'limine'),
        SRC_PATH,
        '-o', EXE_PATH
    ]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        print("[ERROR] Compilation failed:")
        print(res.stderr)
        return False
    print(f"[HOST TEST] Running {EXE_PATH}...")
    res = subprocess.run([EXE_PATH])
    return res.returncode == 0

def run_tests():
    if not build_test_dll():
        return run_tests_exe()
    print(f"[HOST TEST] Running {DLL_PATH} via ctypes...")
    try:
        lib = ctypes.CDLL(DLL_PATH)
        lib.run_all_elf_host_tests.restype = ctypes.c_int
        res = lib.run_all_elf_host_tests()
        if res == 0:
            print("\n[HOST TEST] ALL ELF64 LOADER SECURITY TESTS PASSED.\n")
            return True
        else:
            print(f"\n[HOST TEST] FAILED with code {res}\n")
            return False
    except Exception as e:
        print(f"[NOTE] Loading DLL directly blocked by host policy ({e}); falling back to standalone test binary.")
        return run_tests_exe()

if __name__ == '__main__':
    ok = run_tests()
    sys.exit(0 if ok else 1)


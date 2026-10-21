"""
AstraOS Kernel Heap Allocator Host-Side Verification Suite
Compiles the production allocator implementation (kernel/mm/heap.c) and
the comprehensive C host test suite (tests/test_heap_host.c) into a native DLL,
then invokes it to verify all host-side heap requirements:
- Zero-size and NULL behavior
- Alignment and arithmetic overflow
- Exact fit and 80-byte minimum split boundaries
- Coalescing in both directions (left, right, bridge)
- kcalloc zeroing
- krealloc shrink, grow, move, and failure preservation
- Invalid, interior, and double-free detection via panic traps
- 10,000-iteration seeded randomized stress test with independent tracking
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
DLL_PATH = os.path.join(BUILD_DIR, 'test_heap.dll')
TEST_SRC = os.path.join(BASE_DIR, 'tests', 'test_heap_host.c')
HEAP_SRC = os.path.join(BASE_DIR, 'kernel', 'mm', 'heap.c')

def build_test_dll():
    print(f"[HOST TEST] Compiling production kernel heap with host test runner using {CC}...")
    os.makedirs(BUILD_DIR, exist_ok=True)
    cmd = [
        CC,
        '-O2',
        '-shared',
        '-I', BASE_DIR,
        '-I', os.path.join(BASE_DIR, 'kernel', 'include'),
        '-I', os.path.join(BASE_DIR, 'kernel'),
        '-I', os.path.join(BASE_DIR, 'third_party', 'limine'),
        TEST_SRC,
        HEAP_SRC,
        '-o', DLL_PATH
    ]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"[FAIL] Compilation failed with code {res.returncode}:")
        print(res.stderr)
        return False
    print(f"[HOST TEST] test_heap.dll built successfully.")
    return True

def run_tests():
    if not os.path.exists(DLL_PATH):
        if not build_test_dll():
            return False

    try:
        heap_lib = ctypes.CDLL(DLL_PATH)
        res = heap_lib.run_all_heap_tests()
        if res == 0:
            print("\n[PASS] All AstraOS Kernel Heap Host Tests Succeeded!\n")
            return True
        else:
            print(f"\n[FAIL] Heap host tests returned non-zero code {res}\n")
            return False
    except Exception as e:
        print(f"[FAIL] Error executing host heap test via ctypes: {e}")
        return False

if __name__ == '__main__':
    if not build_test_dll():
        sys.exit(1)
    if not run_tests():
        sys.exit(1)
    sys.exit(0)

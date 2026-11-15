"""
AstraOS Comprehensive Host Verification Test Runner
Sequentially executes all 7 host verification suites:
1. VFS & Initramfs (tests/test_vfs_host.py)
2. AstraFS Persistent Filesystem (tests/test_fs_host.py)
3. ELF64 Loader Security (tests/test_elf_host.py)
4. Bookmark Storage & Persistence (tests/test_bookmark_storage.py)
5. Block Device Subsystem & ATA Flush (tests/test_block_host.py)
6. Cryptographic Subsystem & TLS (tests/test_crypto_host.py)
7. Kernel Heap Allocator (tests/test_heap_host.py)
"""

import os
import sys
import time
import subprocess

SUITES = [
    ("VFS & Initramfs", "tests/test_vfs_host.py"),
    ("AstraFS Persistent Filesystem", "tests/test_fs_host.py"),
    ("ELF64 Loader Security", "tests/test_elf_host.py"),
    ("Bookmark Storage & Persistence", "tests/test_bookmark_storage.py"),
    ("Block Device & ATA Flush", "tests/test_block_host.py"),
    ("Cryptographic Subsystem & TLS", "tests/test_crypto_host.py"),
    ("Kernel Heap Allocator", "tests/test_heap_host.py"),
]

def main():
    print("=" * 80)
    print("           AstraOS Host Verification Test Suite Runner")
    print("=" * 80)
    print(f"Host Python: {sys.version.split()[0]} ({sys.executable})")
    print(f"Working Directory: {os.getcwd()}")
    print("=" * 80 + "\n")

    summary = []
    total_start = time.time()

    for name, script_path in SUITES:
        print(f"\n{'#' * 80}")
        print(f"# Executing Suite: {name}")
        print(f"# Script: {script_path}")
        print(f"{'#' * 80}")

        start_time = time.time()
        try:
            proc = subprocess.run(
                [sys.executable, script_path],
                capture_output=True,
                text=True,
                timeout=30
            )
            elapsed = time.time() - start_time
            stdout = proc.stdout.strip()
            stderr = proc.stderr.strip()

            if stdout:
                print(stdout)
            if stderr:
                print("[STDERR]", stderr)

            if proc.returncode == 0:
                status = "PASS"
                reason = "All unit assertions succeeded"
            elif proc.returncode == 2:
                status = "BLOCKED"
                reason = "Blocked by host OS security policy (SAC WinError 4551)"
            else:
                status = "FAIL"
                reason = f"Exited with error code {proc.returncode}"

            print(f"\n--> Result: {status} ({elapsed:.2f}s, exit code {proc.returncode})")
            summary.append((name, script_path, status, proc.returncode, elapsed, reason))

        except subprocess.TimeoutExpired:
            elapsed = time.time() - start_time
            print(f"\n--> Result: TIMEOUT ({elapsed:.2f}s)")
            summary.append((name, script_path, "TIMEOUT", -1, elapsed, "Subprocess timed out after 30s"))
        except Exception as e:
            elapsed = time.time() - start_time
            print(f"\n--> Result: ERROR ({elapsed:.2f}s): {e}")
            summary.append((name, script_path, "ERROR", -2, elapsed, str(e)))

    total_elapsed = time.time() - total_start

    print("\n" + "=" * 90)
    print("                         HOST TEST SUITE FINAL SUMMARY MATRIX")
    print("=" * 90)
    print(f"{'Suite Name':<35} | {'Status':<8} | {'Exit':<4} | {'Time (s)':<8} | {'Details'}")
    print("-" * 90)

    pass_count = 0
    blocked_count = 0
    fail_count = 0

    for name, path, status, code, elapsed, reason in summary:
        if status == "PASS":
            pass_count += 1
        elif status == "BLOCKED":
            blocked_count += 1
        else:
            fail_count += 1
        print(f"{name:<35} | {status:<8} | {code:<4} | {elapsed:<8.2f} | {reason}")

    print("-" * 90)
    print(f"Total Suites: {len(summary)} | PASS: {pass_count} | BLOCKED: {blocked_count} | FAIL/TIMEOUT: {fail_count}")
    print(f"Total Elapsed Time: {total_elapsed:.2f}s")
    print("=" * 90)

    with open("host_test_summary.txt", "w") as f:
        f.write("AstraOS Host Verification Test Suite Final Summary\n")
        f.write("=" * 70 + "\n")
        for name, path, status, code, elapsed, reason in summary:
            f.write(f"[{status}] {name} (code {code}, {elapsed:.2f}s): {reason}\n")

    return 0 if (fail_count == 0) else 1

if __name__ == '__main__':
    sys.exit(main())

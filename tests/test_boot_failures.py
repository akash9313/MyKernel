#!/usr/bin/env python3
"""
test_boot_failures.py - Automated Boot Failure & Recovery Qualification Suite for Phase AY
Tests the key boot-time failure and recovery scenarios:
1. Missing kernel image (Limine fails safely)
2. Corrupted kernel image (Limine rejects invalid ELF safely)
3. Invalid boot configuration (Limine handles malformed config safely)
4. Missing persistent storage (Kernel detects missing hd1 and falls back to volatile storage)
5. Deliberate CPU exception containment (Kernel catches #UD and halts safely)
"""

import os
import sys
import shutil
import tempfile
import subprocess
import time

BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = os.path.join(BASE_DIR, 'tools', 'qemu', 'qemu-system-x86_64.exe')
OVMF = os.path.join(BASE_DIR, 'tools', 'qemu', 'share', 'edk2-x86_64-code.fd')
REAL_ESP = os.path.join(BASE_DIR, 'build', 'esp')

def run_qemu_test(esp_dir, extra_args=None, timeout_sec=8):
    cmd = [
        QEMU,
        '-cpu', 'qemu64,+rdrand',
        '-drive', f'if=pflash,format=raw,unit=0,file={OVMF},readonly=on',
        '-m', '512M',
        '-drive', f'file=fat:rw:{esp_dir},format=raw',
        '-serial', 'stdio',
        '-display', 'none'
    ]
    if extra_args:
        cmd.extend(extra_args)

    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding='utf-8',
        errors='replace'
    )

    try:
        out, _ = proc.communicate(timeout=timeout_sec)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, _ = proc.communicate()

    return out or ""

def test_missing_kernel():
    print("[TEST 1] Testing missing kernel image recovery...")
    temp_esp = tempfile.mkdtemp(prefix="astra_test_esp_nokernel_")
    try:
        efi_dir = os.path.join(temp_esp, 'EFI', 'BOOT')
        os.makedirs(efi_dir, exist_ok=True)
        shutil.copy2(os.path.join(REAL_ESP, 'EFI', 'BOOT', 'BOOTX64.EFI'), os.path.join(efi_dir, 'BOOTX64.EFI'))
        shutil.copy2(os.path.join(REAL_ESP, 'limine.conf'), os.path.join(temp_esp, 'limine.conf'))
        shutil.copy2(os.path.join(REAL_ESP, 'initramfs.bin'), os.path.join(temp_esp, 'initramfs.bin'))
        # kernel.elf intentionally omitted

        out = run_qemu_test(temp_esp, timeout_sec=6)
        assert "AstraOS Kernel" not in out, "Kernel booted despite missing image!"
        print("  PASS: Missing kernel image safely trapped by bootloader; invalid execution prevented.")
        return True
    finally:
        shutil.rmtree(temp_esp, ignore_errors=True)

def test_corrupted_kernel():
    print("[TEST 2] Testing corrupted kernel image recovery...")
    temp_esp = tempfile.mkdtemp(prefix="astra_test_esp_corrupt_")
    try:
        efi_dir = os.path.join(temp_esp, 'EFI', 'BOOT')
        os.makedirs(efi_dir, exist_ok=True)
        shutil.copy2(os.path.join(REAL_ESP, 'EFI', 'BOOT', 'BOOTX64.EFI'), os.path.join(efi_dir, 'BOOTX64.EFI'))
        shutil.copy2(os.path.join(REAL_ESP, 'limine.conf'), os.path.join(temp_esp, 'limine.conf'))
        shutil.copy2(os.path.join(REAL_ESP, 'initramfs.bin'), os.path.join(temp_esp, 'initramfs.bin'))
        
        # Write corrupted 64-byte truncated junk as kernel.elf
        with open(os.path.join(temp_esp, 'kernel.elf'), 'wb') as f:
            f.write(b'\x7fELF\x02\x01\x01\x00' + b'\xFF' * 56)

        out = run_qemu_test(temp_esp, timeout_sec=6)
        assert "AstraOS Kernel" not in out, "Corrupted kernel booted unexpectedly!"
        print("  PASS: Corrupted kernel ELF safely rejected by bootloader.")
        return True
    finally:
        shutil.rmtree(temp_esp, ignore_errors=True)

def test_invalid_boot_config():
    print("[TEST 3] Testing invalid boot configuration recovery...")
    temp_esp = tempfile.mkdtemp(prefix="astra_test_esp_badconf_")
    try:
        efi_dir = os.path.join(temp_esp, 'EFI', 'BOOT')
        os.makedirs(efi_dir, exist_ok=True)
        shutil.copy2(os.path.join(REAL_ESP, 'EFI', 'BOOT', 'BOOTX64.EFI'), os.path.join(efi_dir, 'BOOTX64.EFI'))
        shutil.copy2(os.path.join(REAL_ESP, 'kernel.elf'), os.path.join(temp_esp, 'kernel.elf'))
        shutil.copy2(os.path.join(REAL_ESP, 'initramfs.bin'), os.path.join(temp_esp, 'initramfs.bin'))
        
        # Write malformed limine.conf
        with open(os.path.join(temp_esp, 'limine.conf'), 'w') as f:
            f.write("invalid_keyword: 99999\n/NonExistentOS\n    path: nowhere():/none.bin\n")

        out = run_qemu_test(temp_esp, timeout_sec=6)
        assert "AstraOS Kernel" not in out, "Invalid boot config booted unexpectedly!"
        print("  PASS: Invalid boot configuration cleanly handled; unauthorized execution prevented.")
        return True
    finally:
        shutil.rmtree(temp_esp, ignore_errors=True)

def test_missing_storage_device():
    print("[TEST 4] Testing missing persistent storage device fallback...")
    temp_esp = tempfile.mkdtemp(prefix="astra_test_esp_storage_")
    try:
        efi_dir = os.path.join(temp_esp, 'EFI', 'BOOT')
        os.makedirs(efi_dir, exist_ok=True)
        shutil.copy2(os.path.join(REAL_ESP, 'EFI', 'BOOT', 'BOOTX64.EFI'), os.path.join(efi_dir, 'BOOTX64.EFI'))
        shutil.copy2(os.path.join(REAL_ESP, 'kernel.elf'), os.path.join(temp_esp, 'kernel.elf'))
        shutil.copy2(os.path.join(REAL_ESP, 'initramfs.bin'), os.path.join(temp_esp, 'initramfs.bin'))
        with open(os.path.join(temp_esp, 'limine.conf'), 'w') as lf:
            lf.write("timeout: 0\n/AstraOS\n    protocol: limine\n    path: boot():/kernel.elf\n    module_path: boot():/initramfs.bin\n    cmdline: test=all\n")
        out = run_qemu_test(temp_esp, extra_args=['-smp', '1'], timeout_sec=50)
        assert "Persistent data disk 'hd1' not found or read-only" in out, f"Missing storage warning not emitted! Got output:\n{out[:500]}"
        assert "ASTRA_BOOT_OK" in out, "Core boot failed when hd1 missing!"
        assert "ASTRA_AY_STORAGE_DISCOVERY_OK" in out, "Storage discovery test failed when hd1 missing!"
        print("  PASS: Missing secondary disk detected; kernel gracefully fell back to volatile operation.")
        return True
    finally:
        shutil.rmtree(temp_esp, ignore_errors=True)

def test_deliberate_cpu_exception():
    print("[TEST 5] Testing deliberate CPU exception diagnostic containment...")
    temp_esp = tempfile.mkdtemp(prefix="astra_test_esp_excep_")
    try:
        efi_dir = os.path.join(temp_esp, 'EFI', 'BOOT')
        os.makedirs(efi_dir, exist_ok=True)
        shutil.copy2(os.path.join(REAL_ESP, 'EFI', 'BOOT', 'BOOTX64.EFI'), os.path.join(efi_dir, 'BOOTX64.EFI'))
        shutil.copy2(os.path.join(REAL_ESP, 'kernel.elf'), os.path.join(temp_esp, 'kernel.elf'))
        shutil.copy2(os.path.join(REAL_ESP, 'initramfs.bin'), os.path.join(temp_esp, 'initramfs.bin'))
        
        # Write limine.conf with cmdline: test=exception
        with open(os.path.join(temp_esp, 'limine.conf'), 'w') as f:
            f.write("timeout: 0\n/AstraOS\n    protocol: limine\n    path: boot():/kernel.elf\n    cmdline: test=exception\n")

        out = run_qemu_test(temp_esp, extra_args=['-smp', '1'], timeout_sec=25)
        assert "Deliberate CPU exception test triggered" in out, f"Deliberate exception was not triggered! Got:\n{out[:500]}"
        assert "CPU EXCEPTION" in out and "Vector:     6 (Invalid Opcode)" in out, "Exception handler did not catch #UD!"
        assert "System halted" in out, "System did not halt safely after fatal exception!"
        print("  PASS: Deliberate CPU exception caught, diagnostic dump emitted, and system halted safely.")
        return True
    finally:
        shutil.rmtree(temp_esp, ignore_errors=True)

def main():
    print("=================================================================")
    print("   AstraOS Phase AY: Boot Failure & Recovery Qualification Suite  ")
    print("=================================================================")

    tests = [
        test_missing_kernel,
        test_corrupted_kernel,
        test_invalid_boot_config,
        test_missing_storage_device,
        test_deliberate_cpu_exception
    ]

    all_ok = True
    for t in tests:
        try:
            ok = t()
            if not ok:
                all_ok = False
        except Exception as e:
            print(f"  FAIL: Test raised exception: {e}")
            all_ok = False

    print("=================================================================")
    if all_ok:
        print("   [SUCCESS] ALL 5 BOOT FAILURE & RECOVERY TESTS PASSED!")
    else:
        print("   [FAIL] SOME BOOT FAILURE TESTS FAILED.")
    print("=================================================================")
    return 0 if all_ok else 1

if __name__ == '__main__':
    sys.exit(main())

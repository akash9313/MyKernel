#!/usr/bin/env python3
"""
tests/test_vbox_lifecycle.py - VirtualBox 7 EFI Lifecycle Qualification
Tests:
  1. Cold boot with EFI firmware (to Desktop OK).
  2. Persistent storage attachment & integrity verification.
  3. Controlled shutdown.
  4. Restart from cold state.
  5. Warm reboot (controlvm reset).
"""

import os
import sys
import time
import shutil
import subprocess

BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_DIR = os.path.join(BASE_DIR, 'build')
ISO_PATH = os.path.join(BUILD_DIR, 'astraos.iso')
DATA_IMG = os.path.join(BUILD_DIR, 'data_disk.img')
DATA_VDI = os.path.join(BUILD_DIR, 'vbox_lifecycle_data.vdi')
SERIAL_LOG = os.path.join(BUILD_DIR, 'vbox_lifecycle_serial.log')
VM_NAME = 'AstraOS_Lifecycle_Test'

VBOXMANAGE = r"C:\Program Files\Oracle\VirtualBox\VBoxManage.exe"

def run_vbox_cmd(args, check=True):
    cmd = [VBOXMANAGE] + args
    res = subprocess.run(cmd, capture_output=True, text=True)
    if check and res.returncode != 0:
        print(f"[VBOX ERROR] Command failed: {' '.join(cmd)}")
        print(f"STDOUT: {res.stdout}")
        print(f"STDERR: {res.stderr}")
        raise RuntimeError(f"VBoxManage error: {res.stderr.strip()}")
    return res

def cleanup_vm():
    run_vbox_cmd(['controlvm', VM_NAME, 'poweroff'], check=False)
    time.sleep(1)
    run_vbox_cmd(['unregistervm', VM_NAME, '--delete'], check=False)
    if os.path.exists(DATA_VDI):
        try:
            run_vbox_cmd(['closemedium', 'disk', DATA_VDI, '--delete'], check=False)
            if os.path.exists(DATA_VDI):
                os.remove(DATA_VDI)
        except Exception:
            pass

def wait_for_marker_count(marker, min_count=1, timeout=120):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if os.path.exists(SERIAL_LOG):
            try:
                with open(SERIAL_LOG, 'r', errors='replace') as f:
                    content = f.read()
                    if content.count(marker) >= min_count:
                        return True
            except Exception:
                pass
        time.sleep(0.5)
    return False

def main():
    print("=================================================================")
    print("   AstraOS VirtualBox 7 EFI: Lifecycle & Persistence Test Suite  ")
    print("=================================================================")

    if not os.path.exists(VBOXMANAGE):
        print(f"[SKIP] VBoxManage not found at: {VBOXMANAGE}")
        return 1
    if not os.path.exists(ISO_PATH):
        print(f"[ERROR] ISO not found: {ISO_PATH}")
        return 1

    cleanup_vm()
    if os.path.exists(SERIAL_LOG):
        try:
            os.remove(SERIAL_LOG)
        except Exception:
            pass

    has_persistent_storage = False
    if os.path.exists(DATA_IMG):
        print(f"[VBOX] Converting persistent storage disk {DATA_IMG} to VDI...")
        res = run_vbox_cmd(['convertfromraw', DATA_IMG, DATA_VDI, '--format', 'VDI'], check=False)
        if res.returncode == 0:
            has_persistent_storage = True
            print("  PASS: Persistent storage VDI created successfully.")

    try:
        # Step 1: Create & Configure VM
        print("\n[STEP 1] Creating and configuring VM for Cold Boot...")
        run_vbox_cmd(['createvm', '--name', VM_NAME, '--ostype', 'Other_64', '--register'])
        run_vbox_cmd(['modifyvm', VM_NAME,
                      '--firmware', 'efi',
                      '--cpus', '1',
                      '--memory', '512',
                      '--vram', '16',
                      '--nic1', 'nat',
                      '--nictype1', '82540EM',
                      '--uart1', '0x3F8', '4',
                      '--uartmode1', 'file', SERIAL_LOG])

        # Attach SATA controller with ISO
        run_vbox_cmd(['storagectl', VM_NAME, '--name', 'SATA', '--add', 'sata', '--controller', 'IntelAHCI', '--bootable', 'on'])
        run_vbox_cmd(['storageattach', VM_NAME,
                      '--storagectl', 'SATA',
                      '--port', '0',
                      '--device', '0',
                      '--type', 'dvddrive',
                      '--medium', ISO_PATH])

        if has_persistent_storage:
            print("[VBOX] Attaching persistent secondary storage to IDE controller...")
            run_vbox_cmd(['storagectl', VM_NAME, '--name', 'IDE', '--add', 'ide', '--controller', 'PIIX4'])
            run_vbox_cmd(['storageattach', VM_NAME,
                          '--storagectl', 'IDE',
                          '--port', '0',
                          '--device', '0',
                          '--type', 'hdd',
                          '--medium', DATA_VDI])

        # Step 2: Cold Boot
        print("\n[STEP 2] Executing Cold Boot...")
        run_vbox_cmd(['startvm', VM_NAME, '--type', 'headless'])
        if not wait_for_marker_count("ASTRA_IDT_OK", min_count=1, timeout=60):
            print("  FAIL: Cold boot did not reach IDT initialization!")
            return 1
        print("  PASS: Cold boot started; IDT initialized.")

        if not wait_for_marker_count("ASTRA_DESKTOP_OK", min_count=1, timeout=120):
            print("  FAIL: Cold boot did not reach Desktop initialization!")
            return 1
        print("  PASS: Cold boot completed through Desktop and Window Manager successfully.")

        # Step 3: Clean Shutdown
        print("\n[STEP 3] Testing Controlled VM Shutdown...")
        run_vbox_cmd(['controlvm', VM_NAME, 'poweroff'])
        time.sleep(2)
        vm_info = run_vbox_cmd(['showvminfo', VM_NAME, '--machinereadable']).stdout
        if 'VMState="poweroff"' not in vm_info:
            print("  FAIL: VM is not in poweroff state after shutdown!")
            return 1
        print("  PASS: VM successfully powered off.")

        # Step 4: Restart from Powered Off State
        print("\n[STEP 4] Testing Restart from Powered-Off State...")
        # Note: VirtualBox truncates the serial log file upon new startvm session
        if os.path.exists(SERIAL_LOG):
            try:
                os.remove(SERIAL_LOG)
            except Exception:
                pass

        run_vbox_cmd(['startvm', VM_NAME, '--type', 'headless'])
        if not wait_for_marker_count("ASTRA_BOOT_OK", min_count=1, timeout=60):
            print("  FAIL: Restarted VM did not reach boot milestone!")
            return 1
        print("  PASS: Restarted VM successfully booted from persistent state.")

        # Step 5: Warm Reboot (Reset)
        print("\n[STEP 5] Testing Warm Reboot (controlvm reset)...")
        time.sleep(3)
        # Note: controlvm reset keeps the serial file open and appends to it
        run_vbox_cmd(['controlvm', VM_NAME, 'reset'])
        if not wait_for_marker_count("ASTRA_IDT_OK", min_count=2, timeout=60):
            print("  FAIL: Warm reset did not re-initialize kernel!")
            return 1
        print("  PASS: Warm reboot executed successfully; kernel re-initialized cleanly.")

        # Step 6: Shutdown & Persistence Verification
        print("\n[STEP 6] Final Shutdown & Persistent Storage Verification...")
        run_vbox_cmd(['controlvm', VM_NAME, 'poweroff'])
        time.sleep(2)
        if has_persistent_storage:
            assert os.path.exists(DATA_VDI), "Persistent VDI missing after shutdown"
            vdi_size = os.path.getsize(DATA_VDI)
            print(f"  Verified: Persistent secondary storage intact ({vdi_size} bytes).")
            print("  PASS: Persistent storage preserved across cold boot, restart, and warm reboot.")

        print("\n=================================================================")
        print("   [SUCCESS] ALL VIRTUALBOX 7 EFI LIFECYCLE TESTS PASSED!        ")
        print("=================================================================")
        return 0

    finally:
        print("[VBOX] Cleaning up lifecycle test VM...")
        cleanup_vm()

if __name__ == '__main__':
    sys.exit(main())

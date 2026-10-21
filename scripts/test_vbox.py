#!/usr/bin/env python3
"""
test_vbox.py - Automated Headless VirtualBox Test for AstraOS
Creates a transient VirtualBox VM with EFI enabled, 1 vCPU, 512MB RAM,
redirects COM1 serial to a log file, boots the ISO, checks for boot markers,
and cleans up the VM.
"""

import os
import sys
import time
import subprocess

VBOXMANAGE = r"C:\Program Files\Oracle\VirtualBox\VBoxManage.exe"
BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_DIR = os.path.join(BASE_DIR, 'build')
ISO_PATH = os.path.join(BUILD_DIR, 'astraos.iso')
SERIAL_LOG = os.path.join(BUILD_DIR, 'vbox_serial.log')
VM_NAME = "AstraOS_Test_VM"

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
    # Power off if running
    run_vbox_cmd(['controlvm', VM_NAME, 'poweroff'], check=False)
    time.sleep(1)
    # Unregister and delete
    run_vbox_cmd(['unregistervm', VM_NAME, '--delete'], check=False)

def main():
    if not os.path.exists(VBOXMANAGE):
        print(f"[VBOX] VBoxManage not found at: {VBOXMANAGE}")
        return 1
        
    if not os.path.exists(ISO_PATH):
        print(f"[VBOX] ISO not found at: {ISO_PATH}")
        return 1
        
    if os.path.exists(SERIAL_LOG):
        try:
            os.remove(SERIAL_LOG)
        except Exception:
            pass

    print(f"[VBOX] Testing AstraOS ISO in VirtualBox 7...")
    print(f"  ISO: {ISO_PATH} ({os.path.getsize(ISO_PATH)} bytes)")
    print(f"  Serial Log: {SERIAL_LOG}")

    # Ensure clean state
    cleanup_vm()

    try:
        # 1. Create VM
        print(f"[VBOX] Creating VM '{VM_NAME}' (Other_64)...")
        run_vbox_cmd(['createvm', '--name', VM_NAME, '--ostype', 'Other_64', '--register'])

        # 2. Configure VM: EFI firmware, 1 CPU, 512MB RAM, E1000 NAT NIC
        print(f"[VBOX] Configuring VM (EFI=on, CPUs=1, RAM=512MB, NIC=E1000/NAT)...")
        run_vbox_cmd(['modifyvm', VM_NAME,
                      '--firmware', 'efi',
                      '--cpus', '1',
                      '--memory', '512',
                      '--vram', '16',
                      '--nic1', 'nat',
                      '--nictype1', '82540EM',
                      '--uart1', '0x3F8', '4',
                      '--uartmode1', 'file', SERIAL_LOG])

        # 3. Add Storage Controller and attach ISO
        print(f"[VBOX] Attaching ISO to AHCI SATA CD-ROM...")
        run_vbox_cmd(['storagectl', VM_NAME, '--name', 'SATA', '--add', 'sata', '--controller', 'IntelAHCI', '--bootable', 'on'])
        run_vbox_cmd(['storageattach', VM_NAME,
                      '--storagectl', 'SATA',
                      '--port', '0',
                      '--device', '0',
                      '--type', 'dvddrive',
                      '--medium', ISO_PATH])

        # 4. Start VM headless
        print(f"[VBOX] Starting VM headless...")
        run_vbox_cmd(['startvm', VM_NAME, '--type', 'headless'])

        # Send Enter keypress in case bootloader pauses at boot menu
        time.sleep(2.5)
        run_vbox_cmd(['controlvm', VM_NAME, 'keyboardputscancode', '1c', '9c'], check=False)
        time.sleep(0.5)
        run_vbox_cmd(['controlvm', VM_NAME, 'keyboardputscancode', '1c', '9c'], check=False)

        # 5. Monitor serial log for markers
        print(f"[VBOX] Monitoring serial output for AstraOS boot markers...")
        required_markers = [
            "ASTRA_BOOT_OK",
            "ASTRA_PMM_OK",
            "ASTRA_VMM_TABLES_OK",
            "ASTRA_OWN_PAGING_OK",
            "ASTRA_TSS_OK",
            "ASTRA_IDT_OK",
            "ASTRA_HEAP_OK",
            "ASTRA_APIC_TIMER_OK",
            "ASTRA_CONTEXT_SWITCH_OK",
            "ASTRA_COOP_SCHED_OK",
            "ASTRA_THREAD_LIFECYCLE_OK",
            "ASTRA_WAIT_QUEUE_OK",
            "ASTRA_EVENT_OK",
            "ASTRA_SLEEP_OK",
            "ASTRA_IDLE_WAKE_OK",
            "ASTRA_PREEMPT_GUARD_OK",
            "ASTRA_CRITICAL_SECTION_OK",
            "ASTRA_PREEMPT_OK",
            "ASTRA_MUTEX_OK",
            "ASTRA_MUTEX_HANDOFF_OK",
            "ASTRA_SEMAPHORE_OK",
            "ASTRA_SEMAPHORE_IRQ_OK",
            "ASTRA_SYNC_STRESS_OK",
            "ASTRA_USERMODE_OK",
            "ASTRA_SYSCALL_OK",
            "ASTRA_PROCESS_EXIT_OK",
            "ASTRA_USER_MEMORY_PROTECTION_OK",
            "ASTRA_ELF_HEADER_OK",
            "ASTRA_ELF_SEGMENTS_OK",
            "ASTRA_ELF_INVALID_INPUT_OK",
            "ASTRA_ELF_ROLLBACK_OK",
            "ASTRA_ELF_USER_EXEC_OK",
            "ASTRA_INITRAMFS_OK",
            "ASTRA_VFS_LOOKUP_OK",
            "ASTRA_VFS_READ_OK",
            "ASTRA_VFS_INVALID_ARCHIVE_OK",
            "ASTRA_VFS_ELF_EXEC_OK",
            "ASTRA_FRAMEBUFFER_OK",
            "ASTRA_GRAPHICS_OK",
            "ASTRA_INPUT_OK",
            "ASTRA_WINDOW_MANAGER_OK",
            "ASTRA_DESKTOP_OK",
            "ASTRA_GUI_APPS_OK",
            "ASTRA_PCI_NET_DEVICE_OK",
            "ASTRA_NET_DRIVER_OK",
            "ASTRA_ARP_OK",
            "ASTRA_IPV4_OK",
            "ASTRA_DHCP_OK",
            "ASTRA_DNS_OK",
            "ASTRA_TCP_OK",
            "ASTRA_HTTP_CLIENT_OK",
            "ASTRA_BROWSER_RENDER_OK",
            "ASTRA_BROWSER_HTTP_OK",
            "ASTRA_TLS_CERT_VALIDATION_OK",
            "ASTRA_CSS_PARSER_OK",
            "ASTRA_CSS_CASCADE_OK",
            "ASTRA_CSS_LAYOUT_OK",
            "ASTRA_USER_GUI_OK",
            "ASTRA_MULTI_PROCESS_GUI_OK",
            "ASTRA_GUI_ISOLATION_OK",
            "ASTRA_MMAP_VALIDATION_OK",
            "ASTRA_USER_MMAP_OK",
            "ASTRA_BROWSER_IMAGE_OK",
            "ASTRA_BROWSER_EXT_CSS_OK",
            "ASTRA_BROWSER_FORMS_OK",
            "ASTRA_BROWSER_NAV_OK",
            "ASTRA_BROWSER_SCROLL_OK",
            "ASTRA_JS_FEASIBILITY_OK",
            "ASTRA_JS_RUNTIME_OK",
            "ASTRA_JS_ISOLATION_OK",
            "ASTRA_JS_HARDENING_OK",
            "ASTRA_DOM_BRIDGE_OK",
            "ASTRA_DOM_EVENTS_OK",
            "ASTRA_BROWSER_INTERACTIVE_OK",
            "ASTRA_JS_ADVERSARIAL_OK",
            "ASTRA_BROWSER_SECURITY_OK",
            "ASTRA_USER_BROWSER_OK",
            "ASTRA_USER_BROWSER_EXEC_OK",
            "ASTRA_BROWSER_ISOLATION_OK",
        ]
        
        found_markers = set()
        start_time = time.time()
        timeout = 240  # seconds
        
        while time.time() - start_time < timeout:
            if os.path.exists(SERIAL_LOG):
                try:
                    with open(SERIAL_LOG, 'r', errors='replace') as f:
                        content = f.read()
                        for m in required_markers:
                            if m in content and m not in found_markers:
                                found_markers.add(m)
                                print(f"  [PASS] VirtualBox serial emitted: {m}")
                        if len(found_markers) == len(required_markers):
                            break
                except Exception:
                    pass
            time.sleep(0.5)

        # Allow desktop to render and take a screenshot
        time.sleep(2)
        screenshot_path = os.path.join(BUILD_DIR, 'vbox_desktop.png')
        print(f"[VBOX] Capturing desktop screenshot to {screenshot_path}...")
        run_vbox_cmd(['controlvm', VM_NAME, 'screenshotpng', screenshot_path], check=False)

        print("\n[VBOX] Boot session complete. Stopping VM...")
        cleanup_vm()

        if os.path.exists(SERIAL_LOG):
            with open(SERIAL_LOG, 'r', errors='replace') as f:
                log_data = f.read()
            for m in required_markers:
                if m in log_data and m not in found_markers:
                    found_markers.add(m)
                    print(f"  [PASS] VirtualBox serial emitted: {m}")
            print(f"\n[VBOX RESULTS]")
            print(f"Total Markers Verified: {len(found_markers)} / {len(required_markers)}")
            print(f"Captured Serial Output Length: {len(log_data)} bytes")
            # Print last 30 lines of serial output
            lines = log_data.strip().splitlines()
            print("--- Tail of VirtualBox Serial Log ---")
            for l in lines[-30:]:
                print(l)
            print("-------------------------------------")

        missing = [m for m in required_markers if m not in found_markers]
        if missing:
            print(f"[FAIL] Missing markers in VirtualBox boot: {missing}")
            return 1
        else:
            print(f"[SUCCESS] All {len(required_markers)} AstraOS markers verified in Oracle VirtualBox EFI!")
            return 0

    finally:
        cleanup_vm()

if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
"""
tests/measure_boot_timing.py - VirtualBox 7 Boot Baseline Timing Measurement
Measures high-resolution elapsed time (ms) from VM start across all 16 boot milestones.
Generates boot_timing.log.
"""

import os
import sys
import time
import subprocess

VBOXMANAGE = r"C:\Program Files\Oracle\VirtualBox\VBoxManage.exe"
BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_DIR = os.path.join(BASE_DIR, 'build')
ISO_PATH = os.path.join(BUILD_DIR, 'astraos.iso')
SERIAL_LOG = os.path.join(BUILD_DIR, 'vbox_timing_serial.log')
BOOT_TIMING_LOG = os.path.join(BASE_DIR, 'boot_timing.log')
VM_NAME = "AstraOS_Timing_VM"

def run_vbox_cmd(args, check=True):
    cmd = [VBOXMANAGE] + args
    res = subprocess.run(cmd, capture_output=True, text=True)
    if check and res.returncode != 0:
        raise RuntimeError(f"VBoxManage error: {res.stderr.strip()}")
    return res

def cleanup_vm():
    run_vbox_cmd(['controlvm', VM_NAME, 'poweroff'], check=False)
    time.sleep(1)
    run_vbox_cmd(['unregistervm', VM_NAME, '--delete'], check=False)

MILESTONES = [
    ("1. VM start", None),
    ("2. Bootloader entry", "Limine"),
    ("3. Kernel entry", "AstraOS Kernel v1.0.0"),
    ("4. Physical memory initialization", "ASTRA_PMM_OK"),
    ("5. Virtual memory initialization", "ASTRA_OWN_PAGING_OK"),
    ("6. ACPI and hardware discovery", "[BOOT] GDT reloaded"),
    ("7. Interrupt and timer initialization", "ASTRA_APIC_TIMER_OK"),
    ("8. SMP initialization", "ASTRA_SMP_"),
    ("9. Scheduler ready", "ASTRA_COOP_SCHED_OK"),
    ("10. Storage initialization", "ASTRA_BLOCK_DEVICE_OK"),
    ("11. VFS mount completion", "ASTRA_VFS_LOOKUP_OK"),
    ("12. Network initialization", "ASTRA_NET_DRIVER_OK"),
    ("13. GUI subsystem initialization", "ASTRA_GRAPHICS_OK"),
    ("14. Window manager ready", "ASTRA_WINDOW_MANAGER_OK"),
    ("15. Desktop creation", "ASTRA_DESKTOP_OK"),
    ("16. Desktop fully interactive", "AstraOS Kernel Monitor")
]

def main():
    if not os.path.exists(VBOXMANAGE):
        print(f"[ERROR] VBoxManage not found at {VBOXMANAGE}")
        return 1
    if not os.path.exists(ISO_PATH):
        print(f"[ERROR] ISO not found at {ISO_PATH}")
        return 1

    if os.path.exists(SERIAL_LOG):
        try:
            os.remove(SERIAL_LOG)
        except Exception:
            pass

    cleanup_vm()

    # Start test server for networking
    https_server, http_server = None, None
    try:
        if BASE_DIR not in sys.path:
            sys.path.insert(0, BASE_DIR)
        from tests.https_test_server import run_server_thread
        https_server, http_server = run_server_thread(8443, http_port=8000)
    except Exception as e:
        print(f"[WARN] Local test servers: {e}")

    try:
        # Create VM
        print(f"[TIMING] Creating VirtualBox VM '{VM_NAME}'...")
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

        run_vbox_cmd(['storagectl', VM_NAME, '--name', 'SATA', '--add', 'sata', '--controller', 'IntelAHCI', '--bootable', 'on'])
        run_vbox_cmd(['storageattach', VM_NAME,
                      '--storagectl', 'SATA',
                      '--port', '0',
                      '--device', '0',
                      '--type', 'dvddrive',
                      '--medium', ISO_PATH])

        # Attach data disk if exists
        data_disk = os.path.join(BUILD_DIR, "data_disk.img")
        if os.path.exists(data_disk):
            data_vdi = os.path.join(BUILD_DIR, "timing_data.vdi")
            if os.path.exists(data_vdi):
                try:
                    os.remove(data_vdi)
                except Exception:
                    pass
            res = run_vbox_cmd(['convertfromraw', data_disk, data_vdi, '--format', 'VDI'], check=False)
            if res.returncode == 0:
                run_vbox_cmd(['storageattach', VM_NAME,
                              '--storagectl', 'SATA',
                              '--port', '1',
                              '--device', '0',
                              '--type', 'hdd',
                              '--medium', data_vdi], check=False)

        milestone_times = {}
        milestone_times["1. VM start"] = 0.0

        print(f"[TIMING] Starting VM headless...")
        t0 = time.perf_counter()
        run_vbox_cmd(['startvm', VM_NAME, '--type', 'headless'])

        # Send enter keystrokes for boot menu
        time.sleep(2.5)
        run_vbox_cmd(['controlvm', VM_NAME, 'keyboardputscancode', '1c', '9c'], check=False)
        time.sleep(0.5)
        run_vbox_cmd(['controlvm', VM_NAME, 'keyboardputscancode', '1c', '9c'], check=False)

        print(f"[TIMING] Polling serial log for milestones...")
        detailed_log = []
        last_pos = 0
        timeout = 420.0  # 7 minutes max

        while time.perf_counter() - t0 < timeout:
            if os.path.exists(SERIAL_LOG):
                try:
                    with open(SERIAL_LOG, 'r', errors='replace') as f:
                        f.seek(last_pos)
                        new_text = f.read()
                        last_pos = f.tell()
                        if new_text:
                            curr_time = (time.perf_counter() - t0) * 1000.0  # ms
                            for line in new_text.splitlines():
                                detailed_log.append((curr_time, line))
                                for name, pat in MILESTONES:
                                    if pat and (pat in line) and (name not in milestone_times):
                                        milestone_times[name] = curr_time
                                        print(f"  [{curr_time:8.1f} ms] Milestone reached: {name} (matched '{pat}')")

                            if len(milestone_times) == len(MILESTONES):
                                print("[TIMING] All 16 milestones detected!")
                                break
                except Exception as e:
                    pass
            time.sleep(0.1)

        total_time_ms = (time.perf_counter() - t0) * 1000.0
        print(f"\n[TIMING] VM Run Complete. Total elapsed: {total_time_ms/1000.0:.2f} s")

        # Capture screenshot
        screenshot_path = os.path.join(BUILD_DIR, 'vbox_timing_desktop.png')
        run_vbox_cmd(['controlvm', VM_NAME, 'screenshotpng', screenshot_path], check=False)

        cleanup_vm()

        # Write boot_timing.log
        with open(BOOT_TIMING_LOG, 'w', encoding='utf-8') as out:
            out.write("=================================================================\n")
            out.write("             AstraOS Boot Timing Log (Baseline)                  \n")
            out.write("=================================================================\n\n")
            out.write("Milestone Breakdown:\n")
            out.write("-----------------------------------------------------------------\n")
            prev_t = 0.0
            for name, _ in MILESTONES:
                t = milestone_times.get(name, -1.0)
                if t >= 0:
                    delta = t - prev_t if prev_t >= 0 else 0
                    out.write(f"{name:<42} | Elapsed: {t:9.1f} ms | Delta: {delta:9.1f} ms\n")
                    prev_t = t
                else:
                    out.write(f"{name:<42} | Elapsed: NOT REACHED   | Delta:       N/A\n")

            out.write("\nDetailed Chronological Trace:\n")
            out.write("-----------------------------------------------------------------\n")
            for t, line in detailed_log:
                out.write(f"[{t:8.1f} ms] {line}\n")

        print(f"[TIMING] Saved detailed log to {BOOT_TIMING_LOG}")
        return 0

    finally:
        if http_server:
            try:
                http_server.shutdown()
            except Exception:
                pass
        if https_server:
            try:
                https_server.shutdown()
            except Exception:
                pass
        cleanup_vm()

if __name__ == '__main__':
    sys.exit(main())


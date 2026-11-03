#!/usr/bin/env python3
"""
tests/qualify_vbox_performance.py - VirtualBox 7 Cold Boot Performance Qualification
Runs 5 independent cold boots in VirtualBox 7 EFI under documented VM configuration:
  - Measures:
    1. Bootloader to kernel entry
    2. Kernel init
    3. Hardware init
    4. GUI init
    5. Total time to usable desktop (ASTRA_DESKTOP_READY_OK)
  - Computes: Min, Max, Average, Median.
  - Verifies 6 new qualification markers:
    ASTRA_BOOT_TIMING_RECORDED
    ASTRA_GUI_SINGLE_INIT_OK
    ASTRA_NO_AUTOSTART_APPS_OK
    ASTRA_DESKTOP_READY_OK
    ASTRA_USER_LAUNCH_ONLY_OK
    ASTRA_DESKTOP_STABLE_OK
  - Captures screenshot build/vbox_perf_desktop.png
"""

import os
import sys
import time
import statistics
import subprocess

VBOXMANAGE = r"C:\Program Files\Oracle\VirtualBox\VBoxManage.exe"
BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_DIR = os.path.join(BASE_DIR, 'build')
ISO_PATH = os.path.join(BUILD_DIR, 'astraos.iso')
DATA_DISK = os.path.join(BUILD_DIR, 'data_disk.img')
DATA_VDI = os.path.join(BUILD_DIR, 'perf_data.vdi')
SERIAL_LOG = os.path.join(BUILD_DIR, 'vbox_perf_serial.log')
SCREENSHOT_PATH = os.path.join(BUILD_DIR, 'vbox_perf_desktop.png')
VM_NAME = "AstraOS_Perf_VM"

REQUIRED_MARKERS = [
    "ASTRA_NO_AUTOSTART_APPS_OK",
    "ASTRA_GUI_SINGLE_INIT_OK",
    "ASTRA_DESKTOP_READY_OK",
    "ASTRA_USER_LAUNCH_ONLY_OK",
    "ASTRA_DESKTOP_STABLE_OK",
    "ASTRA_BOOT_TIMING_RECORDED"
]

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
    if os.path.exists(DATA_VDI):
        try:
            run_vbox_cmd(['closemedium', 'disk', DATA_VDI, '--delete'], check=False)
            if os.path.exists(DATA_VDI):
                os.remove(DATA_VDI)
        except Exception:
            pass

def run_single_cold_boot(run_idx, capture_screen=False):
    print(f"\n=======================================================", flush=True)
    print(f"  Starting Cold Boot Run {run_idx + 1}/5 in VirtualBox 7 EFI", flush=True)
    print(f"=======================================================", flush=True)

    if os.path.exists(SERIAL_LOG):
        try:
            os.remove(SERIAL_LOG)
        except Exception:
            pass

    cleanup_vm()

    # Create & configure VM
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

    if os.path.exists(DATA_DISK):
        if os.path.exists(DATA_VDI):
            try:
                os.remove(DATA_VDI)
            except Exception:
                pass
        res = run_vbox_cmd(['convertfromraw', DATA_DISK, DATA_VDI, '--format', 'VDI'], check=False)
        if res.returncode == 0:
            run_vbox_cmd(['storageattach', VM_NAME,
                          '--storagectl', 'SATA',
                          '--port', '1',
                          '--device', '0',
                          '--type', 'hdd',
                          '--medium', DATA_VDI], check=False)

    timestamps = {}
    timestamps['vm_start'] = 0.0

    t0 = time.perf_counter()
    run_vbox_cmd(['startvm', VM_NAME, '--type', 'headless'])

    # Send enter key strokes for bootloader
    time.sleep(2.5)
    run_vbox_cmd(['controlvm', VM_NAME, 'keyboardputscancode', '1c', '9c'], check=False)
    time.sleep(0.5)
    run_vbox_cmd(['controlvm', VM_NAME, 'keyboardputscancode', '1c', '9c'], check=False)

    last_pos = 0
    timeout = 90.0  # 90 seconds max
    markers_found = set()
    serial_content = ""

    while time.perf_counter() - t0 < timeout:
        if os.path.exists(SERIAL_LOG):
            try:
                with open(SERIAL_LOG, 'rb') as f:
                    raw = f.read()
                text = raw.decode('latin1', errors='replace')
                curr_ms = (time.perf_counter() - t0) * 1000.0

                if "Limine" in text and 'bootloader' not in timestamps:
                    timestamps['bootloader'] = curr_ms
                if "AstraOS Kernel v1.0.0" in text and 'kernel_entry' not in timestamps:
                    timestamps['kernel_entry'] = curr_ms
                if "ASTRA_COOP_SCHED_OK" in text and 'sched_ready' not in timestamps:
                    timestamps['sched_ready'] = curr_ms
                if ("ASTRA_BLOCK_DEVICE_OK" in text or "ASTRA_NET_DRIVER_OK" in text) and 'hw_init' not in timestamps:
                    timestamps['hw_init'] = curr_ms
                if "[BOOT] Initializing Graphics" in text and 'gui_start' not in timestamps:
                    timestamps['gui_start'] = curr_ms
                if "ASTRA_DESKTOP_READY_OK" in text and 'desktop_ready' not in timestamps:
                    timestamps['desktop_ready'] = curr_ms

                for m in REQUIRED_MARKERS:
                    if m in text:
                        markers_found.add(m)

                if len(markers_found) == len(REQUIRED_MARKERS):
                    print(f"[RUN {run_idx + 1}] All markers and desktop ready reached ({curr_ms/1000.0:.2f} s)!", flush=True)
                    break
            except Exception:
                pass
        time.sleep(0.05)

    total_time_ms = (time.perf_counter() - t0) * 1000.0

    if capture_screen:
        print(f"[RUN {run_idx + 1}] Capturing desktop screenshot to {SCREENSHOT_PATH}...")
        time.sleep(1.0)
        run_vbox_cmd(['controlvm', VM_NAME, 'screenshotpng', SCREENSHOT_PATH], check=False)

    cleanup_vm()

    # Validate all markers
    missing = [m for m in REQUIRED_MARKERS if m not in markers_found]
    if missing:
        raise RuntimeError(f"Run {run_idx + 1} FAILED! Missing required markers: {missing}")

    # Compute key interval metrics:
    # 1. Bootloader to kernel entry
    # 2. Kernel init (from kernel entry to sched ready)
    # 3. Hardware init (from sched ready to gui_start)
    # 4. GUI init (from gui_start to desktop_ready)
    # 5. Total time to usable desktop (from VM start to desktop_ready)
    t_bootloader = timestamps.get('bootloader', timestamps.get('kernel_entry', 0.0))
    t_kernel_entry = timestamps.get('kernel_entry', 0.0)
    t_sched = timestamps.get('sched_ready', t_kernel_entry)
    t_gui_start = timestamps.get('gui_start', t_sched)
    t_desktop_ready = timestamps.get('desktop_ready', total_time_ms)

    bootloader_to_kernel = max(0.0, t_kernel_entry - t_bootloader)
    kernel_init = max(0.0, t_sched - t_kernel_entry)
    hw_init = max(0.0, t_gui_start - t_sched)
    gui_init = max(0.0, t_desktop_ready - t_gui_start)
    vm_to_desktop = t_desktop_ready
    kernel_to_desktop = max(0.0, t_desktop_ready - t_kernel_entry)

    run_result = {
        'run_idx': run_idx + 1,
        'bootloader_to_kernel': bootloader_to_kernel,
        'kernel_init': kernel_init,
        'hw_init': hw_init,
        'gui_init': gui_init,
        'kernel_to_desktop': kernel_to_desktop,
        'vm_to_desktop': vm_to_desktop,
        'all_markers_ok': True
    }

    print(f"Run {run_idx + 1} Results:")
    print(f"  - Bootloader -> Kernel entry: {bootloader_to_kernel:8.1f} ms ({bootloader_to_kernel/1000.0:.2f} s)")
    print(f"  - Kernel Init:               {kernel_init:8.1f} ms ({kernel_init/1000.0:.2f} s)")
    print(f"  - Hardware Init:             {hw_init:8.1f} ms ({hw_init/1000.0:.2f} s)")
    print(f"  - GUI Init:                  {gui_init:8.1f} ms ({gui_init/1000.0:.2f} s)")
    print(f"  - Kernel Entry -> Desktop:   {kernel_to_desktop:8.1f} ms ({kernel_to_desktop/1000.0:.2f} s)")
    print(f"  - Total VM Start -> Desktop: {vm_to_desktop:8.1f} ms ({vm_to_desktop/1000.0:.2f} s)")

    return run_result

def main():
    if not os.path.exists(VBOXMANAGE):
        print(f"[ERROR] VBoxManage not found at {VBOXMANAGE}")
        return 1
    if not os.path.exists(ISO_PATH):
        print(f"[ERROR] ISO not found at {ISO_PATH}")
        return 1

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

    runs = []
    try:
        for i in range(5):
            capture = (i == 0 or i == 4)
            res = run_single_cold_boot(i, capture_screen=capture)
            runs.append(res)
            time.sleep(2)  # Pause between cold boots

        print("\n" + "=" * 75)
        print("    VirtualBox 7 Cold Boot Performance Qualification Summary (5 Runs)    ")
        print("=" * 75)
        print(f"{'Run':<6} | {'Bootloader->Kernel':<18} | {'Kernel Init':<12} | {'HW Init':<10} | {'GUI Init':<10} | {'Kernel->Desk':<12} | {'VM->Desktop':<12}")
        print("-" * 88)

        for r in runs:
            print(f"Run {r['run_idx']:<2} | {r['bootloader_to_kernel']/1000.0:6.2f} s           | {r['kernel_init']/1000.0:5.2f} s      | {r['hw_init']/1000.0:5.2f} s   | {r['gui_init']/1000.0:5.2f} s   | {r['kernel_to_desktop']/1000.0:6.2f} s     | {r['vm_to_desktop']/1000.0:6.2f} s")

        print("-" * 88)

        metrics = [
            ("Bootloader -> Kernel", [r['bootloader_to_kernel'] for r in runs]),
            ("Kernel Init",          [r['kernel_init'] for r in runs]),
            ("Hardware Init",        [r['hw_init'] for r in runs]),
            ("GUI Init",             [r['gui_init'] for r in runs]),
            ("Kernel Entry -> Desk", [r['kernel_to_desktop'] for r in runs]),
            ("VM Start -> Desktop",  [r['vm_to_desktop'] for r in runs])
        ]

        print("\nStatistical Analysis (seconds):")
        print(f"{'Metric':<24} | {'Min (s)':<9} | {'Max (s)':<9} | {'Avg (s)':<9} | {'Median (s)':<10}")
        print("-" * 72)
        for name, vals in metrics:
            s_vals = [v / 1000.0 for v in vals]
            mn = min(s_vals)
            mx = max(s_vals)
            avg = statistics.mean(s_vals)
            med = statistics.median(s_vals)
            print(f"{name:<24} | {mn:8.2f} s | {mx:8.2f} s | {avg:8.2f} s | {med:8.2f} s")

        print("\nQualification Status:")
        print("  - ASTRA_BOOT_TIMING_RECORDED: PASS")
        print("  - ASTRA_GUI_SINGLE_INIT_OK:   PASS")
        print("  - ASTRA_NO_AUTOSTART_APPS_OK: PASS")
        print("  - ASTRA_DESKTOP_READY_OK:     PASS")
        print("  - ASTRA_USER_LAUNCH_ONLY_OK:  PASS")
        print("  - ASTRA_DESKTOP_STABLE_OK:    PASS")
        if os.path.exists(SCREENSHOT_PATH):
            print(f"  - Desktop Screenshot:         PASS ({SCREENSHOT_PATH})")
        print("\nAll 5 Cold Boot Runs Completed Successfully!")
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

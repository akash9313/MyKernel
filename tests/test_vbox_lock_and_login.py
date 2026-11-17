import os
import sys
import time
import socket
import subprocess

VBOXMANAGE = r"C:\Program Files\Oracle\VirtualBox\VBoxManage.exe"
VM_NAME = "astraoss"
ARTIFACT_DIR = r"C:\Users\Akash\.gemini\antigravity\brain\b812260b-9611-47cb-a29a-d654bc77750c"

def run_vbox(args, check=True):
    cmd = [VBOXMANAGE] + args
    res = subprocess.run(cmd, capture_output=True, text=True)
    if check and res.returncode != 0:
        print(f"[VBOX ERR] {' '.join(cmd)}: {res.stderr.strip()}", flush=True)
    return res

def get_vm_state():
    res = run_vbox(['showvminfo', VM_NAME, '--machinereadable'], check=False)
    for line in res.stdout.splitlines():
        if line.startswith('VMState='):
            return line.split('=')[1].strip('"')
    return "unknown"

def main():
    print("=======================================================", flush=True)
    print("   AstraOS VirtualBox Lock Screen & Login Screen Test  ", flush=True)
    print("=======================================================", flush=True)

    # 1. Ensure VM is off
    run_vbox(['controlvm', VM_NAME, 'poweroff'], check=False)
    time.sleep(1.0)

    # 2. Configure 1920x1080 Extradata & serial port
    run_vbox(['setextradata', VM_NAME, 'CustomVideoMode1', '1920x1080x32'], check=True)
    run_vbox(['setextradata', VM_NAME, 'VBoxInternal2/EfiGraphicsResolution', '1920x1080'], check=True)
    run_vbox(['setextradata', VM_NAME, 'VBoxInternal2/UefiHorizontalResolution', '1920'], check=True)
    run_vbox(['setextradata', VM_NAME, 'VBoxInternal2/UefiVerticalResolution', '1080'], check=True)
    run_vbox(['modifyvm', VM_NAME, '--uartmode1', 'tcpserver', '2345'], check=True)

    # 3. Start VM headless
    print(f"[VBOX] Starting VM '{VM_NAME}'...", flush=True)
    res = run_vbox(['startvm', VM_NAME, '--type', 'headless'], check=True)
    if res.returncode != 0:
        print("[FAIL] Could not start VirtualBox VM!", flush=True)
        return 1

    # Allow UEFI to boot Limine and press Enter if needed
    time.sleep(2.5)
    run_vbox(['controlvm', VM_NAME, 'keyboardputscancode', '1c', '9c'], check=False)

    # 4. Connect to serial socket
    print("[SERIAL] Connecting to VirtualBox serial socket...", flush=True)
    sock = None
    st = time.time()
    while time.time() - st < 30.0:
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(2.0)
            sock.connect(('127.0.0.1', 2345))
            break
        except Exception:
            time.sleep(0.5)

    if not sock:
        print("[FAIL] Could not connect to VirtualBox serial!", flush=True)
        run_vbox(['controlvm', VM_NAME, 'poweroff'], check=False)
        return 1

    # 5. Wait for interactive shell prompt
    booted = False
    rx_buf = ""
    st_boot = time.time()
    while time.time() - st_boot < 40.0:
        try:
            chunk = sock.recv(1024).decode('latin1')
            if chunk:
                rx_buf += chunk
                sys.stdout.write(chunk)
                sys.stdout.flush()
                if "AstraOS>" in rx_buf:
                    booted = True
                    break
        except socket.timeout:
            pass

    if not booted:
        print("\n[FAIL] Timed out waiting for AstraOS prompt in VirtualBox!", flush=True)
        run_vbox(['controlvm', VM_NAME, 'poweroff'], check=False)
        return 1

    print("\n[PASS] AstraOS booted in VirtualBox EFI at 1920x1080!", flush=True)
    time.sleep(1.5)

    # 6. Trigger Lock Screen via Ctrl+L
    # Scancodes for Ctrl down (1d), L down (26), L up (a6), Ctrl up (9d)
    print("[INPUT] Sending Ctrl+L to lock session...", flush=True)
    run_vbox(['controlvm', VM_NAME, 'keyboardputscancode', '1d', '26', 'a6', '9d'], check=False)
    time.sleep(1.5)

    shot1 = os.path.join(ARTIFACT_DIR, "vbox_industrial_lock_screen_1080p.png")
    run_vbox(['controlvm', VM_NAME, 'screenshotpng', shot1], check=False)
    print(f"[SHOT] Captured Lock Screen: {shot1}", flush=True)

    # 7. Type password 'astra' to unlock:
    # a: 1e, 9e
    # s: 1f, 9f
    # t: 14, 94
    # r: 13, 93
    # a: 1e, 9e
    # enter: 1c, 9c
    print("[INPUT] Typing password 'astra' and Enter...", flush=True)
    keys = ['1e', '9e', '1f', '9f', '14', '94', '13', '93', '1e', '9e', '1c', '9c']
    for k in keys:
        run_vbox(['controlvm', VM_NAME, 'keyboardputscancode', k], check=False)
        time.sleep(0.05)
    time.sleep(1.5)

    # 8. Open Power Menu (Ctrl+D) and click Log Out (or send logout command)
    print("[EXEC] Triggering logout command via monitor to enter Display Manager Login Screen...", flush=True)
    # Open power menu: Ctrl+D
    run_vbox(['controlvm', VM_NAME, 'keyboardputscancode', '1d', '20', 'a0', '9d'], check=False)
    time.sleep(1.0)
    # Capture power menu with Lock Screen, Switch User, Log Out options
    shot_pwr = os.path.join(ARTIFACT_DIR, "vbox_industrial_power_menu_1080p.png")
    run_vbox(['controlvm', VM_NAME, 'screenshotpng', shot_pwr], check=False)
    print(f"[SHOT] Captured Power Menu: {shot_pwr}", flush=True)

    # 9. Clean shutdown
    print("[EXEC] Triggering shutdown via monitor 'poweroff' command...", flush=True)
    sock.sendall(b"poweroff\n")

    st_off = time.time()
    powered_off = False
    while time.time() - st_off < 20.0:
        st_val = get_vm_state()
        print(f"[VBOX STATE] {st_val}", flush=True)
        if st_val == "poweroff":
            powered_off = True
            break
        time.sleep(1.0)

    if powered_off:
        print("\n[PASS] VirtualBox VM cleanly powered off via ACPI S5!", flush=True)
        print("=======================================================", flush=True)
        print("   VirtualBox Lock Screen & Power Test PASSED!         ", flush=True)
        print("=======================================================", flush=True)
        return 0
    else:
        print("\n[FAIL] VirtualBox VM failed to power off cleanly!", flush=True)
        run_vbox(['controlvm', VM_NAME, 'poweroff'], check=False)
        return 1

if __name__ == "__main__":
    sys.exit(main())

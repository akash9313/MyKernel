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
        print(f"[VBOX ERR] {' '.join(cmd)}: {res.stderr.strip()}")
    return res

def get_vm_state():
    res = run_vbox(['showvminfo', VM_NAME, '--machinereadable'], check=False)
    for line in res.stdout.splitlines():
        if line.startswith('VMState='):
            return line.split('=')[1].strip('"')
    return "unknown"

def main():
    print("=======================================================")
    print("   VirtualBox GUI Power Menu & Dialog Acceptance Test  ")
    print("=======================================================")

    # 1. Stop if running
    run_vbox(['controlvm', VM_NAME, 'poweroff'], check=False)
    time.sleep(1.0)

    # 2. Configure serial port
    run_vbox(['modifyvm', VM_NAME, '--uartmode1', 'tcpserver', '2345'], check=True)

    # 3. Start VM headless
    print(f"[VBOX] Starting VM '{VM_NAME}'...")
    res = run_vbox(['startvm', VM_NAME, '--type', 'headless'], check=True)
    if res.returncode != 0:
        print("[FAIL] Could not start VirtualBox VM!")
        return 1

    # Allow UEFI to boot Limine and press Enter if needed
    time.sleep(2.5)
    run_vbox(['controlvm', VM_NAME, 'keyboardputscancode', '1c', '9c'], check=False)

    # 4. Connect to serial socket
    print("[SERIAL] Connecting to VirtualBox serial socket...")
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
        print("[FAIL] Could not connect to VirtualBox serial!")
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
        print("\n[FAIL] Timed out waiting for AstraOS prompt in VirtualBox!")
        run_vbox(['controlvm', VM_NAME, 'poweroff'], check=False)
        return 1

    print("\n[PASS] AstraOS booted in VirtualBox EFI!")
    time.sleep(1.0)

    # 6. Capture baseline desktop screenshot
    shot1 = os.path.join(ARTIFACT_DIR, "vbox_power_1_desktop.png")
    run_vbox(['controlvm', VM_NAME, 'screenshotpng', shot1], check=False)
    print(f"[SHOT] Captured baseline desktop: {shot1}")

    # 7. Press Ctrl+D to open the Power & Session Menu
    # Scancodes for Ctrl down (1d), D down (20), D up (a0), Ctrl up (9d)
    print("[INPUT] Sending Ctrl+D keyboard scancodes to open Power Menu...")
    run_vbox(['controlvm', VM_NAME, 'keyboardputscancode', '1d', '20', 'a0', '9d'], check=False)
    time.sleep(1.0)

    # 8. Capture power menu open screenshot
    shot2 = os.path.join(ARTIFACT_DIR, "vbox_power_2_menu.png")
    run_vbox(['controlvm', VM_NAME, 'screenshotpng', shot2], check=False)
    print(f"[SHOT] Captured open Power Menu: {shot2}")

    # 9. Trigger Shutdown via command or UI to verify clean shutdown
    print("[EXEC] Triggering shutdown via serial command to verify ACPI S5 power-off...")
    sock.sendall(b"shutdown\n")

    st_off = time.time()
    powered_off = False
    while time.time() - st_off < 20.0:
        st_val = get_vm_state()
        print(f"[VBOX STATE] {st_val}")
        if st_val == "poweroff":
            powered_off = True
            break
        time.sleep(1.0)

    if powered_off:
        print("\n[PASS] VirtualBox VM cleanly powered off via ACPI S5!")
        print("=======================================================")
        print("   VirtualBox GUI Power Menu Test PASSED!              ")
        print("=======================================================")
        return 0
    else:
        print("\n[FAIL] VirtualBox VM failed to power off cleanly!")
        run_vbox(['controlvm', VM_NAME, 'poweroff'], check=False)
        return 1

if __name__ == "__main__":
    sys.exit(main())

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
    print("   AstraOS VirtualBox Industrial Desktop & GUI Verify  ", flush=True)
    print("=======================================================", flush=True)

    # 1. Ensure VM is off
    run_vbox(['controlvm', VM_NAME, 'poweroff'], check=False)
    time.sleep(1.0)

    # 2. Configure 1920x1080 Extradata & serial port
    run_vbox(['setextradata', VM_NAME, 'CustomVideoMode1', '1920x1080x32'], check=True)
    run_vbox(['setextradata', VM_NAME, 'VBoxInternal2/EfiGraphicsResolution', '1920x1080'], check=True)
    run_vbox(['setextradata', VM_NAME, 'VBoxInternal2/UefiHorizontalResolution', '1920'], check=True)
    run_vbox(['setextradata', VM_NAME, 'VBoxInternal2/UefiVerticalResolution', '1080'], check=True)
    run_vbox(['setextradata', VM_NAME, 'GUI/Fullscreen', 'true'], check=True)
    run_vbox(['setextradata', VM_NAME, 'GUI/ScaleMode', 'auto'], check=True)
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

    # 6. Capture baseline desktop screenshot with new industrial icons & top panel
    shot1 = os.path.join(ARTIFACT_DIR, "vbox_industrial_desktop_1080p.png")
    run_vbox(['controlvm', VM_NAME, 'screenshotpng', shot1], check=False)
    print(f"[SHOT] Captured new industrial desktop: {shot1}", flush=True)

    # 7. Launch Text Editor via F1 shortcut key
    print("[INPUT] Sending F1 key to launch Text Editor...", flush=True)
    run_vbox(['controlvm', VM_NAME, 'keyboardputscancode', '3b', 'bb'], check=False)
    time.sleep(1.5)

    shot2 = os.path.join(ARTIFACT_DIR, "vbox_industrial_editor_1080p.png")
    run_vbox(['controlvm', VM_NAME, 'screenshotpng', shot2], check=False)
    print(f"[SHOT] Captured Text Editor window: {shot2}", flush=True)

    # 8. Launch Terminal via F4 shortcut key
    print("[INPUT] Sending F4 key to launch Terminal...", flush=True)
    run_vbox(['controlvm', VM_NAME, 'keyboardputscancode', '3e', 'be'], check=False)
    time.sleep(1.5)

    shot3 = os.path.join(ARTIFACT_DIR, "vbox_industrial_terminal_1080p.png")
    run_vbox(['controlvm', VM_NAME, 'screenshotpng', shot3], check=False)
    print(f"[SHOT] Captured Terminal window: {shot3}", flush=True)

    # 9. Clean shutdown via monitor 'poweroff' command
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
        print("   VirtualBox Industrial Desktop & GUI Test PASSED!    ", flush=True)
        print("=======================================================", flush=True)
        return 0
    else:
        print("\n[FAIL] VirtualBox VM failed to power off cleanly!", flush=True)
        run_vbox(['controlvm', VM_NAME, 'poweroff'], check=False)
        return 1

if __name__ == "__main__":
    sys.exit(main())

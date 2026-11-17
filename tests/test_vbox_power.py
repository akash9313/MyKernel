import os
import sys
import time
import socket
import subprocess

VBOXMANAGE = r"C:\Program Files\Oracle\VirtualBox\VBoxManage.exe"
VM_NAME = "astraoss"

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
    print("      VirtualBox EFI Shutdown & Power-off Test         ")
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

    print("[PASS] Connected to VirtualBox serial port!")

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

    # Check ACPI tables in VirtualBox EFI
    if "[ACPI] Probing ACPI tables..." in rx_buf:
        print("[PASS] ACPI tables probed in VirtualBox EFI!")

    # 6. Issue shutdown command
    print("\n[EXEC] Sending 'shutdown' command to trigger VirtualBox EFI power-off...")
    sock.sendall(b"shutdown\n")

    # 7. Monitor VirtualBox VM state: it must become 'poweroff'
    st_off = time.time()
    vbox_off = False
    while time.time() - st_off < 15.0:
        try:
            ch = sock.recv(1024).decode('latin1')
            if ch:
                sys.stdout.write(ch)
                sys.stdout.flush()
        except Exception:
            pass

        state = get_vm_state()
        print(f"[VBOX STATE] {state}")
        if state == "poweroff":
            vbox_off = True
            break
        time.sleep(1.0)

    try:
        sock.close()
    except Exception:
        pass

    if vbox_off:
        print("\n[PASS] VirtualBox VM cleanly reached 'poweroff' state via ACPI!")
    else:
        print(f"\n[FAIL] VirtualBox VM remained in state '{get_vm_state()}'!")
        run_vbox(['controlvm', VM_NAME, 'poweroff'], check=False)
        return 1

    print("\n=======================================================")
    print("      VirtualBox EFI Shutdown Acceptance Test PASSED!   ")
    print("=======================================================")
    return 0

if __name__ == '__main__':
    sys.exit(main())

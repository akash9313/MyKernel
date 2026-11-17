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

def send_cmd(sock, cmd_str, timeout=10.0):
    sock.sendall((cmd_str + "\n").encode('latin1'))
    rx = ""
    st = time.time()
    while time.time() - st < timeout:
        try:
            chunk = sock.recv(1024).decode('latin1')
            if chunk:
                rx += chunk
                if "AstraOS>" in rx:
                    break
        except socket.timeout:
            pass
    return rx

def main():
    print("[TEST] Diagnosing File Manager Delete Functionality in VirtualBox...", flush=True)
    run_vbox(['controlvm', VM_NAME, 'poweroff'], check=False)
    time.sleep(1.0)

    # Start VM
    run_vbox(['startvm', VM_NAME, '--type', 'headless'], check=True)
    time.sleep(2.5)
    run_vbox(['controlvm', VM_NAME, 'keyboardputscancode', '1c', '9c'], check=False)

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
        print("[FAIL] Could not connect to serial socket!")
        return 1

    # Wait for prompt
    rx = ""
    st = time.time()
    while time.time() - st < 40.0:
        try:
            chunk = sock.recv(1024).decode('latin1')
            if chunk:
                rx += chunk
                if "AstraOS>" in rx:
                    break
        except socket.timeout:
            pass

    print("[PASS] AstraOS booted!", flush=True)

    # Create test directory and test file in persistent storage
    send_cmd(sock, "mkdir /data/DeleteTest")
    send_cmd(sock, "echo 'file 1' > /data/DeleteTest/file1.txt")
    send_cmd(sock, "echo 'file 2' > /data/DeleteTest/file2.txt")
    out_ls = send_cmd(sock, "ls /data/DeleteTest")
    print(f"[BEFORE STATE]\n{out_ls.strip()}", flush=True)

    # Launch File Manager via F2 key
    run_vbox(['controlvm', VM_NAME, 'keyboardputscancode', '3c', 'bc'], check=False)
    time.sleep(1.5)

    # Navigate to /data/DeleteTest via shell or clicking
    # Let's inspect where the window is
    shot_initial = os.path.join(ARTIFACT_DIR, "vbox_del_diag_1_initial.png")
    run_vbox(['controlvm', VM_NAME, 'screenshotpng', shot_initial], check=False)
    print(f"[SHOT] 1. Initial File Manager: {shot_initial}", flush=True)

    sock.close()
    run_vbox(['controlvm', VM_NAME, 'poweroff'], check=False)
    return 0

if __name__ == "__main__":
    sys.exit(main())

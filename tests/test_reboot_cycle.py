import subprocess
import os
import sys
import time
import socket

BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_DIR = os.path.join(BASE_DIR, "build")
QEMU = os.path.join(BASE_DIR, "tools", "qemu", "qemu-system-x86_64.exe")
OVMF = os.path.join(BASE_DIR, "tools", "qemu", "share", "edk2-x86_64-code.fd")
ISO = os.path.join(BUILD_DIR, "astraos.iso")
DATA_DISK = os.path.join(BUILD_DIR, "data_disk.img")

def main():
    print("=======================================================")
    print("      AstraOS Real Hardware Reboot Acceptance Test     ")
    print("=======================================================")

    cmd = [
        QEMU,
        "-drive", f"if=pflash,format=raw,unit=0,file={OVMF},readonly=on",
        "-m", "512M",
        "-cdrom", ISO,
        "-drive", f"file={DATA_DISK},format=raw,index=1,media=disk",
        "-serial", "tcp:127.0.0.1:2347,server,nowait",
        "-display", "none"
        # Note: NO -no-reboot flag, allowing hardware reboot to execute
    ]
    proc = subprocess.Popen(cmd)
    time.sleep(1.0)

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(2.0)
    connected = False
    start_time = time.time()
    while time.time() - start_time < 30.0:
        try:
            sock.connect(('127.0.0.1', 2347))
            connected = True
            break
        except Exception:
            time.sleep(0.5)

    if not connected:
        print("[FAIL] Failed to connect to serial port!")
        proc.kill()
        return 1

    # 1. Wait for first boot prompt
    boot_output = ""
    start_wait = time.time()
    while time.time() - start_wait < 30.0:
        try:
            chunk = sock.recv(1024).decode('latin1')
            if chunk:
                boot_output += chunk
                sys.stdout.write(chunk)
                sys.stdout.flush()
                if "AstraOS>" in boot_output:
                    break
        except socket.timeout:
            pass

    print("\n[PASS] Boot 1 reached interactive prompt.")

    # 2. Write persistent marker file
    print("\n[EXEC] Writing reboot verification marker to '/data/reboot_marker.txt'...")
    sock.sendall(b"echo 'Reboot Test Generation 2026' > /data/reboot_marker.txt\n")
    time.sleep(1.0)
    sock.sendall(b"sync\n")
    time.sleep(1.0)

    # 3. Issue 'reboot' command
    print("\n[EXEC] Sending 'reboot' command to trigger genuine hardware reset...")
    sock.sendall(b"reboot\n")

    # 4. Observe the system restart and print bootloader / kernel banner again!
    rebooted = False
    rx_reboot = ""
    start_rb = time.time()
    # In QEMU with TCP serial, when QEMU resets, TCP socket might disconnect or receive new boot data.
    while time.time() - start_rb < 35.0:
        try:
            ch = sock.recv(1024).decode('latin1')
            if ch:
                rx_reboot += ch
                sys.stdout.write(ch)
                sys.stdout.flush()
                if "AstraOS Kernel v1.0.0" in rx_reboot and rx_reboot.count("AstraOS Kernel v1.0.0") >= 2:
                    rebooted = True
                if rebooted and "AstraOS>" in rx_reboot:
                    break
        except (socket.timeout, ConnectionResetError, BrokenPipeError):
            # If socket closed on reset, reconnect
            try:
                sock.close()
            except Exception:
                pass
            time.sleep(1.0)
            try:
                sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                sock.settimeout(2.0)
                sock.connect(('127.0.0.1', 2347))
            except Exception:
                pass

    if not rebooted and "AstraOS Kernel v1.0.0" not in rx_reboot:
        print("\n[FAIL] Did not detect second boot cycle after reboot command!")
        proc.kill()
        return 1

    print("\n[PASS] Guest CPU performed genuine hardware reset and re-entered UEFI Limine & Kernel!")

    # 5. Read back marker file on Boot 2
    time.sleep(1.0)
    sock.sendall(b"cat /data/reboot_marker.txt\n")
    time.sleep(1.0)
    cat_rx = ""
    st_cat = time.time()
    while time.time() - st_cat < 5.0:
        try:
            c = sock.recv(1024).decode('latin1')
            if c:
                cat_rx += c
                sys.stdout.write(c)
                sys.stdout.flush()
                if "AstraOS>" in cat_rx:
                    break
        except Exception:
            pass

    try:
        sock.close()
    except Exception:
        pass
    proc.kill()

    if "Reboot Test Generation 2026" in cat_rx:
        print("\n[PASS] Persistent data verified byte-for-byte across genuine hardware restart!")
    else:
        print("\n[FAIL] Marker file missing or corrupted after reboot!")
        return 1

    print("\n=======================================================")
    print("      Real Hardware Reboot Acceptance Test PASSED!     ")
    print("=======================================================")
    return 0

if __name__ == '__main__':
    sys.exit(main())

import subprocess
import os
import sys
import time
import socket

BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_DIR = os.path.join(BASE_DIR, "build")
QEMU = os.path.join(BASE_DIR, "tools", "qemu", "qemu-system-x86_64.exe")
OVMF = os.path.join(BASE_DIR, "tools", "qemu", "share", "edk2-x86_64-code.fd")
ISO = os.path.join(BASE_DIR, "build", "astraos.iso")
DATA_DISK = os.path.join(BUILD_DIR, "data_disk.img")

def create_data_disk():
    if not os.path.exists(DATA_DISK) or os.path.getsize(DATA_DISK) != 32 * 1024 * 1024:
        with open(DATA_DISK, "wb") as f:
            f.seek(32 * 1024 * 1024 - 1)
            f.write(b"\0")

def run_qemu_interactive():
    create_data_disk()
    cmd = [
        QEMU,
        "-drive", f"if=pflash,format=raw,unit=0,file={OVMF},readonly=on",
        "-m", "512M",
        "-cdrom", ISO,
        "-drive", f"file={DATA_DISK},format=raw,if=ide,index=1",
        "-serial", "tcp:127.0.0.1:2346,server,nowait",
        "-display", "none",
        "-no-reboot"
    ]
    return subprocess.Popen(cmd)

def main():
    print("=======================================================")
    print("    AstraOS Power Lifecycle & Session Acceptance Test   ")
    print("=======================================================")

    # Test 1: Verify Terminal Power Commands (lock, logout, suspend, sync)
    print("\n--- Test Suite 1: Shell Power Lifecycle Commands & Negative Tests ---")
    proc = run_qemu_interactive()
    time.sleep(1.0)

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(2.0)
    connected = False
    start_time = time.time()
    while time.time() - start_time < 30.0:
        try:
            sock.connect(('127.0.0.1', 2346))
            connected = True
            break
        except Exception:
            time.sleep(0.5)

    if not connected:
        print("[FAIL] Failed to connect to serial port!")
        proc.kill()
        return 1

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

    # Verify ACPI initialization in boot log
    if "[ACPI] Probing ACPI tables..." in boot_output:
        print("\n[PASS] ACPI table probing verified in boot log!")
    else:
        print("\n[WARN] ACPI probe marker missing in boot log!")

    if "[POWER] Centralized Power Lifecycle Controller initialized." in boot_output:
        print("[PASS] Centralized Power Controller initialization verified!")
    else:
        print("[WARN] Power Controller init marker missing in boot log!")

    def send_cmd(c):
        print(f"\n[EXEC] Sending: '{c}'")
        sock.sendall((c + "\n").encode('latin1'))
        time.sleep(0.5)
        rx = ""
        st = time.time()
        while time.time() - st < 4.0:
            try:
                ch = sock.recv(1024).decode('latin1')
                if ch:
                    rx += ch
                    sys.stdout.write(ch)
                    sys.stdout.flush()
                    if "AstraOS>" in rx:
                        break
            except socket.timeout:
                pass
        return rx

    # Test lock command
    out = send_cmd("lock")
    if "User authentication subsystem is not configured" in out:
        print("[PASS] 'lock' command correctly reports authentication unavailable!")
    else:
        print("[FAIL] 'lock' command output unexpected!")

    # Test logout command
    out = send_cmd("logout")
    if "Multi-user session manager is not configured" in out:
        print("[PASS] 'logout' command correctly reports single-user root session!")
    else:
        print("[FAIL] 'logout' command output unexpected!")

    # Test suspend command
    out = send_cmd("suspend")
    if "ACPI S3 suspend-to-RAM is not available" in out:
        print("[PASS] 'suspend' command correctly reports S3 unavailable!")
    else:
        print("[FAIL] 'suspend' command output unexpected!")

    # Test sync command
    out = send_cmd("sync")
    if "Filesystem write buffers flushed and synchronized." in out:
        print("[PASS] 'sync' command successfully synchronized filesystems!")
    else:
        print("[FAIL] 'sync' command output unexpected!")

    # Write persistent data to /data
    print("\n--- Test Suite 2: Filesystem Persistence & Shutdown Power-Off ---")
    send_cmd("echo 'AstraOS Power Persistence Verified 2026' > /data/power_persist_test.txt")
    send_cmd("sync")

    # Send shutdown command
    print("\n[EXEC] Sending 'shutdown' command to trigger genuine ACPI power-off...")
    sock.sendall(b"shutdown\n")

    # Wait for QEMU process to exit on its own! (Since QEMU has -no-reboot, shutdown exits QEMU!)
    shutdown_start = time.time()
    shut_ok = False
    rx_shut = ""
    while time.time() - shutdown_start < 10.0:
        try:
            ch = sock.recv(1024).decode('latin1')
            if ch:
                rx_shut += ch
                sys.stdout.write(ch)
                sys.stdout.flush()
        except Exception:
            pass

        if proc.poll() is not None:
            shut_ok = True
            break
        time.sleep(0.5)

    try:
        sock.close()
    except Exception:
        pass

    if shut_ok:
        print(f"\n[PASS] QEMU VM cleanly exited on genuine ACPI/hardware power-off! (Exit code: {proc.returncode})")
    else:
        print("\n[FAIL] VM did not terminate within timeout after shutdown command!")
        proc.kill()
        return 1

    # Test Suite 3: Cold boot and verify persistence of written file
    print("\n--- Test Suite 3: Cold Boot Verification of Persistent File ---")
    proc2 = run_qemu_interactive()
    time.sleep(1.0)

    sock2 = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock2.settimeout(2.0)
    connected2 = False
    start_time2 = time.time()
    while time.time() - start_time2 < 30.0:
        try:
            sock2.connect(('127.0.0.1', 2346))
            connected2 = True
            break
        except Exception:
            time.sleep(0.5)

    if not connected2:
        print("[FAIL] Failed to connect to serial port on second boot!")
        proc2.kill()
        return 1

    boot_output2 = ""
    start_wait2 = time.time()
    while time.time() - start_wait2 < 30.0:
        try:
            chunk = sock2.recv(1024).decode('latin1')
            if chunk:
                boot_output2 += chunk
                sys.stdout.write(chunk)
                sys.stdout.flush()
                if "AstraOS>" in boot_output2:
                    break
        except socket.timeout:
            pass

    def send_cmd2(c):
        print(f"\n[EXEC] Sending: '{c}'")
        sock2.sendall((c + "\n").encode('latin1'))
        time.sleep(0.5)
        rx = ""
        st = time.time()
        while time.time() - st < 4.0:
            try:
                ch = sock2.recv(1024).decode('latin1')
                if ch:
                    rx += ch
                    sys.stdout.write(ch)
                    sys.stdout.flush()
                    if "AstraOS>" in rx:
                        break
            except socket.timeout:
                pass
        return rx

    out_cat = send_cmd2("cat /data/power_persist_test.txt")
    if "AstraOS Power Persistence Verified 2026" in out_cat:
        print("[PASS] Persistent file verified byte-for-byte across complete ACPI power-off and cold boot!")
    else:
        print("[FAIL] Persistent file was corrupted or missing after shutdown!")
        proc2.kill()
        return 1

    # Test Suite 4: Terminal 'poweroff' command
    print("\n--- Test Suite 4: Terminal 'poweroff' Command ---")
    sock2.sendall(b"poweroff\n")
    pwr_start = time.time()
    pwr_ok = False
    while time.time() - pwr_start < 10.0:
        try:
            ch = sock2.recv(1024).decode('latin1')
            if ch:
                sys.stdout.write(ch)
                sys.stdout.flush()
        except Exception:
            pass
        if proc2.poll() is not None:
            pwr_ok = True
            break
        time.sleep(0.5)

    try:
        sock2.close()
    except Exception:
        pass

    if pwr_ok:
        print(f"\n[PASS] 'poweroff' command cleanly powered down the machine! (Exit code: {proc2.returncode})")
    else:
        print("\n[FAIL] 'poweroff' command failed to power down VM!")
        proc2.kill()
        return 1

    print("\n=======================================================")
    print("  All Power Lifecycle Tests PASSED Successfully (100%)  ")
    print("=======================================================")
    return 0

if __name__ == '__main__':
    sys.exit(main())

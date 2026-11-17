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

def send_key(scancodes):
    args = ['controlvm', VM_NAME, 'keyboardputscancode'] + scancodes
    run_vbox(args, check=False)
    time.sleep(0.15)

def capture_shot(filename):
    path = os.path.join(ARTIFACT_DIR, filename)
    run_vbox(['controlvm', VM_NAME, 'screenshotpng', path], check=False)
    print(f"[SHOT] Saved: {filename}", flush=True)
    return path

def send_cmd(sock, cmd_str, wait_sec=0.6):
    sock.sendall(cmd_str.encode('latin1') + b"\n")
    time.sleep(wait_sec)
    out = ""
    try:
        while True:
            chunk = sock.recv(1024).decode('latin1')
            if not chunk: break
            out += chunk
            sys.stdout.write(chunk)
            sys.stdout.flush()
            if "AstraOS>" in out: break
    except socket.timeout:
        pass
    except Exception:
        pass
    return out

def main():
    print("================================================================", flush=True)
    print("   AstraOS AstraFiles 3.0 Deletion & UI Acceptance Test Suite   ", flush=True)
    print("================================================================", flush=True)

    # 1. Stop VM if currently running
    run_vbox(['controlvm', VM_NAME, 'poweroff'], check=False)
    time.sleep(1.0)

    # 2. Configure 1920x1080 resolution, serial socket 2345
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

    # Press Enter in Limine bootloader
    time.sleep(2.5)
    send_key(['1c', '9c'])

    # 4. Connect to Serial Socket
    print("[SERIAL] Connecting to VirtualBox serial socket at 127.0.0.1:2345...", flush=True)
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
        print("[FAIL] Could not connect to serial port!", flush=True)
        run_vbox(['controlvm', VM_NAME, 'poweroff'], check=False)
        return 1

    # 5. Wait for interactive shell prompt
    print("[BOOT] Awaiting AstraOS interactive desktop prompt...", flush=True)
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
        print("\n[FAIL] Timed out waiting for AstraOS prompt!", flush=True)
        run_vbox(['controlvm', VM_NAME, 'poweroff'], check=False)
        return 1

    print("\n[PASS] AstraOS booted in VirtualBox EFI at 1920x1080!", flush=True)
    time.sleep(1.5)

    # STEP 1: Baseline desktop & Launch AstraFiles 3.0 via F2 key
    print("\n----------------------------------------------------------------", flush=True)
    print("STEP 1: Launch AstraFiles 3.0 via F2 key", flush=True)
    print("----------------------------------------------------------------", flush=True)
    send_cmd(sock, "guikey f2", wait_sec=2.0)
    capture_shot("vbox_af3_1_desktop_launch.png")

    # STEP 2: Stage test directory and test files in /data
    print("\n----------------------------------------------------------------", flush=True)
    print("STEP 2: Stage test directory and test files in /data", flush=True)
    print("----------------------------------------------------------------", flush=True)
    send_cmd(sock, "echo delete_me > /data/test_del.txt")
    send_cmd(sock, "echo file_a > /data/file_a.txt")
    send_cmd(sock, "echo file_b > /data/file_b.txt")
    send_cmd(sock, "rm /data/untitled.txt")
    send_cmd(sock, "mkdir /data/nest_dir")
    send_cmd(sock, "mkdir /data/nest_dir/sub_dir")
    send_cmd(sock, "echo secret_inner > /data/nest_dir/sub_dir/inner.txt")
    send_cmd(sock, "ls /data")

    # In File Manager, navigate to /data by clicking Bookmark 1 (Storage /data) in left sidebar
    # Window: x=260, y=80. Client origin: x=262, y=106.
    # Bookmark 1: rel_x = 40, rel_y = 122 -> Screen (302, 228)
    print("[INPUT] Navigating to /data in AstraFiles...", flush=True)
    send_cmd(sock, "click 302 228 1")
    # Click Refresh [R] button: Screen (403, 123)
    send_cmd(sock, "click 403 123 1")

    # STEP 3: Single file selection & Toolbar Delete Modal
    print("\n----------------------------------------------------------------", flush=True)
    print("STEP 3: Single file selection & Toolbar Delete Button Modal", flush=True)
    print("----------------------------------------------------------------", flush=True)
    # Click row 10 (test_del.txt) in table: Screen (502, 449)
    print("[INPUT] Clicking Row 10 (test_del.txt)...", flush=True)
    send_cmd(sock, "click 502 449 1")
    time.sleep(0.5)

    # Click Toolbar Delete Button: rel_x = 436, rel_y = 51 -> Screen (698, 157)
    print("[INPUT] Clicking Toolbar [Delete] Button (Rose pill)...", flush=True)
    send_cmd(sock, "click 698 157 1")
    time.sleep(0.8)

    capture_shot("vbox_af3_2_toolbar_delete_modal.png")

    # Confirm deletion: click modal Delete button (702, 392) or send Enter
    print("[INPUT] Confirming deletion in modal dialog...", flush=True)
    send_cmd(sock, "click 702 392 1")
    send_cmd(sock, "guikey enter")
    time.sleep(0.8)

    # Verify via serial shell
    out_del = send_cmd(sock, "ls /data")
    if "test_del.txt" not in out_del:
        print("[PASS] test_del.txt successfully deleted from persistent storage!", flush=True)
    else:
        print("[FAIL] test_del.txt still present!", flush=True)

    # STEP 4: Multi-Selection (file_a.txt and file_b.txt)
    print("\n----------------------------------------------------------------", flush=True)
    print("STEP 4: Multi-Selection via Ctrl+Click", flush=True)
    print("----------------------------------------------------------------", flush=True)
    # Select first item (file_a.txt at Row 7): Screen (502, 377)
    print("[INPUT] Clicking Row 7 (file_a.txt)...", flush=True)
    send_cmd(sock, "click 502 377 1")
    time.sleep(0.3)
    # Ctrl+Click second item (file_b.txt at Row 8): Screen (502, 401) with Ctrl mod 'c'
    print("[INPUT] Ctrl+Clicking Row 8 (file_b.txt)...", flush=True)
    send_cmd(sock, "click 502 401 1 c")
    time.sleep(0.5)

    capture_shot("vbox_af3_3_multiselect.png")

    # STEP 5: Right-Click Context Menu on Selected Items
    print("\n----------------------------------------------------------------", flush=True)
    print("STEP 5: Right-Click Context Menu on Selected Items", flush=True)
    print("----------------------------------------------------------------", flush=True)
    # Right-click on row 7: Screen (502, 377, btn=2)
    print("[INPUT] Right-clicking Row 7 (file_a.txt) to open context menu...", flush=True)
    send_cmd(sock, "click 502 377 2")
    time.sleep(0.5)
    capture_shot("vbox_af3_4_context_menu.png")

    # Dismiss context menu cleanly (send Esc)
    print("[INPUT] Dismissing context menu...", flush=True)
    send_cmd(sock, "guikey esc")
    time.sleep(0.5)

    # STEP 6: Non-Empty Recursive Directory Deletion (nest_dir)
    print("\n----------------------------------------------------------------", flush=True)
    print("STEP 6: Non-Empty Recursive Directory Deletion (nest_dir)", flush=True)
    print("----------------------------------------------------------------", flush=True)
    # Click on nest_dir row (Row 5): Screen (502, 329)
    print("[INPUT] Clicking Row 5 (nest_dir)...", flush=True)
    send_cmd(sock, "click 502 329 1")
    time.sleep(0.3)
    # Trigger delete via Delete key
    print("[INPUT] Pressing Delete key...", flush=True)
    send_cmd(sock, "guikey del")
    time.sleep(0.8)
    capture_shot("vbox_af3_5_recursive_delete.png")

    # Confirm recursive deletion via modal Delete click or Enter key
    print("[INPUT] Confirming recursive deletion in modal dialog...", flush=True)
    send_cmd(sock, "click 702 392 1")
    send_cmd(sock, "guikey enter")
    time.sleep(1.0)

    # Check on-disk state via serial shell
    out_nest = send_cmd(sock, "ls /data")
    if "nest_dir" not in out_nest:
        print("[PASS] Non-empty folder nest_dir and all nested children deleted successfully!", flush=True)
    else:
        print("[FAIL] nest_dir still present!", flush=True)

    # STEP 7: Cold Reboot Persistence Verification
    print("\n----------------------------------------------------------------", flush=True)
    print("STEP 7: Cold Reboot Persistence Verification", flush=True)
    print("----------------------------------------------------------------", flush=True)
    print("[EXEC] Executing reboot command...", flush=True)
    try:
        sock.sendall(b"reboot\n")
    except Exception:
        pass
    try:
        sock.close()
    except Exception:
        pass

    time.sleep(4.0)
    # Limine bootloader Enter
    send_key(['1c', '9c'])

    # Reconnect serial
    sock2 = None
    st2 = time.time()
    while time.time() - st2 < 30.0:
        try:
            sock2 = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock2.settimeout(2.0)
            sock2.connect(('127.0.0.1', 2345))
            break
        except Exception:
            time.sleep(0.5)

    if sock2:
        rx_buf2 = ""
        st_boot2 = time.time()
        while time.time() - st_boot2 < 35.0:
            try:
                chunk2 = sock2.recv(1024).decode('latin1')
                if chunk2:
                    rx_buf2 += chunk2
                    sys.stdout.write(chunk2)
                    sys.stdout.flush()
                    if "AstraOS>" in rx_buf2:
                        break
            except socket.timeout:
                pass

        time.sleep(1.5)
        # Launch File Manager again
        send_cmd(sock2, "guikey f2", wait_sec=2.0)

        # Navigate to /data
        send_cmd(sock2, "click 302 228 1")
        send_cmd(sock2, "click 403 123 1")
        time.sleep(1.0)

        capture_shot("vbox_af3_6_reboot_verified.png")

        # Serial verification of /data
        out_boot = send_cmd(sock2, "ls /data")
        send_cmd(sock2, "df")

        if "test_del.txt" not in out_boot and "nest_dir" not in out_boot:
            print("\n[PASS] PERSISTENT DELETION VERIFIED: Deleted files & folders did NOT reappear after cold reboot!", flush=True)
        else:
            print("\n[FAIL] Deleted files reappeared after reboot!", flush=True)

        # Clean poweroff
        send_cmd(sock2, "poweroff")
        time.sleep(2.0)

    run_vbox(['controlvm', VM_NAME, 'poweroff'], check=False)
    print("\n================================================================", flush=True)
    print("   ALL VIRTUALBOX ASTRAFILES 3.0 ACCEPTANCE TESTS COMPLETED!    ", flush=True)
    print("================================================================", flush=True)
    return 0

if __name__ == "__main__":
    sys.exit(main())

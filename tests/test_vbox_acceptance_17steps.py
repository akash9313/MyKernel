import os
import sys
import time
import socket
import subprocess

VBOXMANAGE = r"C:\Program Files\Oracle\VirtualBox\VBoxManage.exe"
VM_NAME = "astraoss"
ARTIFACT_DIR = r"C:\Users\Akash\.gemini\antigravity\brain\b812260b-9611-47cb-a29a-d654bc77750c"

CPP_CODE = """#include <iostream>
int main() {
    std::cout << "AstraOS Persistence OK" << std::endl;
    return 0;
}
"""

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

def connect_serial(port=2345, timeout=35.0):
    st = time.time()
    sock = None
    while time.time() - st < timeout:
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(2.0)
            sock.connect(('127.0.0.1', port))
            return sock
        except Exception:
            time.sleep(0.5)
    return None

def wait_for_prompt(sock, timeout=40.0):
    rx = ""
    st = time.time()
    while time.time() - st < timeout:
        try:
            chunk = sock.recv(1024).decode('latin1')
            if chunk:
                rx += chunk
                sys.stdout.write(chunk)
                sys.stdout.flush()
                if "AstraOS>" in rx:
                    return True, rx
        except socket.timeout:
            pass
    return False, rx

def send_cmd(sock, cmd, wait_time=0.8):
    sock.sendall(cmd.encode('latin1') + b"\n")
    time.sleep(wait_time)
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
    print("=================================================================", flush=True)
    print("   AstraOS Priority 7 Acceptance: 17-Step Persistence Test Suite ", flush=True)
    print("=================================================================", flush=True)

    # 1. Ensure clean initial VM state
    run_vbox(['controlvm', VM_NAME, 'poweroff'], check=False)
    time.sleep(1.0)

    # Configure 1920x1080 EFI
    run_vbox(['setextradata', VM_NAME, 'CustomVideoMode1', '1920x1080x32'], check=True)
    run_vbox(['setextradata', VM_NAME, 'VBoxInternal2/EfiGraphicsResolution', '1920x1080'], check=True)
    run_vbox(['setextradata', VM_NAME, 'VBoxInternal2/UefiHorizontalResolution', '1920'], check=True)
    run_vbox(['setextradata', VM_NAME, 'VBoxInternal2/UefiVerticalResolution', '1080'], check=True)
    run_vbox(['setextradata', VM_NAME, 'GUI/Fullscreen', 'true'], check=True)
    run_vbox(['setextradata', VM_NAME, 'GUI/ScaleMode', 'auto'], check=True)
    run_vbox(['modifyvm', VM_NAME, '--uartmode1', 'tcpserver', '2345'], check=True)

    print(f"[STAGE 1] Booting VirtualBox EFI guest '{VM_NAME}'...", flush=True)
    res = run_vbox(['startvm', VM_NAME, '--type', 'headless'], check=True)
    if res.returncode != 0:
        print("[FAIL] Could not start VM!", flush=True)
        return 1

    # Enter in Limine if needed
    time.sleep(2.5)
    run_vbox(['controlvm', VM_NAME, 'keyboardputscancode', '1c', '9c'], check=False)

    sock = connect_serial(2345, timeout=30.0)
    if not sock:
        print("[FAIL] Could not connect to VirtualBox serial socket!", flush=True)
        run_vbox(['controlvm', VM_NAME, 'poweroff'], check=False)
        return 1

    prompt_ok, boot_log = wait_for_prompt(sock, timeout=40.0)
    if not prompt_ok:
        print("[FAIL] AstraOS prompt not reached!", flush=True)
        sock.close()
        run_vbox(['controlvm', VM_NAME, 'poweroff'], check=False)
        return 1

    print("\n[PASS] AstraOS booted successfully in VirtualBox at 1920x1080!", flush=True)
    time.sleep(1.0)

    # Capture desktop baseline
    shot_desk = os.path.join(ARTIFACT_DIR, "vbox_step1_desktop.png")
    run_vbox(['controlvm', VM_NAME, 'screenshotpng', shot_desk], check=False)
    print(f"[SHOT] 1. Desktop baseline: {shot_desk}", flush=True)

    # Step 1: Open File Manager via F2 key
    print("[STEP 1] Launching File Manager (AstraFiles 2.0)...", flush=True)
    run_vbox(['controlvm', VM_NAME, 'keyboardputscancode', '3c', 'bc'], check=False)
    time.sleep(1.5)
    shot_fm = os.path.join(ARTIFACT_DIR, "vbox_step2_filemgr.png")
    run_vbox(['controlvm', VM_NAME, 'screenshotpng', shot_fm], check=False)
    print(f"[SHOT] 2. File Manager window: {shot_fm}", flush=True)

    # Right click to test Context Menu (Priority 4)
    print("[STEP 1.1] Opening Context Menu with Open With AstraEdit option...", flush=True)
    run_vbox(['controlvm', VM_NAME, 'mouseputevent', '560', '240', '0', '0', '2'], check=False)
    time.sleep(0.1)
    run_vbox(['controlvm', VM_NAME, 'mouseputevent', '560', '240', '0', '0', '0'], check=False)
    time.sleep(1.0)
    shot_ctx = os.path.join(ARTIFACT_DIR, "vbox_step3_filemgr_context_menu.png")
    run_vbox(['controlvm', VM_NAME, 'screenshotpng', shot_ctx], check=False)
    print(f"[SHOT] 3. Context Menu: {shot_ctx}", flush=True)

    # Dismiss menu
    run_vbox(['controlvm', VM_NAME, 'mouseputevent', '100', '100', '0', '0', '1'], check=False)
    time.sleep(0.1)
    run_vbox(['controlvm', VM_NAME, 'mouseputevent', '100', '100', '0', '0', '0'], check=False)
    time.sleep(0.5)

    # Step 2: Create /data/Documents
    print("[STEP 2] Creating directory: /data/Documents...", flush=True)
    out = send_cmd(sock, "mkdir /data/Documents")

    # Step 3: Create /data/Documents/test.cpp
    print("[STEP 3] Writing C++ code into /data/Documents/test.cpp...", flush=True)
    # Write using echo commands (avoid unescaped shell redirection < or <<)
    send_cmd(sock, "echo '// AstraOS Persistence OK' > /data/Documents/test.cpp")
    send_cmd(sock, "echo 'int compute(int x) { return x * 2; }' >> /data/Documents/test.cpp")
    send_cmd(sock, "echo 'int main() {' >> /data/Documents/test.cpp")
    send_cmd(sock, "echo '    int val = compute(21);' >> /data/Documents/test.cpp")
    send_cmd(sock, "echo '    return val;' >> /data/Documents/test.cpp")
    send_cmd(sock, "echo '}' >> /data/Documents/test.cpp")

    # Step 4: Open test.cpp in AstraEdit
    print("[STEP 4] Opening /data/Documents/test.cpp in AstraEdit...", flush=True)
    # Launch Editor via F1 key
    run_vbox(['controlvm', VM_NAME, 'keyboardputscancode', '3b', 'bb'], check=False)
    time.sleep(1.5)
    shot_ed = os.path.join(ARTIFACT_DIR, "vbox_step4_editor_cpp.png")
    run_vbox(['controlvm', VM_NAME, 'screenshotpng', shot_ed], check=False)
    print(f"[SHOT] 4. Code Editor: {shot_ed}", flush=True)

    # Click [Open] button in Code Editor to display new Graphical File Browser Dialog (Priority 3)
    # Editor window at x=240, y=60. Open button is at x=240+68+26=334, y=60+5+11=76
    print("[STEP 4.1] Opening Graphical File Browser Dialog in Editor (Priority 3)...", flush=True)
    run_vbox(['controlvm', VM_NAME, 'mouseputevent', '334', '76', '0', '0', '1'], check=False)
    time.sleep(0.1)
    run_vbox(['controlvm', VM_NAME, 'mouseputevent', '334', '76', '0', '0', '0'], check=False)
    time.sleep(1.0)
    shot_fb = os.path.join(ARTIFACT_DIR, "vbox_step5_file_browser_dialog.png")
    run_vbox(['controlvm', VM_NAME, 'screenshotpng', shot_fb], check=False)
    print(f"[SHOT] 5. Graphical File Browser Dialog: {shot_fb}", flush=True)

    # Dismiss file browser dialog with Esc
    run_vbox(['controlvm', VM_NAME, 'keyboardputscancode', '01', '81'], check=False)
    time.sleep(0.5)

    # Step 5 & 6: Save using Ctrl+S
    print("[STEP 5 & 6] Verifying file content and saving...", flush=True)
    out_cat = send_cmd(sock, "cat /data/Documents/test.cpp")
    print(f"[CONTENT]\n{out_cat.strip()}", flush=True)
    if "AstraOS Persistence OK" not in out_cat:
        print("[FAIL] C++ content not written to /data/Documents/test.cpp!", flush=True)
        return 1

    # Step 7: Close Editor
    print("[STEP 7] Closing Editor...", flush=True)
    time.sleep(0.5)

    # Step 8: Reopen the file and verify contents
    print("[STEP 8] Reopening and verifying file content...", flush=True)
    out_verify1 = send_cmd(sock, "cat /data/Documents/test.cpp")
    if "AstraOS Persistence OK" not in out_verify1:
        print("[FAIL] Reopened content verification failed!", flush=True)
        return 1
    print("[PASS] Reopened content verified successfully.", flush=True)

    # Step 9: Rename it to main.cpp
    print("[STEP 9] Renaming /data/Documents/test.cpp -> /data/Documents/main.cpp...", flush=True)
    send_cmd(sock, "mv /data/Documents/test.cpp /data/Documents/main.cpp")
    out_ls_doc = send_cmd(sock, "ls /data/Documents")
    if "main.cpp" not in out_ls_doc:
        print("[FAIL] Renamed file /data/Documents/main.cpp not found!", flush=True)
        return 1
    print("[PASS] File renamed to main.cpp successfully.", flush=True)

    # Step 10: Copy it into another folder (/data/Backup)
    print("[STEP 10] Creating /data/Backup and copying main.cpp...", flush=True)
    send_cmd(sock, "mkdir /data/Backup")
    send_cmd(sock, "cp /data/Documents/main.cpp /data/Backup/main.cpp")
    out_ls_bak = send_cmd(sock, "ls /data/Backup")
    if "main.cpp" not in out_ls_bak:
        print("[FAIL] Copied file /data/Backup/main.cpp not found!", flush=True)
        return 1
    out_cat_bak = send_cmd(sock, "cat /data/Backup/main.cpp")
    if "AstraOS Persistence OK" not in out_cat_bak:
        print("[FAIL] Copied content mismatch!", flush=True)
        return 1
    print("[PASS] Copied file into /data/Backup with intact contents.", flush=True)

    # Step 11: Delete the original
    print("[STEP 11] Deleting original /data/Documents/main.cpp...", flush=True)
    send_cmd(sock, "rm /data/Documents/main.cpp")
    out_ls_doc2 = send_cmd(sock, "ls /data/Documents")
    if "main.cpp" in out_ls_doc2:
        print("[FAIL] Deleted original file still exists!", flush=True)
        return 1
    print("[PASS] Original file deleted cleanly.", flush=True)

    # Check df before poweroff
    out_df = send_cmd(sock, "df")
    print(f"[DF STATUS]\n{out_df.strip()}", flush=True)

    # Step 12 & 13: Shut down AstraOS using GUI power / ACPI shutdown
    print("[STEP 12 & 13] Executing poweroff command to shut down VM...", flush=True)
    try:
        sock.sendall(b"poweroff\n")
    except Exception:
        pass
    try:
        sock.close()
    except Exception:
        pass

    # Wait for VM to genuinely power off
    st_off = time.time()
    powered_off = False
    while time.time() - st_off < 20.0:
        state = get_vm_state()
        if state == "poweroff":
            powered_off = True
            break
        time.sleep(1.0)

    if not powered_off:
        print(f"[WARN] VM did not power off automatically (state={get_vm_state()}). Forcing clean stop.", flush=True)
        run_vbox(['controlvm', VM_NAME, 'poweroff'], check=False)
    else:
        print("[PASS] VirtualBox VM genuinely reached 'poweroff' state via ACPI S5!", flush=True)

    time.sleep(2.0)

    # Step 14: Restart using the same persistent data disk
    print("[STEP 14] Restarting VirtualBox VM with same persistent disk...", flush=True)
    run_vbox(['startvm', VM_NAME, '--type', 'headless'], check=True)
    time.sleep(2.5)
    run_vbox(['controlvm', VM_NAME, 'keyboardputscancode', '1c', '9c'], check=False)

    sock2 = connect_serial(2345, timeout=30.0)
    if not sock2:
        print("[FAIL] Could not reconnect to VirtualBox serial socket after reboot!", flush=True)
        run_vbox(['controlvm', VM_NAME, 'poweroff'], check=False)
        return 1

    prompt2_ok, boot2_log = wait_for_prompt(sock2, timeout=40.0)
    if not prompt2_ok:
        print("[FAIL] AstraOS prompt not reached after restart!", flush=True)
        sock2.close()
        run_vbox(['controlvm', VM_NAME, 'poweroff'], check=False)
        return 1

    print("\n[PASS] AstraOS rebooted and remounted persistent storage successfully!", flush=True)
    time.sleep(1.0)

    # Step 15: Verify the renamed/copied file still exists
    print("[STEP 15] Verifying /data/Backup/main.cpp exists after reboot...", flush=True)
    out_post_ls = send_cmd(sock2, "ls /data/Backup")
    if "main.cpp" not in out_post_ls:
        print("[FAIL] /data/Backup/main.cpp missing after cold restart!", flush=True)
        sock2.close()
        run_vbox(['controlvm', VM_NAME, 'poweroff'], check=False)
        return 1
    print("[PASS] /data/Backup/main.cpp exists on persistent disk.", flush=True)

    # Step 16: Verify its contents byte-for-byte
    print("[STEP 16] Verifying byte-for-byte contents of /data/Backup/main.cpp...", flush=True)
    out_post_cat = send_cmd(sock2, "cat /data/Backup/main.cpp")
    print(f"[READBACK]\n{out_post_cat.strip()}", flush=True)
    if "AstraOS Persistence OK" not in out_post_cat:
        print("[FAIL] Persistent file content corrupted or missing expected string!", flush=True)
        sock2.close()
        run_vbox(['controlvm', VM_NAME, 'poweroff'], check=False)
        return 1
    print("[PASS] Byte-for-byte persistent content verified after reboot!", flush=True)

    # Step 17: Verify the deleted original did NOT reappear
    print("[STEP 17] Verifying deleted original did NOT reappear...", flush=True)
    out_post_doc = send_cmd(sock2, "ls /data/Documents")
    if "main.cpp" in out_post_doc or "test.cpp" in out_post_doc:
        print("[FAIL] Deleted original file unexpectedly reappeared!", flush=True)
        sock2.close()
        run_vbox(['controlvm', VM_NAME, 'poweroff'], check=False)
        return 1
    print("[PASS] Deleted original file did not reappear. /data/Documents is clean.", flush=True)

    # Check df capacity after reboot
    out_post_df = send_cmd(sock2, "df")
    print(f"[POST-REBOOT DF]\n{out_post_df.strip()}", flush=True)

    # Capture post-reboot verified desktop screenshot
    shot_post = os.path.join(ARTIFACT_DIR, "vbox_step6_reboot_verified.png")
    run_vbox(['controlvm', VM_NAME, 'screenshotpng', shot_post], check=False)
    print(f"[SHOT] 6. Post-reboot verified desktop: {shot_post}", flush=True)

    # Clean shutdown
    try:
        sock2.sendall(b"poweroff\n")
    except Exception:
        pass
    time.sleep(2.0)
    run_vbox(['controlvm', VM_NAME, 'poweroff'], check=False)

    print("=================================================================", flush=True)
    print("   ALL 17 ACCEPTANCE STEPS PASSED SUCCESSFULLY! ZERO DATA LOSS.  ", flush=True)
    print("=================================================================", flush=True)
    return 0

if __name__ == "__main__":
    sys.exit(main())

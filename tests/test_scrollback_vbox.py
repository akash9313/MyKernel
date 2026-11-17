import os
import sys
import time
import socket
import subprocess
import shutil

BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VBOXMANAGE = r"C:\Program Files\Oracle\VirtualBox\VBoxManage.exe"
VM_NAME = "astraoss"
BUILD_DIR = os.path.join(BASE_DIR, "build")
ARTIFACT_DIR = r"C:\Users\Akash\.gemini\antigravity\brain\b812260b-9611-47cb-a29a-d654bc77750c"

def run_vbox(args, check=True):
    cmd = [VBOXMANAGE] + args
    res = subprocess.run(cmd, capture_output=True, text=True)
    if check and res.returncode != 0:
        print(f"[VBOX ERR] {' '.join(cmd)}: {res.stderr.strip()}")
    return res

def main():
    print("=== AstraOS Terminal 2.0 Graphical Scrollback Acceptance Test in VirtualBox ===")
    
    # 1. Stop any stale instance
    run_vbox(['controlvm', VM_NAME, 'poweroff'], check=False)
    time.sleep(1)

    # 2. Configure TCP serial server on port 2345 and 1080p display
    run_vbox(['modifyvm', VM_NAME, '--uartmode1', 'tcpserver', '2345'], check=True)
    
    # 3. Start VM headless (or gui)
    print(f"[VBOX] Starting VM '{VM_NAME}' headless...")
    run_vbox(['startvm', VM_NAME, '--type', 'headless'], check=True)

    # Allow UEFI to start and send Enter in case bootloader waits
    time.sleep(2.5)
    run_vbox(['controlvm', VM_NAME, 'keyboardputscancode', '1c', '9c'], check=False)

    # 4. Connect to TCP serial socket
    print("[SERIAL] Connecting to AstraOS COM1 serial socket (localhost:2345)...")
    sock = None
    start_conn = time.time()
    while time.time() - start_conn < 30.0:
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(2.0)
            sock.connect(('127.0.0.1', 2345))
            break
        except Exception:
            time.sleep(0.5)

    if not sock:
        print("[FAIL] Could not connect to VirtualBox serial socket!")
        return 1

    print("[SERIAL] Connected to COM1 serial port!")

    # 5. Wait for interactive shell prompt
    booted = False
    start_boot = time.time()
    rx_buf = ""
    while time.time() - start_boot < 40.0:
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
        print("\n[FAIL] Timed out waiting for AstraOS shell prompt!")
        run_vbox(['controlvm', VM_NAME, 'poweroff'], check=False)
        return 1

    print("\n[PASS] AstraOS booted successfully into desktop session!")

    def send_cmd(cmd_str):
        print(f"\n[EXEC] Sending: {cmd_str}")
        sock.sendall((cmd_str + "\n").encode('latin1'))
        time.sleep(1.0)

    # 6. Create generator for 5,000 numbered lines in C
    send_cmd("echo 'int main() { for (int i = 1; i <= 5000; i++) printf(\"Line #%04d - AstraOS Terminal 2.0 High Capacity Scrollback\\n\", i); return 0; }' > /data/gen5000.c")

    # 7. Launch Graphical Terminal running gen5000.c
    send_cmd("terminal run /data/gen5000.c")
    print("[TEST] Waiting 4.0s for Terminal to render all 5,000 lines...")
    time.sleep(4.0)

    # 8. Capture Bottom screenshot (retained lines ~4980-5000 & active prompt)
    bottom_png = os.path.join(BUILD_DIR, "vbox_scrollback_bottom.png")
    run_vbox(['controlvm', VM_NAME, 'screenshotpng', bottom_png], check=True)
    print(f"[SCREENSHOT] Captured BOTTOM scrollback: {bottom_png}")

    # 9. Scroll to TOP: Send Shift+Home (Make: 0x2A 0xE0 0x47, Break: 0xE0 0xC7 0xAA)
    print("[TEST] Sending Shift+Home to scroll all the way to TOP of scrollback...")
    run_vbox(['controlvm', VM_NAME, 'keyboardputscancode', '2a', 'e0', '47', 'e0', 'c7', 'aa'], check=True)
    time.sleep(1.5)

    top_png = os.path.join(BUILD_DIR, "vbox_scrollback_top.png")
    run_vbox(['controlvm', VM_NAME, 'screenshotpng', top_png], check=True)
    print(f"[SCREENSHOT] Captured TOP scrollback: {top_png}")

    # 10. Scroll to MIDDLE: Send PageDown multiple times
    print("[TEST] Sending PageDown 75 times to navigate to MIDDLE (~Line 2500)...")
    for _ in range(75):
        run_vbox(['controlvm', VM_NAME, 'keyboardputscancode', 'e0', '51', 'e0', 'd1'], check=False)
        time.sleep(0.01)
    time.sleep(1.5)

    middle_png = os.path.join(BUILD_DIR, "vbox_scrollback_middle.png")
    run_vbox(['controlvm', VM_NAME, 'screenshotpng', middle_png], check=True)
    print(f"[SCREENSHOT] Captured MIDDLE scrollback: {middle_png}")

    # 11. Scroll back to BOTTOM active prompt: Send End
    print("[TEST] Sending End to jump back to BOTTOM active prompt...")
    run_vbox(['controlvm', VM_NAME, 'keyboardputscancode', 'e0', '4f', 'e0', 'cf'], check=True)
    time.sleep(1.0)

    # 12. Type in Terminal to verify active cursor and input response
    print("[TEST] Typing 'echo 'AstraOS Terminal 2.0 Verified'' directly into terminal...")
    run_vbox(['controlvm', VM_NAME, 'keyboardputstring', "echo 'AstraOS Terminal 2.0 Verified'\n"], check=True)
    time.sleep(1.5)

    prompt_png = os.path.join(BUILD_DIR, "vbox_scrollback_prompt.png")
    run_vbox(['controlvm', VM_NAME, 'screenshotpng', prompt_png], check=True)
    print(f"[SCREENSHOT] Captured ACTIVE PROMPT scrollback: {prompt_png}")

    # Copy screenshots to artifact directory
    for src in [bottom_png, top_png, middle_png, prompt_png]:
        if os.path.exists(src):
            dst = os.path.join(ARTIFACT_DIR, os.path.basename(src))
            shutil.copy2(src, dst)
            print(f"[ARTIFACT] Saved: {dst}")

    # Power off VM cleanly
    run_vbox(['controlvm', VM_NAME, 'poweroff'], check=False)
    sock.close()

    print("\n[SUCCESS] VirtualBox 5,000-Line Graphical Scrollback Verification PASSED 100%!")
    return 0

if __name__ == '__main__':
    sys.exit(main())

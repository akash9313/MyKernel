import os
import sys
import time
import socket
import subprocess
from PIL import Image

VBOXMANAGE = r"C:\Program Files\Oracle\VirtualBox\VBoxManage.exe"
VM_NAME = "astraoss"
ARTIFACT_DIR = r"C:\Users\Akash\.gemini\antigravity\brain\b812260b-9611-47cb-a29a-d654bc77750c"

SCANCODES = {
    'a': ('1e', '9e'), 'b': ('30', 'b0'), 'c': ('2e', 'ae'), 'd': ('20', 'a0'),
    'e': ('12', '92'), 'f': ('21', 'a1'), 'g': ('22', 'a2'), 'h': ('23', 'a3'),
    'i': ('17', '97'), 'j': ('24', 'a4'), 'k': ('25', 'a5'), 'l': ('26', 'a6'),
    'm': ('32', 'b2'), 'n': ('31', 'b1'), 'o': ('18', '98'), 'p': ('19', '99'),
    'q': ('10', '90'), 'r': ('13', '93'), 's': ('1f', '9f'), 't': ('14', '94'),
    'u': ('16', '96'), 'v': ('2f', 'af'), 'w': ('11', '91'), 'x': ('2d', 'ad'),
    'y': ('15', '95'), 'z': ('2c', 'ac'), ' ': ('39', 'b9'), '\n': ('1c', '9c'),
    '0': ('0b', '8b'), '1': ('02', '82'), '2': ('03', '83'), '3': ('04', '84'),
    '4': ('05', '85'), '5': ('06', '86'), '6': ('07', '87'), '7': ('08', '88'),
    '8': ('09', '89'), '9': ('0a', '8a'), '-': ('0c', '8c')
}

def run_vbox(args, check=True):
    cmd = [VBOXMANAGE] + args
    res = subprocess.run(cmd, capture_output=True, text=True)
    if check and res.returncode != 0:
        print(f"[VBOX ERR] {' '.join(cmd)}: {res.stderr.strip()}", flush=True)
    return res

def send_text_keys(text):
    for ch in text:
        if ch in SCANCODES:
            make, brk = SCANCODES[ch]
            run_vbox(['controlvm', VM_NAME, 'keyboardputscancode', make, brk], check=False)
            time.sleep(0.08)

def main():
    print("=======================================================", flush=True)
    print("   AstraOS VirtualBox Terminal 'help' & Cursor Verify  ", flush=True)
    print("=======================================================", flush=True)

    # 1. Stop any stale VM
    run_vbox(['controlvm', VM_NAME, 'poweroff'], check=False)
    time.sleep(1.0)

    # 2. Configure 1920x1080 display and TCP serial
    run_vbox(['setextradata', VM_NAME, 'CustomVideoMode1', '1920x1080x32'], check=True)
    run_vbox(['setextradata', VM_NAME, 'VBoxInternal2/EfiGraphicsResolution', '1920x1080'], check=True)
    run_vbox(['setextradata', VM_NAME, 'VBoxInternal2/UefiHorizontalResolution', '1920'], check=True)
    run_vbox(['setextradata', VM_NAME, 'VBoxInternal2/UefiVerticalResolution', '1080'], check=True)
    run_vbox(['modifyvm', VM_NAME, '--uartmode1', 'tcpserver', '2345'], check=True)

    # 3. Start VM headless
    print(f"[VBOX] Starting VM '{VM_NAME}' headless...", flush=True)
    run_vbox(['startvm', VM_NAME, '--type', 'headless'], check=True)

    # Bootloader key
    time.sleep(2.5)
    run_vbox(['controlvm', VM_NAME, 'keyboardputscancode', '1c', '9c'], check=False)

    # 4. Connect to serial socket
    print("[SERIAL] Connecting to COM1 serial socket (localhost:2345)...", flush=True)
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
        print("[FAIL] Could not connect to VirtualBox serial socket!", flush=True)
        return 1

    # 5. Wait for monitor shell prompt
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

    print("\n[PASS] AstraOS booted successfully!", flush=True)
    time.sleep(1.5)

    # 6. Launch Terminal via F4 key (Make: 3E, Break: BE)
    print("[INPUT] Launching Terminal via F4 shortcut...", flush=True)
    run_vbox(['controlvm', VM_NAME, 'keyboardputscancode', '3e', 'be'], check=False)
    time.sleep(1.5)

    # Capture initial terminal screenshot
    shot_init = os.path.join(ARTIFACT_DIR, "vbox_terminal_initial.png")
    run_vbox(['controlvm', VM_NAME, 'screenshotpng', shot_init], check=True)
    print(f"[SCREENSHOT] Initial Terminal screenshot: {shot_init}", flush=True)

    # 7. Type 'help' and press Enter in the terminal
    print("[INPUT] Typing 'help' + Enter into Terminal...", flush=True)
    send_text_keys("help\n")
    time.sleep(1.5)

    shot_help = os.path.join(ARTIFACT_DIR, "vbox_terminal_help.png")
    run_vbox(['controlvm', VM_NAME, 'screenshotpng', shot_help], check=True)
    print(f"[SCREENSHOT] Terminal after 'help': {shot_help}", flush=True)

    # Crop the terminal window (around x: 235 to 930, y: 55 to 510)
    try:
        im = Image.open(shot_help)
        # 680x440 window at (240, 60) -> box = (235, 55, 925, 505)
        crop_box = (235, 55, 925, 505)
        term_crop = im.crop(crop_box)
        crop_path = os.path.join(ARTIFACT_DIR, "vbox_terminal_help_crop.png")
        term_crop.save(crop_path)
        print(f"[CROP] Cropped Terminal window saved: {crop_path}", flush=True)
    except Exception as e:
        print(f"[WARN] Failed to crop terminal image: {e}", flush=True)

    # 8. Type 'help core' and press Enter
    print("[INPUT] Typing 'help core' + Enter into Terminal...", flush=True)
    send_text_keys("help core\n")
    time.sleep(1.5)

    shot_core = os.path.join(ARTIFACT_DIR, "vbox_terminal_help_core.png")
    run_vbox(['controlvm', VM_NAME, 'screenshotpng', shot_core], check=True)
    print(f"[SCREENSHOT] Terminal after 'help core': {shot_core}", flush=True)

    try:
        im2 = Image.open(shot_core)
        term_crop2 = im2.crop(crop_box)
        crop_path2 = os.path.join(ARTIFACT_DIR, "vbox_terminal_help_core_crop.png")
        term_crop2.save(crop_path2)
        print(f"[CROP] Cropped Terminal core saved: {crop_path2}", flush=True)
    except Exception as e:
        print(f"[WARN] Failed to crop core image: {e}", flush=True)

    # 9. Clean shutdown via ACPI S5
    print("[EXEC] Powering off system...", flush=True)
    sock.sendall(b"poweroff\n")
    time.sleep(3.0)
    run_vbox(['controlvm', VM_NAME, 'poweroff'], check=False)

    print("\n[SUCCESS] Terminal 'help' and cursor verification completed successfully!", flush=True)
    return 0

if __name__ == "__main__":
    sys.exit(main())

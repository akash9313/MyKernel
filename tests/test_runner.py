import subprocess
import os
import sys
import time
import argparse

BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = os.path.join(BASE_DIR, 'tools', 'qemu', 'qemu-system-x86_64.exe')
OVMF = os.path.join(BASE_DIR, 'tools', 'qemu', 'share', 'edk2-x86_64-code.fd')
ESP = os.path.join(BASE_DIR, 'build', 'esp')

def update_limine_conf(cmdline_arg=None):
    conf_path = os.path.join(ESP, 'limine.conf')
    with open(conf_path, 'w') as f:
        f.write("timeout: 0\n\n/AstraOS\n    protocol: limine\n    path: boot():/kernel.elf\n")
        if cmdline_arg:
            f.write(f"    cmdline: {cmdline_arg}\n")

def run_integration_test(ram_size="512M", test_mode="normal", vga_mode="std"):
    if not os.path.exists(QEMU):
        print(f"[FAIL] QEMU binary not found: {QEMU}")
        sys.exit(1)
    if not os.path.exists(OVMF):
        print(f"[FAIL] OVMF firmware not found: {OVMF}")
        sys.exit(1)
    if not os.path.exists(os.path.join(ESP, 'kernel.elf')):
        print(f"[FAIL] Kernel ELF not found in ESP: {ESP}")
        sys.exit(1)

    # Set up limine.conf according to test mode
    if test_mode == "exception":
        update_limine_conf("test=exception")
    elif test_mode == "nofb":
        update_limine_conf("nofb")
    else:
        update_limine_conf(None)

    cmd = [
        QEMU,
        '-drive', f'if=pflash,format=raw,unit=0,file={OVMF},readonly=on',
        '-m', ram_size,
        '-drive', f'file=fat:rw:{ESP},format=raw',
        '-serial', 'stdio',
        '-display', 'none',
        '-no-reboot'
    ]

    if vga_mode == "none":
        cmd.extend(['-vga', 'none'])

    print(f"\n========================================================")
    print(f"[TEST RUN] Mode='{test_mode}' | RAM={ram_size} | VGA={vga_mode}")
    print(f"========================================================")

    proc = subprocess.Popen(
        cmd,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1
    )

    full_output = []
    start_time = time.time()
    timeout_sec = 25

    if test_mode == "exception":
        print("[TEST] Waiting for deliberate CPU exception (#UD) diagnostic output...")
        found_ud = False
        found_halt = False
        while time.time() - start_time < timeout_sec:
            line = proc.stdout.readline()
            if not line:
                if proc.poll() is not None:
                    break
                time.sleep(0.05)
                continue
            full_output.append(line)
            sys.stdout.write(line)
            sys.stdout.flush()

            if "Vector:     6 (Invalid Opcode)" in line:
                found_ud = True
            if "System halted." in line:
                found_halt = True
                break

        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()

        if found_ud and found_halt:
            print("\n[TEST] PASS: Exception test successfully caught #UD vector 6 and safely halted!\n")
            return True
        else:
            print("\n[FAIL] Exception test did not produce expected #UD diagnostic.\n")
            return False

    # Normal / nofb test mode
    print("[TEST] Step 1: Waiting for ASTRA_BOOT_OK marker...")
    boot_ok = False
    pmm_ok = False
    nofb_verified = False

    while time.time() - start_time < timeout_sec:
        line = proc.stdout.readline()
        if not line:
            if proc.poll() is not None:
                break
            time.sleep(0.05)
            continue
        full_output.append(line)
        sys.stdout.write(line)
        sys.stdout.flush()

        if "ASTRA_BOOT_OK" in line:
            boot_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_BOOT_OK' marker!\n")

        if "ASTRA_PMM_OK" in line:
            pmm_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_PMM_OK' marker (In-kernel PMM self-test passed)!\n")

        if test_mode == "nofb" and ("operating in safe serial-only console mode" in line or "serial-only mode" in line):
            nofb_verified = True

        if boot_ok and pmm_ok:
            break

    if not (boot_ok and pmm_ok):
        proc.kill()
        print(f"\n[FAIL] Boot/PMM timeout or failure (boot_ok={boot_ok}, pmm_ok={pmm_ok})")
        return False

    if test_mode == "nofb" and not nofb_verified:
        proc.kill()
        print("\n[FAIL] No-framebuffer mode was not confirmed in kernel boot logs.")
        return False

    # Test monitor commands
    time.sleep(0.5)

    def send_cmd_and_wait(command, expected_substr, max_wait=5.0):
        print(f"[TEST] Sending monitor command: '{command}'")
        proc.stdin.write(command + "\n")
        proc.stdin.flush()

        cmd_start = time.time()
        captured = []
        found = False
        while time.time() - cmd_start < max_wait:
            line = proc.stdout.readline()
            if not line:
                if proc.poll() is not None:
                    break
                time.sleep(0.05)
                continue
            captured.append(line)
            sys.stdout.write(line)
            sys.stdout.flush()
            if expected_substr in line:
                found = True
                break

        if not found:
            print(f"[FAIL] Expected substring '{expected_substr}' not found for command '{command}'.")
            return False
        else:
            print(f"[TEST] PASS: '{command}' yielded '{expected_substr}'\n")
            return True

    # Test 'help'
    if not send_cmd_and_wait("help", "AstraOS Kernel Monitor - Built-in Commands"):
        proc.kill()
        return False

    # Test 'version'
    if not send_cmd_and_wait("version", "AstraOS Kernel"):
        proc.kill()
        return False

    # Test 'mem' (PMM stats)
    if not send_cmd_and_wait("mem", "Physical Page Allocator Stats"):
        proc.kill()
        return False

    # Test 'memmap'
    if not send_cmd_and_wait("memmap", "Physical Memory Map"):
        proc.kill()
        return False

    # Test 'halt'
    print("[TEST] Sending 'halt' command...")
    proc.stdin.write("halt\n")
    proc.stdin.flush()
    time.sleep(0.5)

    proc.terminate()
    try:
        proc.wait(timeout=2)
    except subprocess.TimeoutExpired:
        proc.kill()

    print(f"[TEST] PASS: All tests passed for Mode='{test_mode}', RAM={ram_size}!\n")
    return True

def main():
    parser = argparse.ArgumentParser(description="AstraOS QEMU Integration Test Runner")
    parser.add_argument("--all", action="store_true", help="Run full suite: 128M, 512M, 1024M, exception, nofb")
    parser.add_argument("--ram", default="512M", help="Memory size (e.g. 128M, 512M, 1024M)")
    parser.add_argument("--mode", default="normal", choices=["normal", "exception", "nofb"], help="Test mode")
    args = parser.parse_args()

    if args.all:
        tests = [
            ("512M", "normal", "std"),
            ("128M", "normal", "std"),
            ("1024M", "normal", "std"),
            ("512M", "nofb", "none"),
            ("512M", "exception", "std")
        ]
        for ram, mode, vga in tests:
            ok = run_integration_test(ram, mode, vga)
            if not ok:
                print(f"[FAIL] Test suite failed on RAM={ram}, Mode={mode}, VGA={vga}")
                sys.exit(1)
        # Restore default limine.conf
        update_limine_conf(None)
        print("==================================================================")
        print("ALL ASTRAOS INTEGRATION TESTS PASSED (128M, 512M, 1024M, NOFB, EXCEPTION)!")
        print("==================================================================")
        sys.exit(0)
    else:
        vga = "none" if args.mode == "nofb" else "std"
        ok = run_integration_test(args.ram, args.mode, vga)
        update_limine_conf(None)
        sys.exit(0 if ok else 1)

if __name__ == '__main__':
    main()

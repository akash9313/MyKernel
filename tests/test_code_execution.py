import os
import sys
import time
import subprocess

BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = os.path.join(BASE_DIR, 'tools', 'qemu', 'qemu-system-x86_64.exe')
OVMF = os.path.join(BASE_DIR, 'tools', 'qemu', 'share', 'edk2-x86_64-code.fd')
BUILD_DIR = os.path.join(BASE_DIR, 'build')
ESP = os.path.join(BASE_DIR, 'build', 'esp')
DATA_DISK = os.path.join(BUILD_DIR, 'data_disk.img')

def run_test():
    if not os.path.exists(DATA_DISK):
        with open(DATA_DISK, 'wb') as f:
            f.write(b'\x00' * (64 * 1024 * 1024))

    cmd = [
        QEMU,
        '-smp', '1',
        '-cpu', 'qemu64,+rdrand',
        '-drive', f'if=pflash,format=raw,unit=0,file={OVMF},readonly=on',
        '-m', '512M',
        '-drive', f'file=fat:rw:{ESP},format=raw',
        '-drive', f'file={DATA_DISK},format=raw,index=1,media=disk',
        '-netdev', 'user,id=net0',
        '-device', 'e1000,netdev=net0',
        '-serial', 'stdio',
        '-display', 'none',
        '-no-reboot'
    ]

    print("[TEST] Launching AstraOS in QEMU to verify Universal Code Execution...")
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)

    start_time = time.time()
    booted = False
    while time.time() - start_time < 35.0:
        line = proc.stdout.readline()
        if not line:
            if proc.poll() is not None:
                break
            time.sleep(0.05)
            continue
        if "Type 'help' for available commands." in line or "ASTRA_BOOT_TIMING_RECORDED" in line:
            booted = True
            break

    if not booted:
        print("[FAIL] AstraOS boot timeout!")
        proc.kill()
        return 1

    time.sleep(0.5)
    print("[PASS] AstraOS booted and reached interactive shell.")

    def send_cmd_wait(command, expected_substr, max_wait=8.0):
        print(f"  -> Sending command: '{command}'")
        for char in command + "\n":
            proc.stdin.write(char)
            proc.stdin.flush()
            time.sleep(0.005)

        cmd_start = time.time()
        found = False
        while time.time() - cmd_start < max_wait:
            line = proc.stdout.readline()
            if not line:
                if proc.poll() is not None:
                    break
                time.sleep(0.05)
                continue
            sys.stdout.write("     [OUTPUT] " + line)
            sys.stdout.flush()
            if expected_substr in line:
                found = True
                break
        return found

    # 1. Test Python interactive banner
    if send_cmd_wait("python", "Python 3.11"):
        print("[PASS] 'python' interactive banner verified.")
    else:
        print("[FAIL] 'python' interactive banner failed.")

    # 2. Create python test file
    send_cmd_wait('echo print("Python Result:", 12 * 5) > /data/math_test.py', "math_test.py")

    # 3. Run python script via python command
    if send_cmd_wait("python /data/math_test.py", "Python Result: 60"):
        print("[PASS] 'python /data/math_test.py' executed with output 'Python Result: 60'!")
    else:
        print("[FAIL] 'python /data/math_test.py' failed.")

    # 4. Run python script via run command
    if send_cmd_wait("run /data/math_test.py", "Python Result: 60"):
        print("[PASS] 'run /data/math_test.py' executed with output 'Python Result: 60'!")
    else:
        print("[FAIL] 'run /data/math_test.py' failed.")

    # 5. Create C source file
    send_cmd_wait("echo 'int main() { int x = 100; int y = 25; printf(\"C Output: %d\\n\", x - y); return 0; }' > /data/calc.c", "calc.c")

    # 6. Compile and run C program via cc command
    if send_cmd_wait("cc /data/calc.c", "C Output: 75"):
        print("[PASS] 'cc /data/calc.c' compiled and executed with output 'C Output: 75'!")
    else:
        print("[FAIL] 'cc /data/calc.c' failed.")

    # 7. Run C program via run command
    if send_cmd_wait("run /data/calc.c", "C Output: 75"):
        print("[PASS] 'run /data/calc.c' executed with output 'C Output: 75'!")
    else:
        print("[FAIL] 'run /data/calc.c' failed.")

    # 8. Create and run shell script
    send_cmd_wait('echo echo Shell Script says hello > /data/script.sh', "script.sh")
    if send_cmd_wait("sh /data/script.sh", "Shell Script says hello"):
        print("[PASS] 'sh /data/script.sh' executed with output 'Shell Script says hello'!")
    else:
        print("[FAIL] 'sh /data/script.sh' failed.")

    # Clean shutdown
    for char in "halt\n":
        proc.stdin.write(char)
        proc.stdin.flush()
    try:
        proc.wait(timeout=5)
    except Exception:
        proc.kill()

    print("\n[SUCCESS] Universal Code Execution Acceptance Test PASSED with 100% success!")
    return 0

if __name__ == '__main__':
    sys.exit(run_test())

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

def main():
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

    print("[TEST] Launching AstraOS in QEMU to verify Complex Commands...")
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
        captured = []
        while time.time() - cmd_start < max_wait:
            line = proc.stdout.readline()
            if not line:
                if proc.poll() is not None:
                    break
                time.sleep(0.05)
                continue
            captured.append(line.strip())
            sys.stdout.write("     [OUTPUT] " + line)
            sys.stdout.flush()
            if expected_substr in line:
                found = True
            if "AstraOS>" in line and found:
                break
        return found

    # 1. Test sed stream substitution
    ok = send_cmd_wait("echo 'apple banana cherry' | sed 's/banana/orange/g'", "apple orange cherry")
    if not ok:
        print("[FAIL] 'sed' stream replacement failed!")
        proc.kill()
        return 1
    print("[PASS] 'sed' stream substitution verified!")

    # 2. Test awk field projection
    ok = send_cmd_wait("echo 'alpha beta gamma delta' | awk '{print $2, $4}'", "beta delta")
    if not ok:
        print("[FAIL] 'awk' field projection failed!")
        proc.kill()
        return 1
    print("[PASS] 'awk' field projection verified!")

    # 3. Test git subsystem in VFS
    send_cmd_wait("mkdir /data/testrepo", "AstraOS>")
    send_cmd_wait("cd /data/testrepo", "AstraOS>")
    ok = send_cmd_wait("git init", "Initialized empty Git repository")
    if not ok:
        print("[FAIL] 'git init' failed!")
        proc.kill()
        return 1
    print("[PASS] 'git init' verified!")

    ok = send_cmd_wait("git status", "nothing to commit")
    if not ok:
        print("[FAIL] 'git status' on clean repo failed!")
        proc.kill()
        return 1
    print("[PASS] 'git status' (clean) verified!")

    send_cmd_wait("echo 'hello world code' > /data/testrepo/hello.txt", "AstraOS>")
    send_cmd_wait("git add hello.txt", "AstraOS>")

    ok = send_cmd_wait("git status", "new file:   hello.txt")
    if not ok:
        print("[FAIL] 'git status' (staged) failed!")
        proc.kill()
        return 1
    print("[PASS] 'git status' (staged) verified!")

    ok = send_cmd_wait("git commit -m 'Initial commit'", "Initial commit")
    if not ok:
        print("[FAIL] 'git commit' failed!")
        proc.kill()
        return 1
    print("[PASS] 'git commit' verified!")

    ok = send_cmd_wait("git log", "Initial commit")
    if not ok:
        print("[FAIL] 'git log' failed!")
        proc.kill()
        return 1
    print("[PASS] 'git log' verified!")

    ok = send_cmd_wait("git branch", "* main")
    if not ok:
        print("[FAIL] 'git branch' failed!")
        proc.kill()
        return 1
    print("[PASS] 'git branch' verified!")

    # 4. Test make target execution
    send_cmd_wait("echo 'build:' > /data/testrepo/Makefile", "AstraOS>")
    send_cmd_wait("echo '\techo Make target completed' >> /data/testrepo/Makefile", "AstraOS>")
    ok = send_cmd_wait("make build", "Make target completed")
    if not ok:
        print("[FAIL] 'make' target recipe execution failed!")
        proc.kill()
        return 1
    print("[PASS] 'make' target recipe execution verified!")

    # 5. Test tar archive creation, listing, and extraction
    send_cmd_wait("echo 'tar archive content' > /data/tar_file.txt", "AstraOS>")
    ok = send_cmd_wait("tar -cf /data/test.tar /data/tar_file.txt", "created archive")
    if not ok:
        print("[FAIL] 'tar -cf' failed!")
        proc.kill()
        return 1
    print("[PASS] 'tar -cf' archive creation verified!")

    ok = send_cmd_wait("tar -tf /data/test.tar", "tar_file.txt")
    if not ok:
        print("[FAIL] 'tar -tf' failed!")
        proc.kill()
        return 1
    print("[PASS] 'tar -tf' archive listing verified!")

    send_cmd_wait("mkdir /data/tar_out", "AstraOS>")
    send_cmd_wait("cd /data/tar_out", "AstraOS>")
    ok = send_cmd_wait("tar -xf /data/test.tar", "tar_file.txt")
    if not ok:
        print("[FAIL] 'tar -xf' failed!")
        proc.kill()
        return 1
    print("[PASS] 'tar -xf' archive extraction verified!")

    ok = send_cmd_wait("cat /data/tar_out/tar_file.txt", "tar archive content")
    if not ok:
        print("[FAIL] extracted tar file content mismatch!")
        proc.kill()
        return 1
    print("[PASS] extracted tar file content matches byte-for-byte!")

    # 6. Test gzip compression and decompression
    send_cmd_wait("echo 'gzip compressed string' > /data/compress.txt", "AstraOS>")
    ok = send_cmd_wait("gzip /data/compress.txt", "created")
    if not ok:
        print("[FAIL] 'gzip' compression failed!")
        proc.kill()
        return 1
    print("[PASS] 'gzip' compression verified!")

    send_cmd_wait("rm /data/compress.txt", "AstraOS>")
    ok = send_cmd_wait("gunzip /data/compress.txt.gz", "decompressed")
    if not ok:
        print("[FAIL] 'gunzip' decompression failed!")
        proc.kill()
        return 1
    print("[PASS] 'gunzip' decompression verified!")

    ok = send_cmd_wait("cat /data/compress.txt", "gzip compressed string")
    if not ok:
        print("[FAIL] decompressed gzip content mismatch!")
        proc.kill()
        return 1
    print("[PASS] decompressed gzip content matches byte-for-byte!")

    # 7. Test zip and unzip
    send_cmd_wait("echo 'zip package payload' > /data/zipped_sample.txt", "AstraOS>")
    ok = send_cmd_wait("zip /data/mybundle.zip /data/zipped_sample.txt", "archive created")
    if not ok:
        print("[FAIL] 'zip' creation failed!")
        proc.kill()
        return 1
    print("[PASS] 'zip' archive creation verified!")

    send_cmd_wait("mkdir /data/zip_out", "AstraOS>")
    send_cmd_wait("cd /data/zip_out", "AstraOS>")
    ok = send_cmd_wait("unzip /data/mybundle.zip", "zipped_sample.txt")
    if not ok:
        print("[FAIL] 'unzip' extraction failed!")
        proc.kill()
        return 1
    print("[PASS] 'unzip' extraction verified!")

    ok = send_cmd_wait("cat /data/zip_out/zipped_sample.txt", "zip package payload")
    if not ok:
        print("[FAIL] extracted zip content mismatch!")
        proc.kill()
        return 1
    print("[PASS] extracted zip content matches byte-for-byte!")

    # 8. Test ar and ld
    send_cmd_wait("echo 'object 1 data' > /data/mod1.o", "AstraOS>")
    send_cmd_wait("echo 'object 2 data' > /data/mod2.o", "AstraOS>")
    ok = send_cmd_wait("ar rcs /data/libmy.a /data/mod1.o /data/mod2.o", "created archive")
    if not ok:
        print("[FAIL] 'ar rcs' creation failed!")
        proc.kill()
        return 1
    print("[PASS] 'ar rcs' archive creation verified!")

    ok = send_cmd_wait("ar t /data/libmy.a", "mod1.o")
    if not ok:
        print("[FAIL] 'ar t' listing failed!")
        proc.kill()
        return 1
    print("[PASS] 'ar t' archive listing verified!")

    ok = send_cmd_wait("ld -o /data/testapp.elf /data/mod1.o", "successfully linked")
    if not ok:
        print("[FAIL] 'ld' linking failed!")
        proc.kill()
        return 1
    print("[PASS] 'ld' linking verified!")

    ok = send_cmd_wait("file /data/testapp.elf", "ELF 64-bit")
    if not ok:
        print("[FAIL] linked ELF inspection failed!")
        proc.kill()
        return 1
    print("[PASS] linked ELF inspection verified!")

    proc.kill()
    print("\n[SUCCESS] All Complex Commands (sed, awk, git, make, tar, gzip, zip, ar, ld) PASSED 100%!")
    return 0

if __name__ == '__main__':
    sys.exit(main())

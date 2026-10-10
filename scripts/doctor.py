import os
import sys
import subprocess

BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

CHECKS = [
    ("Clang C Compiler", os.path.join(BASE_DIR, "tools", "llvm-toolchain", "bin", "clang.exe"), ["--version"]),
    ("LLD Linker", os.path.join(BASE_DIR, "tools", "llvm-toolchain", "bin", "ld.lld.exe"), ["--version"]),
    ("QEMU x86_64", os.path.join(BASE_DIR, "tools", "qemu", "qemu-system-x86_64.exe"), ["--version"]),
    ("OVMF UEFI Firmware", os.path.join(BASE_DIR, "tools", "qemu", "share", "edk2-x86_64-code.fd"), None),
    ("Limine UEFI Binary", os.path.join(BASE_DIR, "third_party", "limine", "BOOTX64.EFI"), None),
    ("Limine Header", os.path.join(BASE_DIR, "third_party", "limine", "limine.h"), None),
    ("Python Runtime", sys.executable, ["--version"]),
]

def main():
    print("==================================================================")
    print("                    AstraOS Toolchain Doctor                      ")
    print("==================================================================")
    all_ok = True

    for name, path, ver_args in CHECKS:
        exists = os.path.exists(path)
        status = "[ OK ]" if exists else "[FAIL]"
        if not exists:
            all_ok = False
            print(f"{status} {name:<22}: Missing at {path}")
            continue

        ver_info = "Present"
        if ver_args:
            try:
                out = subprocess.check_output([path] + ver_args, stderr=subprocess.STDOUT, text=True)
                ver_info = out.strip().splitlines()[0]
            except Exception as e:
                ver_info = f"Present (error checking version: {e})"

        print(f"{status} {name:<22}: {ver_info}")
        print(f"       -> Path: {path}")

    print("------------------------------------------------------------------")
    if all_ok:
        print("[SUCCESS] All required toolchain components and dependencies are ready!")
        print("You can build, test, and run AstraOS using: make, make test, make run")
    else:
        print("[ERROR] Some components are missing. See above messages for missing paths.")
    print("==================================================================")

    sys.exit(0 if all_ok else 1)

if __name__ == '__main__':
    main()

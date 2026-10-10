import os
import shutil

BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ESP_DIR = os.path.join(BASE_DIR, 'build', 'esp')
EFI_BOOT_DIR = os.path.join(ESP_DIR, 'EFI', 'BOOT')

def prepare_esp():
    os.makedirs(EFI_BOOT_DIR, exist_ok=True)

    bootx64_src = os.path.join(BASE_DIR, 'third_party', 'limine', 'BOOTX64.EFI')
    if not os.path.exists(bootx64_src):
        raise FileNotFoundError(f"Missing {bootx64_src}")
    bootx64_dst = os.path.join(EFI_BOOT_DIR, 'BOOTX64.EFI')
    shutil.copy2(bootx64_src, bootx64_dst)

    conf_src = os.path.join(BASE_DIR, 'limine.conf')
    if not os.path.exists(conf_src):
        raise FileNotFoundError(f"Missing {conf_src}")
    conf_dst = os.path.join(ESP_DIR, 'limine.conf')
    shutil.copy2(conf_src, conf_dst)

    kernel_src = os.path.join(BASE_DIR, 'build', 'kernel.elf')
    if not os.path.exists(kernel_src):
        raise FileNotFoundError(f"Missing {kernel_src}")
    kernel_dst = os.path.join(ESP_DIR, 'kernel.elf')
    shutil.copy2(kernel_src, kernel_dst)

    print(f"[ESP] Prepared ESP directory at {ESP_DIR}")
    print(f"  - {bootx64_dst} ({os.path.getsize(bootx64_dst)} bytes)")
    print(f"  - {conf_dst} ({os.path.getsize(conf_dst)} bytes)")
    print(f"  - {kernel_dst} ({os.path.getsize(kernel_dst)} bytes)")

if __name__ == '__main__':
    prepare_esp()

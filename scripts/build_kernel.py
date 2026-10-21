import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_user
import build_initramfs

BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
def find_tool(candidates):
    for c in candidates:
        if os.path.exists(c):
            try:
                res = subprocess.run([c, '--version'], capture_output=True, text=True)
                if res.returncode == 0:
                    return c
            except Exception:
                pass
    return candidates[0]

CLANG = find_tool([
    os.path.join(BASE_DIR, 'tools', 'llvm-toolchain', 'bin', 'clang.exe'),
    r'C:\Program Files\LLVM\bin\clang.exe'
])
LD = find_tool([
    os.path.join(BASE_DIR, 'tools', 'llvm-toolchain', 'bin', 'ld.lld.exe'),
    r'C:\Program Files\LLVM\bin\ld.lld.exe'
])
READELF = find_tool([
    os.path.join(BASE_DIR, 'tools', 'llvm-toolchain', 'bin', 'llvm-readelf.exe'),
    r'C:\mingw64\bin\readelf.exe',
    r'C:\Program Files\LLVM\bin\llvm-readobj.exe'
])
BUILD_DIR = os.path.join(BASE_DIR, 'build')

SOURCES = [
    'kernel/kernel.c',
    'kernel/boot/limine_reqs.c',
    'kernel/drivers/serial.c',
    'kernel/drivers/font.c',
    'kernel/drivers/framebuffer.c',
    'kernel/debug/kprintf.c',
    'kernel/debug/panic.c',
    'kernel/arch/x86_64/gdt.c',
    'kernel/arch/x86_64/idt.c',
    'kernel/arch/x86_64/idt_asm.S',
    'kernel/arch/x86_64/vmm_switch.S',
    'kernel/mm/memmap.c',
    'kernel/mm/pmm.c',
    'kernel/mm/vmm.c',
    'kernel/mm/heap.c',
    'kernel/drivers/apic.c',
    'kernel/sched/sched.c',
    'kernel/sched/switch.S',
    'kernel/sched/sync.c',
    'kernel/process/process.c',
    'kernel/process/syscall.c',
    'kernel/process/user_test.S',
    'kernel/process/elf.c',
    'kernel/fs/vfs.c',
    'kernel/fs/initramfs.c',
    'kernel/fs/vfs_test.c',
    'kernel/monitor/monitor.c',
    'kernel/drivers/gfx.c',
    'kernel/drivers/input.c',
    'kernel/drivers/ps2.c',
    'kernel/gui/wm.c',
    'kernel/gui/desktop.c',
    'kernel/gui/app_editor.c',
    'kernel/gui/app_filemgr.c',
    'kernel/drivers/pci.c',
    'kernel/drivers/e1000.c',
    'kernel/net/net.c',
    'kernel/net/ethernet.c',
    'kernel/net/arp.c',
    'kernel/net/ipv4.c',
    'kernel/net/icmp.c',
    'kernel/net/udp.c',
    'kernel/net/dhcp.c',
    'kernel/net/dns.c',
    'kernel/net/tcp.c',
    'kernel/net/http.c',
    'kernel/net/net_test.c',
    'kernel/drivers/rtc.c',
    'kernel/crypto/crypto_rand.c',
    'kernel/crypto/sha256.c',
    'kernel/crypto/hkdf.c',
    'kernel/crypto/chacha20_poly1305.c',
    'kernel/crypto/x25519.c',
    'kernel/crypto/rsa.c',
    'kernel/crypto/x509.c',
    'kernel/crypto/crypto_test.c',
    'kernel/net/tls.c',
    'kernel/browser/browser_url.c',
    'kernel/browser/css_engine.c',
    'kernel/browser/html_parser.c',
    'kernel/browser/browser_render.c',
    'kernel/browser/app_browser.c'
]

CFLAGS = [
    '-target', 'x86_64-unknown-none-elf',
    '-std=c17',
    '-ffreestanding',
    '-fno-stack-protector',
    '-fno-stack-check',
    '-fno-lto',
    '-mno-red-zone',
    '-mno-80387',
    '-mno-mmx',
    '-mno-sse',
    '-mno-sse2',
    '-mcmodel=kernel',
    '-Wall',
    '-Wextra',
    '-O2',
    '-g',
    '-I', os.path.join(BASE_DIR, 'kernel', 'include'),
    '-I', os.path.join(BASE_DIR, 'kernel'),
    '-I', os.path.join(BASE_DIR, 'third_party', 'limine'),
    '-I', BASE_DIR
]

LDFLAGS = [
    '-m', 'elf_x86_64',
    '-nostdlib',
    '-static',
    '-T', os.path.join(BASE_DIR, 'kernel', 'arch', 'x86_64', 'linker.ld')
]

def main():
    os.makedirs(BUILD_DIR, exist_ok=True)
    build_user.build_user()
    build_initramfs.build_initramfs()
    obj_files = []

    print("[BUILD] Compiling AstraOS kernel sources...")
    for src in SOURCES:
        src_path = os.path.join(BASE_DIR, src)
        base_name = os.path.splitext(os.path.basename(src))[0]
        obj_path = os.path.join(BUILD_DIR, base_name + '.o')
        obj_files.append(obj_path)

        cmd = [CLANG] + CFLAGS + ['-c', src_path, '-o', obj_path]
        ret = subprocess.run(cmd)
        if ret.returncode != 0:
            print(f"[ERROR] Compilation failed for {src}")
            sys.exit(1)
        print(f"  CC  {src} -> {os.path.basename(obj_path)}")

    kernel_elf = os.path.join(BUILD_DIR, 'kernel.elf')
    print(f"[BUILD] Linking kernel executable -> {os.path.basename(kernel_elf)}")
    link_cmd = [LD] + LDFLAGS + ['-o', kernel_elf] + obj_files
    ret = subprocess.run(link_cmd)
    if ret.returncode != 0:
        print("[ERROR] Linking kernel failed")
        sys.exit(1)

    print("[BUILD] Kernel ELF built successfully.")
    # Verify ELF
    subprocess.run([READELF, '-h', kernel_elf])

if __name__ == '__main__':
    main()

import os
import subprocess
import sys

BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLANG = os.path.join(BASE_DIR, 'tools', 'llvm-toolchain', 'bin', 'clang.exe')
LD = os.path.join(BASE_DIR, 'tools', 'llvm-toolchain', 'bin', 'ld.lld.exe')
READELF = os.path.join(BASE_DIR, 'tools', 'llvm-toolchain', 'bin', 'llvm-readelf.exe')
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
    'kernel/mm/memmap.c',
    'kernel/mm/pmm.c',
    'kernel/monitor/monitor.c'
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

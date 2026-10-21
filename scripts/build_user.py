import os
import subprocess
import sys

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
USER_DIR = os.path.join(BASE_DIR, 'user')
OUT_ELF = os.path.join(BUILD_DIR, 'user_app.elf')
HEADER_OUT = os.path.join(BASE_DIR, 'kernel', 'include', 'kernel', 'user_app_elf.h')

CFLAGS = [
    '-target', 'x86_64-unknown-none-elf',
    '-std=c17',
    '-ffreestanding',
    '-fno-stack-protector',
    '-fno-stack-check',
    '-fno-pic',
    '-fno-pie',
    '-mno-red-zone',
    '-mcmodel=small',
    '-mgeneral-regs-only',
    '-O2',
    '-Wall',
    '-Wextra',
    '-Werror'
]

def build_user():
    os.makedirs(BUILD_DIR, exist_ok=True)
    print(f"[USER] Compiling native user program using {CLANG}...")

    # 1. Compile user_entry.S
    s_obj = os.path.join(BUILD_DIR, 'user_entry.o')
    cmd_s = [CLANG] + CFLAGS + ['-c', os.path.join(USER_DIR, 'user_entry.S'), '-o', s_obj]
    res = subprocess.run(cmd_s)
    if res.returncode != 0:
        print("[ERROR] Failed to compile user_entry.S")
        sys.exit(1)

    # 2. Compile main.c
    c_obj = os.path.join(BUILD_DIR, 'user_main.o')
    cmd_c = [CLANG] + CFLAGS + ['-c', os.path.join(USER_DIR, 'main.c'), '-o', c_obj]
    res = subprocess.run(cmd_c)
    if res.returncode != 0:
        print("[ERROR] Failed to compile user main.c")
        sys.exit(1)

    # 3. Link executable
    print(f"[USER] Linking native user executable using {LD}...")
    linker_script = os.path.join(USER_DIR, 'linker.ld')
    cmd_ld = [
        LD,
        '-T', linker_script,
        '-static',
        '-nostdlib',
        '-z', 'max-page-size=0x1000',
        '-o', OUT_ELF,
        s_obj, c_obj
    ]
    res = subprocess.run(cmd_ld)
    if res.returncode != 0:
        print("[ERROR] Failed to link user_app.elf")
        sys.exit(1)

    print(f"[USER] Successfully built ELF: {OUT_ELF} ({os.path.getsize(OUT_ELF)} bytes)")

    # 3b. Compile and Link net_client.c
    net_c_obj = os.path.join(BUILD_DIR, 'net_client.o')
    net_out_elf = os.path.join(BUILD_DIR, 'net_client.elf')
    cmd_net_c = [CLANG] + CFLAGS + ['-c', os.path.join(USER_DIR, 'net_client.c'), '-o', net_c_obj]
    res = subprocess.run(cmd_net_c)
    if res.returncode != 0:
        print("[ERROR] Failed to compile net_client.c")
        sys.exit(1)

    cmd_net_ld = [
        LD,
        '-T', linker_script,
        '-static',
        '-nostdlib',
        '-z', 'max-page-size=0x1000',
        '-o', net_out_elf,
        s_obj, net_c_obj
    ]
    res = subprocess.run(cmd_net_ld)
    if res.returncode != 0:
        print("[ERROR] Failed to link net_client.elf")
        sys.exit(1)
    print(f"[USER] Successfully built ELF: {net_out_elf} ({os.path.getsize(net_out_elf)} bytes)")

    # 3c. Compile and Link hello_gui.c
    hello_c_obj = os.path.join(BUILD_DIR, 'hello_gui.o')
    hello_out_elf = os.path.join(BUILD_DIR, 'hello_gui.elf')
    cmd_hello_c = [CLANG] + CFLAGS + ['-c', os.path.join(USER_DIR, 'hello_gui.c'), '-o', hello_c_obj]
    res = subprocess.run(cmd_hello_c)
    if res.returncode != 0:
        print("[ERROR] Failed to compile hello_gui.c")
        sys.exit(1)

    cmd_hello_ld = [
        LD,
        '-T', linker_script,
        '-static',
        '-nostdlib',
        '-z', 'max-page-size=0x1000',
        '-o', hello_out_elf,
        s_obj, hello_c_obj
    ]
    res = subprocess.run(cmd_hello_ld)
    if res.returncode != 0:
        print("[ERROR] Failed to link hello_gui.elf")
        sys.exit(1)
    print(f"[USER] Successfully built ELF: {hello_out_elf} ({os.path.getsize(hello_out_elf)} bytes)")

    # 3d. Compile and Link counter_gui.c
    counter_c_obj = os.path.join(BUILD_DIR, 'counter_gui.o')
    counter_out_elf = os.path.join(BUILD_DIR, 'counter_gui.elf')
    cmd_counter_c = [CLANG] + CFLAGS + ['-c', os.path.join(USER_DIR, 'counter_gui.c'), '-o', counter_c_obj]
    res = subprocess.run(cmd_counter_c)
    if res.returncode != 0:
        print("[ERROR] Failed to compile counter_gui.c")
        sys.exit(1)

    cmd_counter_ld = [
        LD,
        '-T', linker_script,
        '-static',
        '-nostdlib',
        '-z', 'max-page-size=0x1000',
        '-o', counter_out_elf,
        s_obj, counter_c_obj
    ]
    res = subprocess.run(cmd_counter_ld)
    if res.returncode != 0:
        print("[ERROR] Failed to link counter_gui.elf")
        sys.exit(1)
    print(f"[USER] Successfully built ELF: {counter_out_elf} ({os.path.getsize(counter_out_elf)} bytes)")

    # 3e. Compile and Link browser_main.c and js_engine.c -> browser.elf
    js_c_obj = os.path.join(BUILD_DIR, 'js_engine.o')
    cmd_js_c = [CLANG] + CFLAGS + ['-c', os.path.join(USER_DIR, 'js_engine.c'), '-o', js_c_obj]
    res = subprocess.run(cmd_js_c)
    if res.returncode != 0:
        print("[ERROR] Failed to compile js_engine.c")
        sys.exit(1)

    browser_c_obj = os.path.join(BUILD_DIR, 'browser_main.o')
    browser_out_elf = os.path.join(BUILD_DIR, 'browser.elf')
    cmd_browser_c = [CLANG] + CFLAGS + ['-c', os.path.join(USER_DIR, 'browser_main.c'), '-o', browser_c_obj]
    res = subprocess.run(cmd_browser_c)
    if res.returncode != 0:
        print("[ERROR] Failed to compile browser_main.c")
        sys.exit(1)

    cmd_browser_ld = [
        LD,
        '-T', linker_script,
        '-static',
        '-nostdlib',
        '-z', 'max-page-size=0x1000',
        '-o', browser_out_elf,
        s_obj, browser_c_obj, js_c_obj
    ]
    res = subprocess.run(cmd_browser_ld)
    if res.returncode != 0:
        print("[ERROR] Failed to link browser.elf")
        sys.exit(1)
    print(f"[USER] Successfully built ELF: {browser_out_elf} ({os.path.getsize(browser_out_elf)} bytes)")

    # 4. Inspect ELF header and program headers
    print("\n[USER] Inspecting ELF Header:")
    subprocess.run([READELF, '-h', OUT_ELF])
    print("\n[USER] Inspecting Program Headers:")
    subprocess.run([READELF, '-l', OUT_ELF])

    # 5. Generate C header embedding the ELF executable bytes
    with open(OUT_ELF, 'rb') as f:
        elf_bytes = f.read()

    with open(HEADER_OUT, 'w') as f:
        f.write("/* Auto-generated by scripts/build_user.py - DO NOT EDIT MANUALLY */\n")
        f.write("#ifndef ASTRA_KERNEL_USER_APP_ELF_H\n")
        f.write("#define ASTRA_KERNEL_USER_APP_ELF_H\n\n")
        f.write("#include <kernel/types.h>\n\n")
        f.write(f"static const size_t g_user_app_elf_len = {len(elf_bytes)};\n")
        f.write("static const u8 g_user_app_elf[] __attribute__((aligned(16))) = {\n")
        for i in range(0, len(elf_bytes), 12):
            chunk = elf_bytes[i:i+12]
            f.write("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",\n")
        f.write("};\n\n")
        f.write("#endif /* ASTRA_KERNEL_USER_APP_ELF_H */\n")

    print(f"\n[USER] Generated embedded header: {HEADER_OUT} ({len(elf_bytes)} bytes)\n")

if __name__ == '__main__':
    build_user()

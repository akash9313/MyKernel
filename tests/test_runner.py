import subprocess
import os
import sys
import time
import argparse

BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if BASE_DIR not in sys.path:
    sys.path.insert(0, BASE_DIR)
QEMU = os.path.join(BASE_DIR, 'tools', 'qemu', 'qemu-system-x86_64.exe')
OVMF = os.path.join(BASE_DIR, 'tools', 'qemu', 'share', 'edk2-x86_64-code.fd')
ESP = os.path.join(BASE_DIR, 'build', 'esp')
def find_readelf():
    candidates = [
        os.path.join(BASE_DIR, 'tools', 'llvm-toolchain', 'bin', 'llvm-readelf.exe'),
        r'C:\mingw64\bin\readelf.exe',
        r'C:\Program Files\LLVM\bin\llvm-readobj.exe'
    ]
    for c in candidates:
        if os.path.exists(c):
            try:
                res = subprocess.run([c, '--version'], capture_output=True, text=True)
                if res.returncode == 0:
                    return c
            except Exception:
                pass
    return candidates[0]

READELF = find_readelf()

def get_rodata_start():
    elf_path = os.path.join(BASE_DIR, 'build', 'kernel.elf')
    if not os.path.exists(elf_path):
        elf_path = os.path.join(ESP, 'kernel.elf')
    if os.path.exists(READELF) and os.path.exists(elf_path):
        try:
            res = subprocess.run([READELF, '-s', elf_path], stdout=subprocess.PIPE, text=True)
            for line in res.stdout.splitlines():
                if '__rodata_start' in line:
                    parts = line.strip().split()
                    for p in parts:
                        if len(p) == 16 and all(c in '0123456789abcdefABCDEF' for c in p):
                            return int(p, 16)
        except Exception:
            pass
    return 0xffffffff80209000

def update_limine_conf(cmdline_arg=None):
    conf_path = os.path.join(ESP, 'limine.conf')
    with open(conf_path, 'w') as f:
        f.write("timeout: 0\n\n/AstraOS\n    protocol: limine\n    path: boot():/kernel.elf\n    module_path: boot():/initramfs.bin\n")
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
    if not os.path.exists(os.path.join(ESP, 'initramfs.bin')):
        print(f"[FAIL] Initramfs binary not found in ESP: {ESP}")
        sys.exit(1)

    # Set up limine.conf according to test mode
    if test_mode == "exception":
        update_limine_conf("test=exception")
    elif test_mode == "nofb":
        update_limine_conf("nofb")
    elif test_mode == "ro_fault":
        update_limine_conf("test=ro_fault")
    elif test_mode == "nx_fault":
        update_limine_conf("test=nx_fault")
    elif test_mode == "guard_fault":
        update_limine_conf("test=guard_fault")
    elif test_mode == "heap_lower_guard":
        update_limine_conf("test=heap_lower_guard")
    elif test_mode == "heap_upper_guard":
        update_limine_conf("test=heap_upper_guard")
    elif test_mode == "heap_rollback":
        update_limine_conf("test=heap_rollback")
    elif test_mode == "df_fault":
        update_limine_conf("test=df_fault")
    elif test_mode == "thread_guard":
        update_limine_conf("test=thread_guard")
    elif test_mode == "mutex_recursive":
        update_limine_conf("test=mutex_recursive")
    elif test_mode == "mutex_non_owner":
        update_limine_conf("test=mutex_non_owner")
    elif test_mode == "mutex_exit_owning":
        update_limine_conf("test=mutex_exit_owning")
    else:
        update_limine_conf(None)

    https_server = None
    if test_mode == "normal":
        try:
            from tests.https_test_server import run_server_thread
            https_server, _ = run_server_thread(8443)
        except Exception as e:
            print(f"[WARN] Could not start local HTTPS test server: {e}")

    cmd = [
        QEMU,
        '-cpu', 'qemu64,+rdrand',
        '-drive', f'if=pflash,format=raw,unit=0,file={OVMF},readonly=on',
        '-m', ram_size,
        '-drive', f'file=fat:rw:{ESP},format=raw',
        '-netdev', 'user,id=net0',
        '-device', 'e1000,netdev=net0',
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
        stderr=subprocess.DEVNULL,
        text=True,
        bufsize=1
    )

    full_output = []
    start_time = time.time()
    timeout_sec = 35

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

    if test_mode in ["ro_fault", "nx_fault", "guard_fault", "heap_lower_guard", "heap_upper_guard"]:
        print(f"[TEST] Waiting for deliberate CPU Page Fault (#PF) Vector 14 diagnostic for '{test_mode}'...")
        found_pf = False
        found_perm_ok = False
        found_cr2 = False
        found_flags = False

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

            if "Vector:     14 (Page Fault)" in line:
                found_pf = True

            if test_mode == "ro_fault":
                rodata_va = get_rodata_start()
                if f"CR2 (Fault Addr): 0x{rodata_va:016x}" in line:
                    found_cr2 = True
                if "PF Flags:         Present=1, Write=1, User=0, Rsvd=0, Fetch=0" in line:
                    found_flags = True
            elif test_mode == "nx_fault":
                if "CR2 (Fault Addr): 0xffffffff80500000" in line:
                    found_cr2 = True
                if "PF Flags:         Present=1, Write=0, User=0, Rsvd=0, Fetch=1" in line:
                    found_flags = True
            elif test_mode == "guard_fault":
                if "CR2 (Fault Addr): 0xffffffff80400000" in line:
                    found_cr2 = True
                if "PF Flags:         Present=0, Write=1, User=0, Rsvd=0, Fetch=0" in line:
                    found_flags = True
            elif test_mode == "heap_lower_guard":
                if "CR2 (Fault Addr): 0xffffffff80600000" in line:
                    found_cr2 = True
                if "PF Flags:         Present=0, Write=1, User=0, Rsvd=0, Fetch=0" in line:
                    found_flags = True
            elif test_mode == "heap_upper_guard":
                if "CR2 (Fault Addr): 0xffffffff80611000" in line:
                    found_cr2 = True
                if "PF Flags:         Present=0, Write=1, User=0, Rsvd=0, Fetch=0" in line:
                    found_flags = True

            if "ASTRA_PERM_TEST_OK" in line:
                found_perm_ok = True
                break

        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()

        if found_pf and found_cr2 and found_flags and found_perm_ok:
            print(f"\n[TEST] PASS: Successfully verified CPU-enforced protection for '{test_mode}' (Vector 14, exact CR2, and expected hardware error bits verified)!\n")
            return True
        else:
            print(f"\n[FAIL] Destructive test '{test_mode}' failed (pf={found_pf}, cr2={found_cr2}, flags={found_flags}, perm_ok={found_perm_ok}).\n")
            return False

    if test_mode == "heap_rollback":
        print(f"[TEST] Waiting for transactional heap rollback diagnostic...")
        found_rollback = False
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

            if "ASTRA_HEAP_ROLLBACK_OK" in line:
                found_rollback = True
                break

        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()

        if found_rollback:
            print(f"\n[TEST] PASS: Successfully verified transactional heap rollback on fault injection!\n")
            return True
        else:
            print(f"\n[FAIL] Heap rollback test failed.\n")
            return False

    if test_mode == "df_fault":
        print("[TEST] Waiting for deliberate Double Fault (#DF Vector 8) on IST1 stack...")
        found_df = False
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

            if "ASTRA_DF_TEST_OK" in line:
                found_df = True
                break

        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()

        if found_df:
            print("\n[TEST] PASS: Double Fault cleanly handled on guarded IST1 stack!\n")
            return True
        else:
            print("\n[FAIL] Double Fault test did not produce expected ASTRA_DF_TEST_OK.\n")
            return False

    if test_mode == "thread_guard":
        print("[TEST] Waiting for deliberate Thread Stack Guard Page Fault (#PF Vector 14)...")
        found_tg = False
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

            if "ASTRA_THREAD_GUARD_OK" in line:
                found_tg = True
                break

        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()

        if found_tg:
            print("\n[TEST] PASS: Thread stack guard fault cleanly caught on unmapped guard page!\n")
            return True
        else:
            print("\n[FAIL] Thread stack guard test did not produce expected ASTRA_THREAD_GUARD_OK.\n")
            return False

    if test_mode == "mutex_recursive":
        print("[TEST] Waiting for deliberate Recursive Mutex Lock panic...")
        found_diag = False
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

            if "recursive acquisition attempted" in line:
                found_diag = True
            if "System halted." in line:
                found_halt = True
                break

        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()

        if found_diag and found_halt:
            print("\n[TEST] PASS: Recursive mutex lock correctly trapped with diagnosed panic and safely halted!\n")
            return True
        else:
            print(f"\n[FAIL] Recursive mutex test failed (diag={found_diag}, halt={found_halt}).\n")
            return False

    if test_mode == "mutex_non_owner":
        print("[TEST] Waiting for deliberate Non-Owner Mutex Unlock panic...")
        found_diag = False
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

            if "attempted to unlock mutex" in line and "owned by" in line:
                found_diag = True
            if "System halted." in line:
                found_halt = True
                break

        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()

        if found_diag and found_halt:
            print("\n[TEST] PASS: Non-owner mutex unlock correctly trapped with diagnosed panic and safely halted!\n")
            return True
        else:
            print(f"\n[FAIL] Non-owner mutex unlock test failed (diag={found_diag}, halt={found_halt}).\n")
            return False

    if test_mode == "mutex_exit_owning":
        print("[TEST] Waiting for deliberate Exit While Owning Mutex panic...")
        found_diag = False
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

            if "attempted to exit while still owning" in line:
                found_diag = True
            if "System halted." in line:
                found_halt = True
                break

        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()

        if found_diag and found_halt:
            print("\n[TEST] PASS: Exit while owning mutex correctly trapped with diagnosed panic and safely halted!\n")
            return True
        else:
            print(f"\n[FAIL] Exit while owning mutex test failed (diag={found_diag}, halt={found_halt}).\n")
            return False

    # Normal / nofb test mode
    print("[TEST] Step 1: Waiting for prerequisites (Boot, PMM, VMM, Own Paging, TSS, IDT, Heap, APIC, Context Switch, Coop Sched, Thread Lifecycle, Wait Queue, Event, Sleep, Idle Wake)...")
    boot_ok = False
    pmm_ok = False
    vmm_ok = False
    own_paging_ok = False
    heap_ok = False
    tss_ok = False
    idt_ok = False
    apic_timer_ok = False
    ctx_switch_ok = False
    coop_sched_ok = False
    thread_lifecycle_ok = False
    wait_queue_ok = False
    event_ok = False
    sleep_ok = False
    idle_wake_ok = False
    preempt_guard_ok = False
    crit_sec_ok = False
    preempt_ok = False
    mutex_ok = False
    mutex_handoff_ok = False
    semaphore_ok = False
    semaphore_irq_ok = False
    sync_stress_ok = False
    usermode_ok = False
    syscall_ok = False
    process_exit_ok = False
    user_mem_prot_ok = False
    elf_header_ok = False
    elf_segments_ok = False
    elf_invalid_input_ok = False
    elf_rollback_ok = False
    elf_user_exec_ok = False
    initramfs_ok = False
    vfs_lookup_ok = False
    vfs_read_ok = False
    vfs_invalid_archive_ok = False
    vfs_elf_exec_ok = False
    net_dev_ok = False
    net_drv_ok = False
    arp_ok = False
    ipv4_ok = False
    dhcp_ok = False
    dns_ok = False
    tcp_ok = False
    http_client_ok = False
    browser_render_ok = False
    browser_http_ok = False
    tls_cert_ok = False
    css_parser_ok = False
    css_cascade_ok = False
    css_layout_ok = False
    user_gui_ok = False
    multi_process_gui_ok = False
    gui_isolation_ok = False
    mmap_val_ok = False
    user_mmap_ok = False
    user_browser_ok = False
    user_browser_exec_ok = False
    browser_iso_ok = False
    browser_image_ok = False
    browser_ext_css_ok = False
    browser_forms_ok = False
    browser_nav_ok = False
    browser_scroll_ok = False
    js_feasibility_ok = False
    js_runtime_ok = False
    js_isolation_ok = False
    js_hardening_ok = False
    dom_bridge_ok = False
    dom_events_ok = False
    browser_interactive_ok = False
    js_adversarial_ok = False
    browser_security_ok = False
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

        if "ASTRA_VMM_TABLES_OK" in line:
            vmm_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_VMM_TABLES_OK' marker (In-kernel VMM self-test passed)!\n")

        if "ASTRA_OWN_PAGING_OK" in line:
            own_paging_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_OWN_PAGING_OK' marker (Owned CR3 & Dedicated Stack active)!\n")

        if "ASTRA_TSS_OK" in line:
            tss_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_TSS_OK' marker (Task State Segment & IST1 active)!\n")

        if "ASTRA_IDT_OK" in line:
            idt_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_IDT_OK' marker (IDT initialized with IST1 & IRQs)!\n")

        if "ASTRA_HEAP_OK" in line:
            heap_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_HEAP_OK' marker (Kernel heap self-test passed)!\n")

        if "ASTRA_APIC_TIMER_OK" in line:
            apic_timer_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_APIC_TIMER_OK' marker (Repeated APIC timer delivery verified)!\n")

        if "ASTRA_CONTEXT_SWITCH_OK" in line:
            ctx_switch_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_CONTEXT_SWITCH_OK' marker (Callee-saved context switch verified)!\n")

        if "ASTRA_COOP_SCHED_OK" in line:
            coop_sched_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_COOP_SCHED_OK' marker (Round-robin cooperative order verified)!\n")

        if "ASTRA_THREAD_LIFECYCLE_OK" in line:
            thread_lifecycle_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_THREAD_LIFECYCLE_OK' marker (Thread lifecycle & zero leaks verified)!\n")

        if "ASTRA_WAIT_QUEUE_OK" in line:
            wait_queue_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_WAIT_QUEUE_OK' marker (FIFO wait queue verified)!\n")

        if "ASTRA_EVENT_OK" in line:
            event_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_EVENT_OK' marker (Persistent auto-reset event verified)!\n")

        if "ASTRA_SLEEP_OK" in line:
            sleep_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_SLEEP_OK' marker (Tick sleep and non-preemption verified)!\n")

        if "ASTRA_IDLE_WAKE_OK" in line:
            idle_wake_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_IDLE_WAKE_OK' marker (Idle check-and-halt timer wakeup verified)!\n")

        if "ASTRA_PREEMPT_GUARD_OK" in line:
            preempt_guard_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_PREEMPT_GUARD_OK' marker (Preemption guard contract verified)!\n")

        if "ASTRA_CRITICAL_SECTION_OK" in line:
            crit_sec_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_CRITICAL_SECTION_OK' marker (Critical section & subsystem balance verified)!\n")

        if "ASTRA_PREEMPT_OK" in line:
            preempt_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_PREEMPT_OK' marker (Timer-driven preemption verified)!\n")

        if "ASTRA_MUTEX_OK" in line:
            mutex_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_MUTEX_OK' marker (Mutex core & preemption exclusion verified)!\n")

        if "ASTRA_MUTEX_HANDOFF_OK" in line:
            mutex_handoff_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_MUTEX_HANDOFF_OK' marker (Mutex FIFO direct handoff & anti-barging verified)!\n")

        if "ASTRA_SEMAPHORE_OK" in line:
            semaphore_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_SEMAPHORE_OK' marker (Counting semaphore accounting, bounds & FIFO verified)!\n")

        if "ASTRA_SEMAPHORE_IRQ_OK" in line:
            semaphore_irq_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_SEMAPHORE_IRQ_OK' marker (Semaphore maskable IRQ-to-thread signaling verified)!\n")

        if "ASTRA_SYNC_STRESS_OK" in line:
            sync_stress_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_SYNC_STRESS_OK' marker (Producer-Consumer bounded buffer stress test verified)!\n")

        if "ASTRA_USERMODE_OK" in line:
            usermode_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_USERMODE_OK' marker (Ring 3 CPL=3 execution verified)!\n")

        if "ASTRA_SYSCALL_OK" in line:
            syscall_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_SYSCALL_OK' marker (Syscall ABI: getpid, write, yield, exit verified)!\n")

        if "ASTRA_PROCESS_EXIT_OK" in line:
            process_exit_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_PROCESS_EXIT_OK' marker (Safe process termination & cleanup verified)!\n")

        if "ASTRA_USER_MEMORY_PROTECTION_OK" in line:
            user_mem_prot_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_USER_MEMORY_PROTECTION_OK' marker (User-mode supervisor memory isolation verified)!\n")

        if "ASTRA_ELF_HEADER_OK" in line:
            elf_header_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_ELF_HEADER_OK' marker (ELF64 header & structure validated)!\n")

        if "ASTRA_ELF_SEGMENTS_OK" in line:
            elf_segments_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_ELF_SEGMENTS_OK' marker (PT_LOAD segments & W^X verified)!\n")

        if "ASTRA_ELF_INVALID_INPUT_OK" in line:
            elf_invalid_input_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_ELF_INVALID_INPUT_OK' marker (Malformed/unsupported ELF variants safely rejected)!\n")

        if "ASTRA_ELF_ROLLBACK_OK" in line:
            elf_rollback_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_ELF_ROLLBACK_OK' marker (Transactional rollback with zero leaks verified)!\n")

        if "ASTRA_ELF_USER_EXEC_OK" in line:
            elf_user_exec_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_ELF_USER_EXEC_OK' marker (Native user ELF program executed at CPL3, verified .bss & syscalls)!\n")

        if "ASTRA_INITRAMFS_OK" in line:
            initramfs_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_INITRAMFS_OK' marker (Initramfs archive validated & mounted at root)!\n")

        if "ASTRA_VFS_LOOKUP_OK" in line:
            vfs_lookup_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_VFS_LOOKUP_OK' marker (VFS path lookup & traversal defenses verified)!\n")

        if "ASTRA_VFS_READ_OK" in line:
            vfs_read_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_VFS_READ_OK' marker (VFS open, read, pread, stat & readdir verified)!\n")

        if "ASTRA_VFS_INVALID_ARCHIVE_OK" in line:
            vfs_invalid_archive_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_VFS_INVALID_ARCHIVE_OK' marker (Parser defense against malformed archives verified)!\n")

        if "ASTRA_VFS_ELF_EXEC_OK" in line:
            vfs_elf_exec_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_VFS_ELF_EXEC_OK' marker (Native user ELF executed via VFS loader)!\n")

        if "ASTRA_PCI_NET_DEVICE_OK" in line:
            net_dev_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_PCI_NET_DEVICE_OK' marker (PCI network device detected)!\n")

        if "ASTRA_NET_DRIVER_OK" in line:
            net_drv_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_NET_DRIVER_OK' marker (E1000 driver initialized)!\n")

        if "ASTRA_ARP_OK" in line:
            arp_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_ARP_OK' marker (ARP parsing and cache verified)!\n")

        if "ASTRA_IPV4_OK" in line:
            ipv4_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_IPV4_OK' marker (IPv4 checksum & validation verified)!\n")

        if "ASTRA_DHCP_OK" in line:
            dhcp_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_DHCP_OK' marker (DHCP client negotiation verified)!\n")

        if "ASTRA_DNS_OK" in line:
            dns_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_DNS_OK' marker (DNS resolution & parser verified)!\n")

        if "ASTRA_TCP_OK" in line:
            tcp_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_TCP_OK' marker (TCP state machine & buffering verified)!\n")

        if "ASTRA_HTTP_CLIENT_OK" in line:
            http_client_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_HTTP_CLIENT_OK' marker (HTTP client request/response verified)!\n")

        if "ASTRA_BROWSER_RENDER_OK" in line:
            browser_render_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_BROWSER_RENDER_OK' marker (AstraBrowser layout & local render verified)!\n")

        if "ASTRA_BROWSER_HTTP_OK" in line:
            browser_http_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_BROWSER_HTTP_OK' marker (AstraBrowser HTTP client integration verified)!\n")

        if "ASTRA_TLS_CERT_VALIDATION_OK" in line:
            tls_cert_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_TLS_CERT_VALIDATION_OK' marker (X.509 DER, RSA, SHA256 & RTC verified)!\n")

        if "ASTRA_CSS_PARSER_OK" in line:
            css_parser_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_CSS_PARSER_OK' marker (CSS parser & tokenization verified)!\n")

        if "ASTRA_CSS_CASCADE_OK" in line:
            css_cascade_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_CSS_CASCADE_OK' marker (CSS cascade, specificity & inheritance verified)!\n")

        if "ASTRA_CSS_LAYOUT_OK" in line:
            css_layout_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_CSS_LAYOUT_OK' marker (CSS box model, alignment & wrapping verified)!\n")

        if "ASTRA_USER_GUI_OK" in line:
            user_gui_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_USER_GUI_OK' marker (Ring 3 GUI app verified)!\n")

        if "ASTRA_MULTI_PROCESS_GUI_OK" in line:
            multi_process_gui_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_MULTI_PROCESS_GUI_OK' marker (Multiple GUI processes verified)!\n")

        if "ASTRA_GUI_ISOLATION_OK" in line:
            gui_isolation_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_GUI_ISOLATION_OK' marker (GUI isolation & fault recovery verified)!\n")

        if "ASTRA_MMAP_VALIDATION_OK" in line:
            mmap_val_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_MMAP_VALIDATION_OK' marker (mmap/munmap validation verified)!\n")

        if "ASTRA_USER_MMAP_OK" in line:
            user_mmap_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_USER_MMAP_OK' marker (User-space mmap allocation verified)!\n")

        if "ASTRA_USER_BROWSER_OK" in line:
            user_browser_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_USER_BROWSER_OK' marker (User-space AstraBrowser initialized in Ring 3)!\n")

        if "ASTRA_USER_BROWSER_EXEC_OK" in line:
            user_browser_exec_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_USER_BROWSER_EXEC_OK' marker (User-space AstraBrowser execution verified)!\n")

        if "ASTRA_BROWSER_ISOLATION_OK" in line:
            browser_iso_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_BROWSER_ISOLATION_OK' marker (Browser memory isolation & fault reclamation verified)!\n")

        if "ASTRA_BROWSER_IMAGE_OK" in line:
            browser_image_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_BROWSER_IMAGE_OK' marker (BMP image decoding, validation & rendering verified)!\n")

        if "ASTRA_BROWSER_EXT_CSS_OK" in line:
            browser_ext_css_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_BROWSER_EXT_CSS_OK' marker (External CSS fetch, URL resolution & cascade verified)!\n")

        if "ASTRA_BROWSER_FORMS_OK" in line:
            browser_forms_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_BROWSER_FORMS_OK' marker (HTML form controls, text/checkbox & GET submission verified)!\n")

        if "ASTRA_BROWSER_NAV_OK" in line:
            browser_nav_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_BROWSER_NAV_OK' marker (Link resolution, bounded history & fragment navigation verified)!\n")

        if "ASTRA_BROWSER_SCROLL_OK" in line:
            browser_scroll_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_BROWSER_SCROLL_OK' marker (Document scrolling, anchor targets & coordinate mapping verified)!\n")

        if "ASTRA_JS_FEASIBILITY_OK" in line:
            js_feasibility_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_JS_FEASIBILITY_OK' marker (JavaScript engine feasibility & integer sandbox verified)!\n")

        if "ASTRA_JS_RUNTIME_OK" in line:
            js_runtime_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_JS_RUNTIME_OK' marker (JavaScript DOM bridging & text mutation verified)!\n")

        if "ASTRA_JS_ISOLATION_OK" in line:
            js_isolation_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_JS_ISOLATION_OK' marker (JavaScript fault containment & unprivileged execution verified)!\n")

        if "ASTRA_JS_HARDENING_OK" in line:
            js_hardening_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_JS_HARDENING_OK' marker (Precedence, short-circuit, overflow & limits verified)!\n")

        if "ASTRA_DOM_BRIDGE_OK" in line:
            dom_bridge_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_DOM_BRIDGE_OK' marker (Safe DOM mutation, reflow & generation guards verified)!\n")

        if "ASTRA_DOM_EVENTS_OK" in line:
            dom_events_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_DOM_EVENTS_OK' marker (Safe DOM event callback registration & dispatch verified)!\n")

        if "ASTRA_BROWSER_INTERACTIVE_OK" in line:
            browser_interactive_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_BROWSER_INTERACTIVE_OK' marker (Interactive browser button & link events verified)!\n")

        if "ASTRA_JS_ADVERSARIAL_OK" in line:
            js_adversarial_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_JS_ADVERSARIAL_OK' marker (Adversarial input syntax, scoping, unclosed functions & arithmetic verified)!\n")

        if "ASTRA_BROWSER_SECURITY_OK" in line:
            browser_security_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_BROWSER_SECURITY_OK' marker (Empty callbacks, mutation throttling, boundary clicks & stale handle rejection verified)!\n")

        if test_mode == "nofb" and ("operating in safe serial-only console mode" in line or "serial-only mode" in line):
            nofb_verified = True

        gui_conditions = (user_gui_ok and multi_process_gui_ok and gui_isolation_ok) if test_mode != "nofb" else True
        phase_o_conditions = (mmap_val_ok and user_mmap_ok and user_browser_ok and user_browser_exec_ok and browser_iso_ok and browser_image_ok and browser_ext_css_ok and browser_forms_ok and browser_nav_ok and browser_scroll_ok and js_feasibility_ok and js_runtime_ok and js_isolation_ok and js_hardening_ok and dom_bridge_ok and dom_events_ok and browser_interactive_ok and js_adversarial_ok and browser_security_ok) if test_mode != "nofb" else True

        if (boot_ok and pmm_ok and vmm_ok and own_paging_ok and heap_ok and 
            tss_ok and idt_ok and apic_timer_ok and ctx_switch_ok and coop_sched_ok and thread_lifecycle_ok and
            wait_queue_ok and event_ok and sleep_ok and idle_wake_ok and preempt_guard_ok and crit_sec_ok and
            preempt_ok and mutex_ok and mutex_handoff_ok and semaphore_ok and semaphore_irq_ok and sync_stress_ok and
            usermode_ok and syscall_ok and process_exit_ok and user_mem_prot_ok and
            elf_header_ok and elf_segments_ok and elf_invalid_input_ok and elf_rollback_ok and elf_user_exec_ok and
            initramfs_ok and vfs_lookup_ok and vfs_read_ok and vfs_invalid_archive_ok and vfs_elf_exec_ok and
            net_dev_ok and net_drv_ok and arp_ok and ipv4_ok and dhcp_ok and dns_ok and tcp_ok and http_client_ok and
            browser_render_ok and browser_http_ok and tls_cert_ok and
            css_parser_ok and css_cascade_ok and css_layout_ok and gui_conditions and phase_o_conditions):
            break

    gui_conditions = (user_gui_ok and multi_process_gui_ok and gui_isolation_ok) if test_mode != "nofb" else True
    phase_o_conditions = (mmap_val_ok and user_mmap_ok and user_browser_ok and user_browser_exec_ok and browser_iso_ok and browser_image_ok and browser_ext_css_ok and browser_forms_ok and browser_nav_ok and browser_scroll_ok and js_feasibility_ok and js_runtime_ok and js_isolation_ok and js_hardening_ok and dom_bridge_ok and dom_events_ok and browser_interactive_ok and js_adversarial_ok and browser_security_ok) if test_mode != "nofb" else True

    if not (boot_ok and pmm_ok and vmm_ok and own_paging_ok and heap_ok and 
            tss_ok and idt_ok and apic_timer_ok and ctx_switch_ok and coop_sched_ok and thread_lifecycle_ok and
            wait_queue_ok and event_ok and sleep_ok and idle_wake_ok and preempt_guard_ok and crit_sec_ok and
            preempt_ok and mutex_ok and mutex_handoff_ok and semaphore_ok and semaphore_irq_ok and sync_stress_ok and
            usermode_ok and syscall_ok and process_exit_ok and user_mem_prot_ok and
            elf_header_ok and elf_segments_ok and elf_invalid_input_ok and elf_rollback_ok and elf_user_exec_ok and
            initramfs_ok and vfs_lookup_ok and vfs_read_ok and vfs_invalid_archive_ok and vfs_elf_exec_ok and
            net_dev_ok and net_drv_ok and arp_ok and ipv4_ok and dhcp_ok and dns_ok and tcp_ok and http_client_ok and
            browser_render_ok and browser_http_ok and tls_cert_ok and
            css_parser_ok and css_cascade_ok and css_layout_ok and gui_conditions and phase_o_conditions):
        proc.kill()
        if https_server:
            https_server.shutdown()
        print(f"\n[FAIL] Boot prerequisites timeout or failure (boot={boot_ok}, net_dev={net_dev_ok}, net_drv={net_drv_ok}, arp={arp_ok}, ip={ipv4_ok}, dhcp={dhcp_ok}, dns={dns_ok}, tcp={tcp_ok}, http={http_client_ok}, browser_render={browser_render_ok}, browser_http={browser_http_ok}, tls_cert={tls_cert_ok}, css_parser={css_parser_ok}, css_cascade={css_cascade_ok}, css_layout={css_layout_ok}, user_gui={user_gui_ok}, multi_gui={multi_process_gui_ok}, gui_iso={gui_isolation_ok}, phase_o={phase_o_conditions})")
        return False

    if test_mode == "nofb" and not nofb_verified:
        proc.kill()
        print("\n[FAIL] No-framebuffer mode was not confirmed in kernel boot logs.")
        return False

    # Wait for initial monitor prompt
    prompt_seen = False
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
        if "AstraOS>" in line:
            prompt_seen = True
            break

    if not prompt_seen:
        proc.kill()
        print("\n[FAIL] Initial monitor prompt 'AstraOS>' was not reached.")
        return False

    def send_cmd_and_wait(command, expected_substr, max_wait=10.0):
        print(f"[TEST] Sending monitor command: '{command}'")
        for char in command + "\n":
            proc.stdin.write(char)
            proc.stdin.flush()
            time.sleep(0.005)

        cmd_start = time.time()
        found = False
        saw_prompt = False

        while time.time() - cmd_start < max_wait:
            line = proc.stdout.readline()
            if not line:
                if proc.poll() is not None:
                    break
                time.sleep(0.05)
                continue
            full_output.append(line)
            sys.stdout.write(line)
            sys.stdout.flush()
            if expected_substr in line:
                found = True
            if found and "AstraOS>" in line:
                saw_prompt = True
                break

        if not found:
            print(f"[FAIL] Expected substring '{expected_substr}' not found for command '{command}'.")
            return False
        if not saw_prompt:
            print(f"[FAIL] Monitor prompt 'AstraOS>' not returned after command '{command}'.")
            return False

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

    # Test 'memmap' (match end of memmap output)
    if not send_cmd_and_wait("memmap", "Reserved/MMIO Space"):
        proc.kill()
        return False

    # Test 'heap' (Heap stats)
    if not send_cmd_and_wait("heap", "Kernel Heap Stats"):
        proc.kill()
        return False

    # Test 'ticks' (APIC Timer Stats)
    if not send_cmd_and_wait("ticks", "APIC Timer Ticks"):
        proc.kill()
        return False

    # Test 'vmap' for active kernel code mapping (.text: Read-Only, Executable)
    if not send_cmd_and_wait("vmap 0xffffffff80200000", "Executable (X)"):
        proc.kill()
        return False

    # Test 'vmap' for kernel read-only data (.rodata: Read-Only, Non-Executable)
    rodata_va = get_rodata_start()
    if not send_cmd_and_wait(f"vmap 0x{rodata_va:016x}", "NX (No-Execute)"):
        proc.kill()
        return False

    # Test 'vmap' for dedicated kernel stack (RW, Non-Executable)
    if not send_cmd_and_wait("vmap 0xffffffff80404000", "Read/Write"):
        proc.kill()
        return False

    # Test 'vmap' for unmapped guard page (Deliberately Not Present)
    if not send_cmd_and_wait("vmap 0xffffffff80400000", "NOT PRESENT (Unmapped)"):
        proc.kill()
        return False

    # Test 'vmap' for heap arena (RW, Non-Executable)
    if not send_cmd_and_wait("vmap 0xffffffff80601000", "NX (No-Execute)"):
        proc.kill()
        return False

    # Test 'vmap' for heap lower guard page (Deliberately Not Present)
    if not send_cmd_and_wait("vmap 0xffffffff80600000", "NOT PRESENT (Unmapped)"):
        proc.kill()
        return False

    # Test 'vmap' for heap upper guard page (Deliberately Not Present)
    if not send_cmd_and_wait("vmap 0xffffffff80611000", "NOT PRESENT (Unmapped)"):
        proc.kill()
        return False

    # Test 'vmap' for non-canonical address
    if not send_cmd_and_wait("vmap 0x800000000000", "NON-CANONICAL"):
        proc.kill()
        return False

    # Test 'ls' (VFS directory listing)
    if not send_cmd_and_wait("ls /", "hello.txt"):
        proc.kill()
        return False

    # Test 'cat' (VFS file display)
    if not send_cmd_and_wait("cat /hello.txt", "Hello from AstraOS Initramfs!"):
        proc.kill()
        return False

    # Test 'ifconfig' (Network Interface display)
    if not send_cmd_and_wait("ifconfig", "Intel 82540EM Gigabit Ethernet"):
        proc.kill()
        return False

    # Test 'browser' (AstraBrowser interactive launch/navigation)
    if not send_cmd_and_wait("browser file:///www/index.html", "Launching AstraBrowser"):
        proc.kill()
        if https_server:
            https_server.shutdown()
        return False

    # Test 'browser' CSS verification page navigation
    if not send_cmd_and_wait("browser file:///www/css_test.html", "Launching AstraBrowser"):
        proc.kill()
        if https_server:
            https_server.shutdown()
        return False

    # Test 'browser' rich portal page navigation
    if not send_cmd_and_wait("browser file:///www/rich_page.html", "Launching AstraBrowser"):
        proc.kill()
        if https_server:
            https_server.shutdown()
        return False

    # Test 'browser' malformed CSS robustness page navigation
    if not send_cmd_and_wait("browser file:///www/css_malformed.html", "Launching AstraBrowser"):
        proc.kill()
        if https_server:
            https_server.shutdown()
        return False

    # Test 'browser' Phase Q navigation, fragments & scrolling page
    if not send_cmd_and_wait("browser file:///www/nav_test.html", "Launching AstraBrowser"):
        proc.kill()
        if https_server:
            https_server.shutdown()
        return False

    # Test 'browser' HTTPS navigation to controlled local HTTPS test server
    if https_server:
        if not send_cmd_and_wait("browser https://10.0.2.2:8443/", "ASTRA_BROWSER_HTTPS_OK", max_wait=15.0):
            proc.kill()
            https_server.shutdown()
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

    if https_server:
        https_server.shutdown()

    print(f"[TEST] PASS: All tests passed for Mode='{test_mode}', RAM={ram_size}!\n")
    return True

def main():
    parser = argparse.ArgumentParser(description="AstraOS QEMU Integration Test Runner")
    parser.add_argument("--all", action="store_true", help="Run full suite: 128M, 512M, 1024M, exception, nofb, memory faults, thread guard, and mutex failure modes")
    parser.add_argument("--ram", default="512M", help="Memory size (e.g. 128M, 512M, 1024M)")
    parser.add_argument("--mode", default="normal",
                        choices=["normal", "exception", "nofb", "ro_fault", "nx_fault", "guard_fault", "heap_lower_guard", "heap_upper_guard", "heap_rollback", "df_fault", "thread_guard", "mutex_recursive", "mutex_non_owner", "mutex_exit_owning"],
                        help="Test mode")
    args = parser.parse_args()

    if args.all:
        tests = [
            ("512M", "normal", "std"),
            ("128M", "normal", "std"),
            ("1024M", "normal", "std"),
            ("512M", "nofb", "none"),
            ("512M", "exception", "std"),
            ("512M", "ro_fault", "std"),
            ("512M", "nx_fault", "std"),
            ("512M", "guard_fault", "std"),
            ("512M", "heap_lower_guard", "std"),
            ("512M", "heap_upper_guard", "std"),
            ("512M", "heap_rollback", "std"),
            ("512M", "df_fault", "std"),
            ("512M", "thread_guard", "std"),
            ("512M", "mutex_recursive", "std"),
            ("512M", "mutex_non_owner", "std"),
            ("512M", "mutex_exit_owning", "std"),
        ]
        for ram, mode, vga in tests:
            ok = run_integration_test(ram, mode, vga)
            if not ok:
                print(f"[FAIL] Test suite failed on RAM={ram}, Mode={mode}, VGA={vga}")
                sys.exit(1)
        # Restore default limine.conf
        update_limine_conf(None)
        print("==================================================================")
        print("ALL ASTRAOS REGRESSION TESTS PASSED!")
        print("  - 128M, 512M, 1024M RAM Normal Boot (Owned CR3, Dedicated Stack & Verified Heap)")
        print("  - TSS with guarded double-fault IST1 stack active")
        print("  - 256-entry IDT with IST1 Double Fault and IRQ handlers")
        print("  - Repeated Local APIC timer delivery and EOI verified")
        print("  - Serial-only (nofb) Safe Console")
        print("  - Invalid Opcode (#UD) Vector 6 Exception")
        print("  - Read-Only Page (#PF Vector 14, Present=1, Write=1)")
        print("  - No-Execute Page (#PF Vector 14, Present=1, Fetch=1)")
        print("  - Stack Guard Page (#PF Vector 14, Present=0)")
        print("  - Heap Lower Boundary Guard Page (#PF Vector 14, Present=0)")
        print("  - Heap Upper Boundary Guard Page (#PF Vector 14, Present=0)")
        print("  - Transactional Heap Initialization Rollback on Fault Injection")
        print("  - Deliberate Double Fault (#DF Vector 8) on guarded IST1 stack")
        print("  - Thread Stack Guard Page Violation (#PF Vector 14) on guarded thread stack")
        print("  - FIFO wait queues verified (ASTRA_WAIT_QUEUE_OK)")
        print("  - Persistent auto-reset events verified (ASTRA_EVENT_OK)")
        print("  - Tick-based sleep and non-preemption verified (ASTRA_SLEEP_OK)")
        print("  - Idle check-and-halt timer wakeup verified (ASTRA_IDLE_WAKE_OK)")
        print("  - Preemption guard contract verified (ASTRA_PREEMPT_GUARD_OK)")
        print("  - Critical section and subsystem balance verified (ASTRA_CRITICAL_SECTION_OK)")
        print("  - Timer-driven kernel-thread preemption verified (ASTRA_PREEMPT_OK)")
        print("  - Non-recursive mutex exclusion verified (ASTRA_MUTEX_OK)")
        print("  - Mutex FIFO direct handoff & anti-barging verified (ASTRA_MUTEX_HANDOFF_OK)")
        print("  - Counting semaphore accounting, bounds & FIFO verified (ASTRA_SEMAPHORE_OK)")
        print("  - Semaphore maskable IRQ-to-thread signaling verified (ASTRA_SEMAPHORE_IRQ_OK)")
        print("  - Producer-Consumer bounded buffer stress test verified (ASTRA_SYNC_STRESS_OK)")
        print("  - Ring 3 CPL=3 execution verified (ASTRA_USERMODE_OK)")
        print("  - Syscall ABI: getpid, write, yield, exit verified (ASTRA_SYSCALL_OK)")
        print("  - Safe process termination & resource reclamation verified (ASTRA_PROCESS_EXIT_OK)")
        print("  - User-mode supervisor memory isolation verified (ASTRA_USER_MEMORY_PROTECTION_OK)")
        print("  - ELF64 header & structural integrity verified (ASTRA_ELF_HEADER_OK)")
        print("  - ELF64 PT_LOAD segments & W^X permissions verified (ASTRA_ELF_SEGMENTS_OK)")
        print("  - Malformed & unsupported ELF inputs safely rejected (ASTRA_ELF_INVALID_INPUT_OK)")
        print("  - Transactional rollback on failed ELF creation verified (ASTRA_ELF_ROLLBACK_OK)")
        print("  - Native user ELF program executed at CPL3 with .bss zeroing (ASTRA_ELF_USER_EXEC_OK)")
        print("  - Diagnosed recursive mutex acquisition trap verified")
        print("  - Diagnosed non-owner mutex unlock trap verified")
        print("  - Diagnosed exit-while-owning-mutex trap verified")
        print("  - Bounded CSS engine & selector parser verified (ASTRA_CSS_PARSER_OK)")
        print("  - CSS cascade, specificity & inheritance verified (ASTRA_CSS_CASCADE_OK)")
        print("  - CSS box-model layout, text alignment & borders verified (ASTRA_CSS_LAYOUT_OK)")
        print("  - Ring 3 user-space GUI application verified (ASTRA_USER_GUI_OK)")
        print("  - Multiple simultaneous isolated GUI processes verified (ASTRA_MULTI_PROCESS_GUI_OK)")
        print("  - GUI syscall validation & fault isolation verified (ASTRA_GUI_ISOLATION_OK)")
        print("  - User-space mmap/munmap memory allocation & W^X verified (ASTRA_MMAP_VALIDATION_OK, ASTRA_USER_MMAP_OK)")
        print("  - User-space AstraBrowser isolated execution verified (ASTRA_USER_BROWSER_OK, ASTRA_USER_BROWSER_EXEC_OK, ASTRA_BROWSER_ISOLATION_OK)")
        print("  - BMP image decoding, validation & GUI rendering verified (ASTRA_BROWSER_IMAGE_OK)")
        print("  - External CSS fetch, URL resolution & cascade verified (ASTRA_BROWSER_EXT_CSS_OK)")
        print("  - HTML form controls, text/checkbox & GET submission verified (ASTRA_BROWSER_FORMS_OK)")
        print("  - Browser URL navigation, bounded history & fragment scrolling verified (ASTRA_BROWSER_NAV_OK, ASTRA_BROWSER_SCROLL_OK)")
        print("  - Isolated JavaScript engine & DOM bridge verified (ASTRA_JS_FEASIBILITY_OK, ASTRA_JS_RUNTIME_OK, ASTRA_JS_ISOLATION_OK)")
        print("  - Hardened JavaScript runtime & safe DOM integration verified (ASTRA_JS_HARDENING_OK, ASTRA_DOM_BRIDGE_OK)")
        print("  - Interactive DOM events & controls verified (ASTRA_DOM_EVENTS_OK, ASTRA_BROWSER_INTERACTIVE_OK)")
        print("  - Browser security regression & adversarial testing verified (ASTRA_JS_ADVERSARIAL_OK, ASTRA_BROWSER_SECURITY_OK)")
        print("==================================================================")
        sys.exit(0)
    else:
        vga = "none" if args.mode == "nofb" else "std"
        ok = run_integration_test(args.ram, args.mode, vga)
        update_limine_conf(None)
        sys.exit(0 if ok else 1)

if __name__ == '__main__':
    main()

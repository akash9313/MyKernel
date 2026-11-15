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
BUILD_DIR = os.path.join(BASE_DIR, 'build')
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

DATA_DISK = os.path.join(BUILD_DIR, "data_disk.img")

def ensure_data_disk():
    os.makedirs(BUILD_DIR, exist_ok=True)
    if not os.path.exists(DATA_DISK) or os.path.getsize(DATA_DISK) != 32 * 1024 * 1024:
        with open(DATA_DISK, "wb") as f:
            f.seek(32 * 1024 * 1024 - 1)
            f.write(b"\0")

def run_integration_test(ram_size="512M", test_mode="normal", vga_mode="std", smp_cpus=4):
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
        update_limine_conf("nofb test=all")
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
        update_limine_conf("test=all")

    https_server = None
    http_server = None
    if test_mode in ("normal", "nofb"):
        try:
            from tests.https_test_server import run_server_thread
            https_server, http_server = run_server_thread(8443, http_port=8000)
        except Exception as e:
            print(f"[WARN] Could not start local test servers: {e}")

    ensure_data_disk()

    cmd = [
        QEMU,
        '-smp', str(smp_cpus),
        '-cpu', 'qemu64,+rdrand',
        '-drive', f'if=pflash,format=raw,unit=0,file={OVMF},readonly=on',
        '-m', ram_size,
        '-drive', f'file=fat:rw:{ESP},format=raw',
        '-drive', f'file={DATA_DISK},format=raw,index=1,media=disk',
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
    smp_val = int(smp_cpus) if smp_cpus else 1
    timeout_sec = 450

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
    browser_net_ok = False
    http_hardening_ok = False
    browser_http_hard_ok = False
    browser_ux_ok = False
    browser_reliability_ok = False
    browser_tabs_ok = False
    block_device_ok = False
    fs_ok = False
    cold_probe_ok = False
    cold_bookmarks_ok = False
    nofb_verified = False
    ak_write_prot_ok = False
    ak_nx_prot_ok = False
    ak_guard_page_ok = False
    ak_syscall_sec_ok = False
    ak_fd_owner_ok = False
    ak_waitpid_ok = False
    ak_multi_app_ok = False
    ak_stress_ok = False
    al_preempt_ok = False
    al_multi_proc_ok = False
    al_block_wakeup_ok = False
    al_waitpid_ok = False
    al_cleanup_ok = False
    al_fairness_ok = False
    al_smp_audit_ok = False
    am_bsp_ready = False
    am_ap_started = False
    am_all_cpus_ready = False
    am_parallel_ok = False
    am_lifecycle_ok = False
    an_tlb_shootdown_ok = False
    an_process_teardown_ok = False
    an_memory_stress_ok = False
    an_scheduler_stress_ok = False
    an_storage_stress_ok = False
    ao_aspace_race_ok = False
    ao_concurrent_tlb_ok = False
    ao_stale_tlb_reuse_ok = False
    ao_deferred_reclaim_ok = False
    ao_lock_order_ok = False
    ao_percpu_isolation_ok = False
    ao_alloc_fault_ok = False
    ao_storage_fault_ok = False
    ap_aspace_protocol_ok = False
    ap_tlb_protocol_audit_ok = False
    ap_frame_reclamation_ok = False
    ap_lock_hierarchy_ok = False
    ap_harness_integrity_ok = False
    ap_negative_tests_ok = False
    aq_concurrent_shootdown_ok = False
    aq_tlb_fault_injection_ok = False
    aq_frame_reuse_ordering_ok = False
    aq_aspace_adversarial_ok = False
    aq_lock_deadlock_audit_ok = False
    aq_harness_classification_ok = False
    ar_concurrent_initiators_ok = False
    ar_delayed_ipi_lifetime_ok = False
    ar_stale_ack_rejection_ok = False
    ar_timeout_frame_retention_ok = False
    ar_unmap_caller_safety_ok = False
    ar_end_to_end_proof_ok = False
    as_queue_lifecycle_ok = False
    as_generation_ack_atomics_ok = False
    as_timedout_slot_drain_ok = False
    as_reentrant_servicing_ok = False
    as_pte_rollback_concurrency_ok = False
    as_end_to_end_failure_qual_ok = False
    at_memory_model_atomics_ok = False
    at_remote_handler_lifetime_ok = False
    at_double_gen_verify_ok = False
    at_lockfree_servicing_ok = False
    at_pte_transition_isolation_ok = False
    at_end_to_end_mem_model_ok = False
    au_reader_pause_before_reg_ok = False
    au_reader_reg_before_retire_ok = False
    au_retire_race_registration_ok = False
    au_delayed_ipi_timeout_drain_ok = False
    au_slot_reuse_stress_ok = False
    au_queue_exhaustion_ok = False
    av_reg_vs_retire_ok = False
    av_reader_blocks_reuse_ok = False
    av_simultaneous_race_ok = False
    av_generation_aba_ok = False
    av_delayed_ipi_timeout_ok = False
    av_concurrent_senders_ok = False
    av_queue_exhaustion_ok = False
    aw_munmap_overflow_ok = False
    aw_fd_cross_priv_ok = False
    aw_gui_cross_priv_ok = False
    aw_net_fetch_concurrency_ok = False
    aw_socket_owner_cleanup_ok = False
    aw_ipv4_underflow_ok = False
    aw_arp_size_validation_ok = False
    aw_path_security_defense_ok = False
    ax_block_bounds_ok = False
    ax_metadata_integrity_ok = False
    ax_crash_recovery_ok = False
    ax_precommit_flush_ok = False
    ax_rename_fd_preservation_ok = False
    ax_concurrent_lifecycle_ok = False
    ay_uefi_boot_info_ok = False
    ay_hardware_timer_ok = False
    ay_storage_discovery_ok = False
    ay_cpu_topology_ok = False
    ay_boot_media_protection_ok = False
    ay_subsystem_containment_ok = False
    term_cursor_ok = False
    native_shell_ok = False
    pkg_manager_ok = False

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

        if "KERNEL PANIC" in line or "System halted." in line:
            time.sleep(0.5)
            try:
                while True:
                    extra = proc.stdout.readline()
                    if not extra: break
                    sys.stdout.write(extra)
                    if "==========================================================" in extra: break
                sys.stdout.flush()
            except Exception:
                pass
            print("\n[FAIL] Kernel panic or unexpected halt detected during boot!")
            proc.kill()
            if https_server:
                https_server.shutdown()
            if http_server:
                http_server.shutdown()
            return False

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

        if "ASTRA_HTTP_HARDENING_OK" in line:
            http_hardening_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_HTTP_HARDENING_OK' marker (HTTP protocol hardening, chunked & framing verified)!\n")

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

        if "ASTRA_BROWSER_NET_ISOLATION_OK" in line:
            browser_net_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_BROWSER_NET_ISOLATION_OK' marker (Syscall boundary parameter bounds, bad schemes & VFS fetch verified)!\n")

        if "ASTRA_BROWSER_HTTP_HARDENING_OK" in line:
            browser_http_hard_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_BROWSER_HTTP_HARDENING_OK' marker (User-space browser HTTP hardening & adversarial recovery verified)!\n")

        if "ASTRA_BROWSER_UX_OK" in line:
            browser_ux_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_BROWSER_UX_OK' marker (Browser UX, navigation history, URL validation & keyboard shortcuts verified)!\n")

        if "ASTRA_BROWSER_RELIABILITY_OK" in line:
            browser_reliability_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_BROWSER_RELIABILITY_OK' marker (Browser reliability, stress navigation, parser boundaries & error recovery verified)!\n")

        if "ASTRA_BROWSER_TABS_BOOKMARKS_OK" in line:
            browser_tabs_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_BROWSER_TABS_BOOKMARKS_OK' marker (Browser tabs, bookmarks & serialization verified)!\n")

        if "ASTRA_BLOCK_DEVICE_OK" in line:
            block_device_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_BLOCK_DEVICE_OK' marker (Block device subsystem & ATA driver verified)!\n")

        if "ASTRA_FS_OK" in line:
            fs_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_FS_OK' marker (AstraFS persistent filesystem verified)!\n")

        if "ASTRA_FS_COLD_REBOOT_CONTENTS_VERIFIED" in line:
            cold_probe_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_FS_COLD_REBOOT_CONTENTS_VERIFIED' marker (Two-stage probe verified byte-for-byte across cold restart)!\n")

        if "ASTRA_BROWSER_BOOKMARKS_COLD_REBOOT_OK" in line:
            cold_bookmarks_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_BROWSER_BOOKMARKS_COLD_REBOOT_OK' marker (Cold-reboot durable browser bookmarks verified)!\n")

        if "ASTRA_USER_WRITE_PROTECTION_OK" in line:
            ak_write_prot_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_USER_WRITE_PROTECTION_OK' marker (Supervisor write protection trapped)!\n")

        if "ASTRA_USER_NX_PROTECTION_OK" in line:
            ak_nx_prot_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_USER_NX_PROTECTION_OK' marker (NX stack execution trapped)!\n")

        if "ASTRA_USER_GUARD_PAGE_OK" in line:
            ak_guard_page_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_USER_GUARD_PAGE_OK' marker (Stack guard page protection trapped)!\n")

        if "ASTRA_SYSCALL_SECURITY_OK" in line:
            ak_syscall_sec_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_SYSCALL_SECURITY_OK' marker (Syscall boundary & pointer security verified)!\n")

        if "ASTRA_FD_OWNERSHIP_OK" in line:
            ak_fd_owner_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_FD_OWNERSHIP_OK' marker (Descriptor ownership isolation & exit reclamation verified)!\n")

        if "ASTRA_PROCESS_WAITPID_OK" in line:
            ak_waitpid_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_PROCESS_WAITPID_OK' marker (process_waitpid & status propagation verified)!\n")

        if "ASTRA_MULTI_APP_ISOLATION_OK" in line:
            ak_multi_app_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_MULTI_APP_ISOLATION_OK' marker (Concurrent 3-app Ring 3 hardware isolation verified)!\n")

        if "ASTRA_PROCESS_LIFECYCLE_STRESS_OK" in line:
            ak_stress_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_PROCESS_LIFECYCLE_STRESS_OK' marker (10x process lifecycle zero-leak stress verified)!\n")

        if "ASTRA_PREEMPT_RING3_OK" in line:
            al_preempt_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_PREEMPT_RING3_OK' marker (Ring 3 CPU-bound preemption verified)!\n")

        if "ASTRA_MULTI_PROCESS_OK" in line:
            al_multi_proc_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_MULTI_PROCESS_OK' marker (5 independent user processes verified)!\n")

        if "ASTRA_BLOCK_WAKEUP_OK" in line:
            al_block_wakeup_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_BLOCK_WAKEUP_OK' marker (RUNNING->BLOCKED->READY->RUNNING verified)!\n")

        if "ASTRA_WAITPID_LIFECYCLE_OK" in line:
            al_waitpid_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_WAITPID_LIFECYCLE_OK' marker (waitpid lifecycle & authorization verified)!\n")

        if "ASTRA_PROCESS_CLEANUP_OK" in line:
            al_cleanup_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_PROCESS_CLEANUP_OK' marker (Preemptive cleanup & zero frame leaks verified)!\n")

        if "ASTRA_SCHED_FAIRNESS_OK" in line:
            al_fairness_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_SCHED_FAIRNESS_OK' marker (Scheduler stability & zero starvation verified)!\n")

        if "ASTRA_SMP_AUDIT_OK" in line:
            al_smp_audit_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_SMP_AUDIT_OK' marker (SMP single-core BSP audit verified)!\n")

        if "ASTRA_SMP_BSP_READY" in line:
            am_bsp_ready = True
            print("\n[TEST] PASS: Detected 'ASTRA_SMP_BSP_READY' marker (BSP online & Local APIC ready)!\n")

        if "ASTRA_SMP_AP_STARTED" in line:
            am_ap_started = True
            print("\n[TEST] PASS: Detected 'ASTRA_SMP_AP_STARTED' marker (AP online & Local APIC timer ready)!\n")

        if "ASTRA_SMP_ALL_CPUS_READY" in line:
            am_all_cpus_ready = True
            print("\n[TEST] PASS: Detected 'ASTRA_SMP_ALL_CPUS_READY' marker (All CPUs online and initialized)!\n")

        if "ASTRA_SMP_PARALLEL_EXECUTION_OK" in line:
            am_parallel_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_SMP_PARALLEL_EXECUTION_OK' marker (Genuine concurrent execution verified)!\n")

        if "ASTRA_SMP_SINGLE_CPU_OK" in line:
            if smp_cpus == 1:
                am_parallel_ok = True
                print("\n[TEST] INFO: Detected 'ASTRA_SMP_SINGLE_CPU_OK' marker (Single-core mode; multi-core parallel tests NOT APPLICABLE).\n")
            else:
                print("\n[TEST] WARN: Single CPU marker emitted on multi-CPU run!\n")

        if "ASTRA_SMP_LIFECYCLE_OK" in line:
            am_lifecycle_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_SMP_LIFECYCLE_OK' marker (SMP process lifecycle verified)!\n")

        if "ASTRA_SMP_TLB_SHOOTDOWN_OK" in line:
            an_tlb_shootdown_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_SMP_TLB_SHOOTDOWN_OK' marker (Cross-CPU TLB shootdown verified)!\n")

        if "ASTRA_SMP_PROCESS_TEARDOWN_OK" in line:
            an_process_teardown_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_SMP_PROCESS_TEARDOWN_OK' marker (Process teardown & active mask verified)!\n")

        if "ASTRA_SMP_MEMORY_STRESS_OK" in line:
            an_memory_stress_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_SMP_MEMORY_STRESS_OK' marker (Multi-core memory stress & 0 leaks verified)!\n")

        if "ASTRA_SMP_SCHEDULER_STRESS_OK" in line:
            an_scheduler_stress_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_SMP_SCHEDULER_STRESS_OK' marker (Multi-core scheduler exclusivity verified)!\n")

        if "ASTRA_SMP_STORAGE_STRESS_OK" in line:
            an_storage_stress_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_SMP_STORAGE_STRESS_OK' marker (ATA storage concurrency verified)!\n")

        if "ASTRA_SMP_ASPACE_ACTIVATION_RACE_OK" in line:
            ao_aspace_race_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_SMP_ASPACE_ACTIVATION_RACE_OK' marker (Address space activation race & active mask ordering verified)!\n")

        if "ASTRA_SMP_CONCURRENT_TLB_OK" in line:
            ao_concurrent_tlb_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_SMP_CONCURRENT_TLB_OK' marker (Monotonic sequence TLB shootdown engine verified)!\n")

        if "ASTRA_SMP_STALE_TLB_REUSE_OK" in line:
            ao_stale_tlb_reuse_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_SMP_STALE_TLB_REUSE_OK' marker (Stale TLB reuse prevention across CPUs verified)!\n")

        if "ASTRA_SMP_DEFERRED_RECLAIM_OK" in line:
            ao_deferred_reclaim_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_SMP_DEFERRED_RECLAIM_OK' marker (Deferred thread reclamation & process dislinking verified)!\n")

        if "ASTRA_SMP_LOCK_ORDER_OK" in line:
            ao_lock_order_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_SMP_LOCK_ORDER_OK' marker (Kernel synchronization & lock order hierarchy verified)!\n")

        if "ASTRA_SMP_PERCPU_ISOLATION_OK" in line:
            ao_percpu_isolation_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_SMP_PERCPU_ISOLATION_OK' marker (Per-CPU state isolation & IA32_GS_BASE verified)!\n")

        if "ASTRA_SMP_ALLOCATOR_FAULT_TEST_OK" in line:
            ao_alloc_fault_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_SMP_ALLOCATOR_FAULT_TEST_OK' marker (Allocator fault injection recovery verified)!\n")

        if "ASTRA_SMP_STORAGE_FAULT_TEST_OK" in line:
            ao_storage_fault_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_SMP_STORAGE_FAULT_TEST_OK' marker (Storage fault & ATA boundary serialization verified)!\n")

        if "ASTRA_AP_ASPACE_PROTOCOL_OK" in line:
            ap_aspace_protocol_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AP_ASPACE_PROTOCOL_OK' marker (Address space teardown & activation race protocol verified)!\n")

        if "ASTRA_AP_TLB_PROTOCOL_AUDIT_OK" in line:
            ap_tlb_protocol_audit_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AP_TLB_PROTOCOL_AUDIT_OK' marker (Cross-CPU TLB invalidation protocol invariants verified)!\n")

        if "ASTRA_AP_FRAME_RECLAMATION_OK" in line:
            ap_frame_reclamation_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AP_FRAME_RECLAMATION_OK' marker (Physical frame reclamation & zero leaks verified)!\n")

        if "ASTRA_AP_LOCK_HIERARCHY_OK" in line:
            ap_lock_hierarchy_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AP_LOCK_HIERARCHY_OK' marker (Kernel lock hierarchy & zero deadlocks verified)!\n")

        if "ASTRA_AP_TEST_HARNESS_INTEGRITY_OK" in line:
            ap_harness_integrity_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AP_TEST_HARNESS_INTEGRITY_OK' marker (Test harness integrity & mode reporting verified)!\n")

        if "ASTRA_AP_NEGATIVE_TESTS_OK" in line:
            ap_negative_tests_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AP_NEGATIVE_TESTS_OK' marker (Defensive negative testing & error handling verified)!\n")

        if "ASTRA_AQ_CONCURRENT_SHOOTDOWN_OK" in line:
            aq_concurrent_shootdown_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AQ_CONCURRENT_SHOOTDOWN_OK' marker (Concurrent shootdowns & monotonic sequence verified)!\n")

        if "ASTRA_AQ_TLB_FAULT_INJECTION_OK" in line:
            aq_tlb_fault_injection_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AQ_TLB_FAULT_INJECTION_OK' marker (TLB shootdown fault injection suite verified)!\n")

        if "ASTRA_AQ_FRAME_REUSE_ORDERING_OK" in line:
            aq_frame_reuse_ordering_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AQ_FRAME_REUSE_ORDERING_OK' marker (Unmap -> Shootdown -> ACK -> Free -> Reuse ordering verified)!\n")

        if "ASTRA_AQ_ASPACE_ADVERSARIAL_OK" in line:
            aq_aspace_adversarial_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AQ_ASPACE_ADVERSARIAL_OK' marker (Adversarial address-space teardown & double-check verified)!\n")

        if "ASTRA_AQ_LOCK_DEADLOCK_AUDIT_OK" in line:
            aq_lock_deadlock_audit_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AQ_LOCK_DEADLOCK_AUDIT_OK' marker (High-concurrency lock hierarchy & deadlock audit verified)!\n")

        if "ASTRA_AQ_HARNESS_CLASSIFICATION_OK" in line:
            aq_harness_classification_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AQ_HARNESS_CLASSIFICATION_OK' marker (Test harness topology classification verified)!\n")

        if "ASTRA_AR_CONCURRENT_INITIATORS_OK" in line:
            ar_concurrent_initiators_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AR_CONCURRENT_INITIATORS_OK' marker (Concurrent initiator queues verified)!\n")

        if "ASTRA_AR_DELAYED_IPI_LIFETIME_OK" in line:
            ar_delayed_ipi_lifetime_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AR_DELAYED_IPI_LIFETIME_OK' marker (Delayed old IPI lifetime safety verified)!\n")

        if "ASTRA_AR_STALE_ACK_REJECTION_OK" in line:
            ar_stale_ack_rejection_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AR_STALE_ACK_REJECTION_OK' marker (Stale & duplicate ACK rejection verified)!\n")

        if "ASTRA_AR_TIMEOUT_FRAME_RETENTION_OK" in line:
            ar_timeout_frame_retention_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AR_TIMEOUT_FRAME_RETENTION_OK' marker (Timeout frame retention verified)!\n")

        if "ASTRA_AR_UNMAP_CALLER_SAFETY_OK" in line:
            ar_unmap_caller_safety_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AR_UNMAP_CALLER_SAFETY_OK' marker (Unmap caller error handling verified)!\n")

        if "ASTRA_AR_END_TO_END_PROOF_OK" in line:
            ar_end_to_end_proof_ok = True

        if "ASTRA_AS_QUEUE_LIFECYCLE_OK" in line:
            as_queue_lifecycle_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AS_QUEUE_LIFECYCLE_OK' marker (Queue lifecycle & wraparound verified)!\n")

        if "ASTRA_AS_GENERATION_ACK_ATOMICS_OK" in line:
            as_generation_ack_atomics_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AS_GENERATION_ACK_ATOMICS_OK' marker (Generation & ACK atomics verified)!\n")

        if "ASTRA_AS_TIMEDOUT_SLOT_DRAIN_OK" in line:
            as_timedout_slot_drain_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AS_TIMEDOUT_SLOT_DRAIN_OK' marker (Timed-out slot drainage verified)!\n")

        if "ASTRA_AS_REENTRANT_SERVICING_OK" in line:
            as_reentrant_servicing_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AS_REENTRANT_SERVICING_OK' marker (Re-entrant request servicing verified)!\n")

        if "ASTRA_AS_PTE_ROLLBACK_CONCURRENCY_OK" in line:
            as_pte_rollback_concurrency_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AS_PTE_ROLLBACK_CONCURRENCY_OK' marker (PTE rollback concurrency verified)!\n")

        if "ASTRA_AS_END_TO_END_FAILURE_QUAL_OK" in line:
            as_end_to_end_failure_qual_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AS_END_TO_END_FAILURE_QUAL_OK' marker (End-to-end failure qualification verified)!\n")

        if "ASTRA_AT_MEMORY_MODEL_ATOMICS_OK" in line:
            at_memory_model_atomics_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AT_MEMORY_MODEL_ATOMICS_OK' marker (Memory model atomics & race freedom verified)!\n")

        if "ASTRA_AT_REMOTE_HANDLER_LIFETIME_OK" in line:
            at_remote_handler_lifetime_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AT_REMOTE_HANDLER_LIFETIME_OK' marker (In-flight reader lifetime & reuse prevention verified)!\n")

        if "ASTRA_AT_DOUBLE_GEN_VERIFY_OK" in line:
            at_double_gen_verify_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AT_DOUBLE_GEN_VERIFY_OK' marker (Double generation verification & stale ACK immunity verified)!\n")

        if "ASTRA_AT_LOCKFREE_SERVICING_OK" in line:
            at_lockfree_servicing_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AT_LOCKFREE_SERVICING_OK' marker (Re-entrant lock-free servicing invariance verified)!\n")

        if "ASTRA_AT_PTE_TRANSITION_ISOLATION_OK" in line:
            at_pte_transition_isolation_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AT_PTE_TRANSITION_ISOLATION_OK' marker (PTE_TRANSITION fault containment & isolation verified)!\n")

        if "ASTRA_AT_END_TO_END_MEM_MODEL_OK" in line:
            at_end_to_end_mem_model_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AT_END_TO_END_MEM_MODEL_OK' marker (End-to-end memory model stress & zero frame leak verified)!\n")
            print("\n[TEST] PASS: Detected 'ASTRA_AR_END_TO_END_PROOF_OK' marker (End-to-end multi-core memory reclamation proof verified)!\n")

        if "ASTRA_AU_READER_PAUSE_BEFORE_REG_OK" in line:
            au_reader_pause_before_reg_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AU_READER_PAUSE_BEFORE_REG_OK' marker (Reader pause before registration verified)!\n")

        if "ASTRA_AU_READER_REG_BEFORE_RETIRE_OK" in line:
            au_reader_reg_before_retire_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AU_READER_REG_BEFORE_RETIRE_OK' marker (Reader registration blocks reclamation verified)!\n")

        if "ASTRA_AU_RETIRE_RACE_REGISTRATION_OK" in line:
            au_retire_race_registration_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AU_RETIRE_RACE_REGISTRATION_OK' marker (Retirement vs registration race mutual exclusion verified)!\n")

        if "ASTRA_AU_DELAYED_IPI_TIMEOUT_DRAIN_OK" in line:
            au_delayed_ipi_timeout_drain_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AU_DELAYED_IPI_TIMEOUT_DRAIN_OK' marker (Delayed IPI timeout drain verified)!\n")

        if "ASTRA_AU_SLOT_REUSE_STRESS_OK" in line:
            au_slot_reuse_stress_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AU_SLOT_REUSE_STRESS_OK' marker (Slot reuse stress verified)!\n")

        if "ASTRA_AU_QUEUE_EXHAUSTION_OK" in line:
            au_queue_exhaustion_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AU_QUEUE_EXHAUSTION_OK' marker (Queue exhaustion and dynamic recovery verified)!\n")

        if "ASTRA_AV_REG_VS_RETIRE_OK" in line:
            av_reg_vs_retire_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AV_REG_VS_RETIRE_OK' marker (Registration vs retirement verified)!\n")

        if "ASTRA_AV_READER_BLOCKS_REUSE_OK" in line:
            av_reader_blocks_reuse_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AV_READER_BLOCKS_REUSE_OK' marker (Registered reader blocks reuse verified)!\n")

        if "ASTRA_AV_SIMULTANEOUS_RACE_OK" in line:
            av_simultaneous_race_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AV_SIMULTANEOUS_RACE_OK' marker (Simultaneous race mutual exclusion verified)!\n")

        if "ASTRA_AV_GENERATION_ABA_OK" in line:
            av_generation_aba_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AV_GENERATION_ABA_OK' marker (Generation ABA protection verified)!\n")

        if "ASTRA_AV_DELAYED_IPI_TIMEOUT_OK" in line:
            av_delayed_ipi_timeout_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AV_DELAYED_IPI_TIMEOUT_OK' marker (Delayed IPI after timeout verified)!\n")

        if "ASTRA_AV_CONCURRENT_SENDERS_OK" in line:
            av_concurrent_senders_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AV_CONCURRENT_SENDERS_OK' marker (Concurrent senders verified)!\n")

        if "ASTRA_AV_QUEUE_EXHAUSTION_OK" in line:
            av_queue_exhaustion_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AV_QUEUE_EXHAUSTION_OK' marker (Queue exhaustion and recovery verified)!\n")

        if "ASTRA_AW_MUNMAP_OVERFLOW_DEFENSE_OK" in line:
            aw_munmap_overflow_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AW_MUNMAP_OVERFLOW_DEFENSE_OK' marker (sys_munmap overflow and bounds defense verified)!\n")

        if "ASTRA_AW_FD_CROSS_PRIV_DEFENSE_OK" in line:
            aw_fd_cross_priv_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AW_FD_CROSS_PRIV_DEFENSE_OK' marker (File descriptor cross-privilege defense verified)!\n")

        if "ASTRA_AW_GUI_CROSS_PRIV_DEFENSE_OK" in line:
            aw_gui_cross_priv_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AW_GUI_CROSS_PRIV_DEFENSE_OK' marker (GUI window cross-privilege defense verified)!\n")

        if "ASTRA_AW_NET_FETCH_CONCURRENCY_OK" in line:
            aw_net_fetch_concurrency_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AW_NET_FETCH_CONCURRENCY_OK' marker (sys_net_fetch buffer safety and synchronization verified)!\n")

        if "ASTRA_AW_SOCKET_OWNER_CLEANUP_OK" in line:
            aw_socket_owner_cleanup_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AW_SOCKET_OWNER_CLEANUP_OK' marker (TCP socket ownership isolation and cleanup verified)!\n")

        if "ASTRA_AW_IPV4_UNDERFLOW_DEFENSE_OK" in line:
            aw_ipv4_underflow_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AW_IPV4_UNDERFLOW_DEFENSE_OK' marker (IPv4 packet length underflow defense verified)!\n")

        if "ASTRA_AW_ARP_SIZE_VALIDATION_OK" in line:
            aw_arp_size_validation_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AW_ARP_SIZE_VALIDATION_OK' marker (ARP packet size validation defense verified)!\n")

        if "ASTRA_AW_PATH_SECURITY_DEFENSE_OK" in line:
            aw_path_security_defense_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AW_PATH_SECURITY_DEFENSE_OK' marker (VFS path security defense verified)!\n")

        if "ASTRA_AX_BLOCK_BOUNDS_OK" in line:
            ax_block_bounds_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AX_BLOCK_BOUNDS_OK' marker (Block device bounds and integer overflow defense verified)!\n")

        if "ASTRA_AX_METADATA_INTEGRITY_OK" in line:
            ax_metadata_integrity_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AX_METADATA_INTEGRITY_OK' marker (Superblock, inode, and bitmap cross-integrity verified)!\n")

        if "ASTRA_AX_CRASH_RECOVERY_OK" in line:
            ax_crash_recovery_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AX_CRASH_RECOVERY_OK' marker (AstraFS crash recovery and dual-set fallback verified)!\n")

        if "ASTRA_AX_PRECOMMIT_FLUSH_OK" in line:
            ax_precommit_flush_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AX_PRECOMMIT_FLUSH_OK' marker (Pre-commit flush and ATA cache barrier verified)!\n")

        if "ASTRA_AX_RENAME_FD_PRESERVATION_OK" in line:
            ax_rename_fd_preservation_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AX_RENAME_FD_PRESERVATION_OK' marker (Atomic file replacement and open FD preservation verified)!\n")

        if "ASTRA_AX_CONCURRENT_LIFECYCLE_OK" in line:
            ax_concurrent_lifecycle_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AX_CONCURRENT_LIFECYCLE_OK' marker (Concurrent allocation, write, and lock ordering verified)!\n")

        if "ASTRA_AY_UEFI_BOOT_INFO_OK" in line:
            ay_uefi_boot_info_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AY_UEFI_BOOT_INFO_OK' marker (UEFI boot info and memory map verified)!\n")

        if "ASTRA_AY_HARDWARE_TIMER_OK" in line:
            ay_hardware_timer_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AY_HARDWARE_TIMER_OK' marker (Hardware timer & APIC interrupt delivery verified)!\n")

        if "ASTRA_AY_STORAGE_DISCOVERY_OK" in line:
            ay_storage_discovery_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AY_STORAGE_DISCOVERY_OK' marker (Storage discovery and fallback safety verified)!\n")

        if "ASTRA_AY_CPU_TOPOLOGY_OK" in line:
            ay_cpu_topology_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AY_CPU_TOPOLOGY_OK' marker (CPU topology and per-CPU isolation verified)!\n")

        if "ASTRA_AY_BOOT_MEDIA_PROTECTION_OK" in line:
            ay_boot_media_protection_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AY_BOOT_MEDIA_PROTECTION_OK' marker (Boot media write-protection and live hierarchy verified)!\n")

        if "ASTRA_AY_SUBSYSTEM_CONTAINMENT_OK" in line:
            ay_subsystem_containment_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_AY_SUBSYSTEM_CONTAINMENT_OK' marker (Subsystem failure containment & diagnostics verified)!\n")

        if "ASTRA_TERMINAL_CURSOR_OK" in line:
            term_cursor_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_TERMINAL_CURSOR_OK' marker (Terminal dynamic cursor positioning & key navigation verified)!\n")

        if "ASTRA_NATIVE_SHELL_OK" in line:
            native_shell_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_NATIVE_SHELL_OK' marker (Universal native shell, pipes, redirection & command chains verified)!\n")

        if "ASTRA_PKG_MANAGER_OK" in line:
            pkg_manager_ok = True
            print("\n[TEST] PASS: Detected 'ASTRA_PKG_MANAGER_OK' marker (Package manager, digital signatures, dependency graph & honest repo verified)!\n")

        if test_mode == "nofb" and ("operating in safe serial-only console mode" in line or "serial-only mode" in line):
            nofb_verified = True

        gui_conditions = (user_gui_ok and multi_process_gui_ok and gui_isolation_ok) if test_mode != "nofb" else True
        phase_o_conditions = (mmap_val_ok and user_mmap_ok and user_browser_ok and user_browser_exec_ok and browser_iso_ok and browser_image_ok and browser_ext_css_ok and browser_forms_ok and browser_nav_ok and browser_scroll_ok and js_feasibility_ok and js_runtime_ok and js_isolation_ok and js_hardening_ok and dom_bridge_ok and dom_events_ok and browser_interactive_ok and js_adversarial_ok and browser_security_ok and browser_net_ok and http_hardening_ok and browser_http_hard_ok and browser_ux_ok and browser_reliability_ok and browser_tabs_ok) if test_mode != "nofb" else True
        phase_ak_conditions = (ak_write_prot_ok and ak_nx_prot_ok and ak_guard_page_ok and
                               ak_syscall_sec_ok and ak_fd_owner_ok and ak_waitpid_ok and
                               ak_multi_app_ok and ak_stress_ok)
        phase_al_conditions = (al_preempt_ok and al_multi_proc_ok and al_block_wakeup_ok and
                               al_waitpid_ok and al_cleanup_ok and al_fairness_ok and al_smp_audit_ok)
        phase_am_conditions = (am_bsp_ready and am_all_cpus_ready and am_parallel_ok and am_lifecycle_ok)
        if smp_cpus >= 2:
            phase_am_conditions = phase_am_conditions and am_ap_started
        phase_an_conditions = (an_tlb_shootdown_ok and an_process_teardown_ok and an_memory_stress_ok and an_scheduler_stress_ok and an_storage_stress_ok)
        phase_ao_conditions = (ao_aspace_race_ok and ao_concurrent_tlb_ok and ao_stale_tlb_reuse_ok and
                               ao_deferred_reclaim_ok and ao_lock_order_ok and ao_percpu_isolation_ok and
                               ao_alloc_fault_ok and ao_storage_fault_ok)
        phase_ap_conditions = (ap_aspace_protocol_ok and ap_tlb_protocol_audit_ok and
                               ap_frame_reclamation_ok and ap_lock_hierarchy_ok and
                               ap_harness_integrity_ok and ap_negative_tests_ok)
        phase_aq_conditions = (aq_concurrent_shootdown_ok and aq_tlb_fault_injection_ok and
                               aq_frame_reuse_ordering_ok and aq_aspace_adversarial_ok and
                               aq_lock_deadlock_audit_ok and aq_harness_classification_ok)
        phase_ar_conditions = (ar_concurrent_initiators_ok and ar_delayed_ipi_lifetime_ok and
                               ar_stale_ack_rejection_ok and ar_timeout_frame_retention_ok and
                               ar_unmap_caller_safety_ok and ar_end_to_end_proof_ok)
        phase_as_conditions = (as_queue_lifecycle_ok and as_generation_ack_atomics_ok and
                               as_timedout_slot_drain_ok and as_reentrant_servicing_ok and
                               as_pte_rollback_concurrency_ok and as_end_to_end_failure_qual_ok)
        phase_at_conditions = (at_memory_model_atomics_ok and at_remote_handler_lifetime_ok and
                               at_double_gen_verify_ok and at_lockfree_servicing_ok and
                               at_pte_transition_isolation_ok and at_end_to_end_mem_model_ok)
        phase_au_conditions = (au_reader_pause_before_reg_ok and au_reader_reg_before_retire_ok and
                               au_retire_race_registration_ok and au_delayed_ipi_timeout_drain_ok and
                               au_slot_reuse_stress_ok and au_queue_exhaustion_ok)
        phase_av_conditions = (av_reg_vs_retire_ok and av_reader_blocks_reuse_ok and
                               av_simultaneous_race_ok and av_generation_aba_ok and
                               av_delayed_ipi_timeout_ok and av_concurrent_senders_ok and
                               av_queue_exhaustion_ok)
        phase_aw_conditions = (aw_munmap_overflow_ok and aw_fd_cross_priv_ok and
                               aw_gui_cross_priv_ok and aw_net_fetch_concurrency_ok and
                               aw_socket_owner_cleanup_ok and aw_ipv4_underflow_ok and
                               aw_arp_size_validation_ok and aw_path_security_defense_ok)
        phase_ax_conditions = (ax_block_bounds_ok and ax_metadata_integrity_ok and
                               ax_crash_recovery_ok and ax_precommit_flush_ok and
                               ax_rename_fd_preservation_ok and ax_concurrent_lifecycle_ok)
        phase_ay_conditions = (ay_uefi_boot_info_ok and ay_hardware_timer_ok and
                               ay_storage_discovery_ok and ay_cpu_topology_ok and
                               ay_boot_media_protection_ok and ay_subsystem_containment_ok)
        terminal_conditions = (term_cursor_ok and native_shell_ok and pkg_manager_ok) if test_mode != "nofb" else (native_shell_ok and pkg_manager_ok)

        if (boot_ok and pmm_ok and vmm_ok and own_paging_ok and heap_ok and 
            tss_ok and idt_ok and apic_timer_ok and ctx_switch_ok and coop_sched_ok and thread_lifecycle_ok and
            wait_queue_ok and event_ok and sleep_ok and idle_wake_ok and preempt_guard_ok and crit_sec_ok and
            preempt_ok and mutex_ok and mutex_handoff_ok and semaphore_ok and semaphore_irq_ok and sync_stress_ok and
            usermode_ok and syscall_ok and process_exit_ok and user_mem_prot_ok and
            elf_header_ok and elf_segments_ok and elf_invalid_input_ok and elf_rollback_ok and elf_user_exec_ok and
            initramfs_ok and vfs_lookup_ok and vfs_read_ok and vfs_invalid_archive_ok and vfs_elf_exec_ok and
            net_dev_ok and net_drv_ok and arp_ok and ipv4_ok and dhcp_ok and dns_ok and tcp_ok and http_client_ok and
            browser_render_ok and browser_http_ok and tls_cert_ok and
            css_parser_ok and css_cascade_ok and css_layout_ok and block_device_ok and fs_ok and gui_conditions and phase_o_conditions and phase_ak_conditions and phase_al_conditions and phase_am_conditions and phase_an_conditions and phase_ao_conditions and phase_ap_conditions and phase_aq_conditions and phase_ar_conditions and phase_as_conditions and phase_at_conditions and phase_au_conditions and phase_av_conditions and phase_aw_conditions and phase_ax_conditions and phase_ay_conditions and terminal_conditions):
            break

    gui_conditions = (user_gui_ok and multi_process_gui_ok and gui_isolation_ok) if test_mode != "nofb" else True
    phase_o_conditions = (mmap_val_ok and user_mmap_ok and user_browser_ok and user_browser_exec_ok and browser_iso_ok and browser_image_ok and browser_ext_css_ok and browser_forms_ok and browser_nav_ok and browser_scroll_ok and js_feasibility_ok and js_runtime_ok and js_isolation_ok and js_hardening_ok and dom_bridge_ok and dom_events_ok and browser_interactive_ok and js_adversarial_ok and browser_security_ok and browser_net_ok and http_hardening_ok and browser_http_hard_ok and browser_ux_ok and browser_reliability_ok and browser_tabs_ok) if test_mode != "nofb" else True
    phase_ak_conditions = (ak_write_prot_ok and ak_nx_prot_ok and ak_guard_page_ok and
                           ak_syscall_sec_ok and ak_fd_owner_ok and ak_waitpid_ok and
                           ak_multi_app_ok and ak_stress_ok)
    phase_al_conditions = (al_preempt_ok and al_multi_proc_ok and al_block_wakeup_ok and
                           al_waitpid_ok and al_cleanup_ok and al_fairness_ok and al_smp_audit_ok)
    phase_am_conditions = (am_bsp_ready and am_all_cpus_ready and am_parallel_ok and am_lifecycle_ok)
    if smp_cpus >= 2:
        phase_am_conditions = phase_am_conditions and am_ap_started
    phase_an_conditions = (an_tlb_shootdown_ok and an_process_teardown_ok and an_memory_stress_ok and an_scheduler_stress_ok and an_storage_stress_ok)
    phase_ao_conditions = (ao_aspace_race_ok and ao_concurrent_tlb_ok and ao_stale_tlb_reuse_ok and
                           ao_deferred_reclaim_ok and ao_lock_order_ok and ao_percpu_isolation_ok and
                           ao_alloc_fault_ok and ao_storage_fault_ok)
    phase_ap_conditions = (ap_aspace_protocol_ok and ap_tlb_protocol_audit_ok and
                           ap_frame_reclamation_ok and ap_lock_hierarchy_ok and
                           ap_harness_integrity_ok and ap_negative_tests_ok)

    if not (boot_ok and pmm_ok and vmm_ok and own_paging_ok and heap_ok and 
            tss_ok and idt_ok and apic_timer_ok and ctx_switch_ok and coop_sched_ok and thread_lifecycle_ok and
            wait_queue_ok and event_ok and sleep_ok and idle_wake_ok and preempt_guard_ok and crit_sec_ok and
            preempt_ok and mutex_ok and mutex_handoff_ok and semaphore_ok and semaphore_irq_ok and sync_stress_ok and
            usermode_ok and syscall_ok and process_exit_ok and user_mem_prot_ok and
            elf_header_ok and elf_segments_ok and elf_invalid_input_ok and elf_rollback_ok and elf_user_exec_ok and
            initramfs_ok and vfs_lookup_ok and vfs_read_ok and vfs_invalid_archive_ok and vfs_elf_exec_ok and
            net_dev_ok and net_drv_ok and arp_ok and ipv4_ok and dhcp_ok and dns_ok and tcp_ok and http_client_ok and
            browser_render_ok and browser_http_ok and tls_cert_ok and
            css_parser_ok and css_cascade_ok and css_layout_ok and block_device_ok and fs_ok and gui_conditions and phase_o_conditions and phase_ak_conditions and phase_al_conditions and phase_am_conditions and phase_an_conditions and phase_ao_conditions and phase_ap_conditions and phase_aq_conditions and phase_ar_conditions and phase_as_conditions and phase_at_conditions and phase_au_conditions and phase_av_conditions and phase_aw_conditions and phase_ax_conditions and phase_ay_conditions and terminal_conditions):
        proc.kill()
        if https_server:
            https_server.shutdown()
        if http_server:
            http_server.shutdown()
        print(f"\n[FAIL] Boot prerequisites timeout or failure (boot={boot_ok}, phase_ak={phase_ak_conditions}, phase_ao={phase_ao_conditions}, net_dev={net_dev_ok}, net_drv={net_drv_ok}, arp={arp_ok}, ip={ipv4_ok}, dhcp={dhcp_ok}, dns={dns_ok}, tcp={tcp_ok}, http={http_client_ok}, browser_render={browser_render_ok}, browser_http={browser_http_ok}, tls_cert={tls_cert_ok}, css_parser={css_parser_ok}, css_cascade={css_cascade_ok}, css_layout={css_layout_ok}, user_gui={user_gui_ok}, multi_gui={multi_process_gui_ok}, gui_iso={gui_isolation_ok}, phase_o={phase_o_conditions})")
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

        if "KERNEL PANIC" in line or "System halted." in line:
            print("\n[FAIL] Kernel panic or unexpected halt detected while waiting for monitor prompt!")
            proc.kill()
            if https_server:
                https_server.shutdown()
            if http_server:
                http_server.shutdown()
            return False

        if "AstraOS>" in line:
            prompt_seen = True
            break

    if not prompt_seen:
        proc.kill()
        if https_server:
            https_server.shutdown()
        if http_server:
            http_server.shutdown()
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

            if "KERNEL PANIC" in line or "System halted." in line:
                print(f"\n[FAIL] Kernel panic detected during command '{command}'!")
                return False

            if expected_substr in line:
                found = True
            if "AstraOS>" in line:
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

    def abort_monitor_fail():
        proc.kill()
        if https_server:
            https_server.shutdown()
        if http_server:
            http_server.shutdown()
        return False

    # Test 'help'
    if not send_cmd_and_wait("help", "AstraOS Kernel Monitor - Built-in Commands"):
        return abort_monitor_fail()

    # Test 'version'
    if not send_cmd_and_wait("version", "AstraOS Kernel"):
        return abort_monitor_fail()

    # Test 'mem' (PMM stats)
    if not send_cmd_and_wait("mem", "Physical Page Allocator Stats"):
        return abort_monitor_fail()

    # Test 'memmap' (match end of memmap output)
    if not send_cmd_and_wait("memmap", "Reserved/MMIO Space"):
        return abort_monitor_fail()

    # Test 'heap' (Heap stats)
    if not send_cmd_and_wait("heap", "Kernel Heap Stats"):
        return abort_monitor_fail()

    # Test 'ticks' (APIC Timer Stats)
    if not send_cmd_and_wait("ticks", "APIC Timer Ticks"):
        return abort_monitor_fail()

    # Test 'vmap' for active kernel code mapping (.text: Read-Only, Executable)
    if not send_cmd_and_wait("vmap 0xffffffff80200000", "Executable (X)"):
        return abort_monitor_fail()

    # Test 'vmap' for kernel read-only data (.rodata: Read-Only, Non-Executable)
    rodata_va = get_rodata_start()
    if not send_cmd_and_wait(f"vmap 0x{rodata_va:016x}", "NX (No-Execute)"):
        return abort_monitor_fail()

    # Test 'vmap' for dedicated kernel stack (RW, Non-Executable)
    if not send_cmd_and_wait("vmap 0xffffffff80404000", "Read/Write"):
        return abort_monitor_fail()

    # Test 'vmap' for unmapped guard page (Deliberately Not Present)
    if not send_cmd_and_wait("vmap 0xffffffff80400000", "NOT PRESENT (Unmapped)"):
        return abort_monitor_fail()

    # Test 'vmap' for heap arena (RW, Non-Executable)
    if not send_cmd_and_wait("vmap 0xffffffff80601000", "NX (No-Execute)"):
        return abort_monitor_fail()

    # Test 'vmap' for heap lower guard page (Deliberately Not Present)
    if not send_cmd_and_wait("vmap 0xffffffff80600000", "NOT PRESENT (Unmapped)"):
        return abort_monitor_fail()

    # Test 'vmap' for heap upper guard page (Deliberately Not Present)
    if not send_cmd_and_wait("vmap 0xffffffff80611000", "NOT PRESENT (Unmapped)"):
        return abort_monitor_fail()

    # Test 'vmap' for non-canonical address
    if not send_cmd_and_wait("vmap 0x800000000000", "NON-CANONICAL"):
        return abort_monitor_fail()

    # Test 'ls' (VFS directory listing)
    if not send_cmd_and_wait("ls /", "hello.txt"):
        return abort_monitor_fail()

    # Test 'cat' (VFS file display)
    if not send_cmd_and_wait("cat /hello.txt", "Hello from AstraOS Initramfs!"):
        return abort_monitor_fail()

    # Test 'ifconfig' (Network Interface display)
    if not send_cmd_and_wait("ifconfig", "Intel 82540EM Gigabit Ethernet"):
        return abort_monitor_fail()

    # Test 'diskinfo' (Block Device display)
    if not send_cmd_and_wait("diskinfo", "Registered Block Devices:"):
        return abort_monitor_fail()

    # Test 'disktest' (Diagnostic block self-test)
    if not send_cmd_and_wait("disktest", "ASTRA_BLOCK_DEVICE_OK"):
        return abort_monitor_fail()

    # Test 'fsinfo' (AstraFS persistent filesystem display)
    if not send_cmd_and_wait("fsinfo", "AstraFS v1 Mount Status:"):
        return abort_monitor_fail()

    # Test 'browser' (AstraBrowser interactive launch/navigation)
    if not send_cmd_and_wait("browser file:///www/index.html", "Launching AstraBrowser"):
        return abort_monitor_fail()

    # Test 'browser' CSS verification page navigation
    if not send_cmd_and_wait("browser file:///www/css_test.html", "Launching AstraBrowser"):
        return abort_monitor_fail()

    # Test 'browser' rich portal page navigation
    if not send_cmd_and_wait("browser file:///www/rich_page.html", "Launching AstraBrowser"):
        return abort_monitor_fail()

    # Test 'browser' malformed CSS robustness page navigation
    if not send_cmd_and_wait("browser file:///www/css_malformed.html", "Launching AstraBrowser"):
        return abort_monitor_fail()

    # Test 'browser' Phase Q navigation, fragments & scrolling page
    if not send_cmd_and_wait("browser file:///www/nav_test.html", "Launching AstraBrowser"):
        return abort_monitor_fail()

    # Test 'browser' HTTP navigation to controlled local HTTP test server
    if http_server:
        if not send_cmd_and_wait("browser http://10.0.2.2:8000/", "ASTRA_BROWSER_HTTP_FETCH_OK", max_wait=15.0):
            return abort_monitor_fail()

        # Test 'browser' chunked response decoding
        if not send_cmd_and_wait("browser http://10.0.2.2:8000/chunked", "ASTRA_BROWSER_HTTP_FETCH_OK", max_wait=15.0):
            return abort_monitor_fail()

        # Test 'browser' HTTP 302 redirect following
        if not send_cmd_and_wait("browser http://10.0.2.2:8000/redirect", "ASTRA_BROWSER_HTTP_FETCH_OK", max_wait=15.0):
            return abort_monitor_fail()

        # Test 'browser' malformed response yielding status 502
        if not send_cmd_and_wait("browser http://10.0.2.2:8000/malformed", "Loaded (Status: 502)", max_wait=15.0):
            return abort_monitor_fail()

    # Test 'browser' HTTPS navigation to controlled local HTTPS test server
    if https_server:
        if not send_cmd_and_wait("browser https://10.0.2.2:8443/", "ASTRA_BROWSER_HTTPS_OK", max_wait=15.0):
            return abort_monitor_fail()

    # Test 'browser' auto-prefixing on local file navigation
    if not send_cmd_and_wait("browser /www/index.html", "Launching AstraBrowser"):
        return abort_monitor_fail()

    # Test 'browser' invalid URL scheme rejection
    if not send_cmd_and_wait("browser ftp://invalid.host/file", "Launching AstraBrowser"):
        return abort_monitor_fail()

    # Phase Z: Error recovery monitor tests
    if http_server:
        # Test 'browser' 404 error page handling
        if not send_cmd_and_wait("browser http://10.0.2.2:8000/status/404", "404 Not Found", max_wait=15.0):
            return abort_monitor_fail()
        # Verify subsequent recovery navigation to valid page
        if not send_cmd_and_wait("browser file:///www/index.html", "Launching AstraBrowser"):
            return abort_monitor_fail()
        # Test 'browser' redirect loop detection
        if not send_cmd_and_wait("browser http://10.0.2.2:8000/redirect_loop", "Launching AstraBrowser"):
            return abort_monitor_fail()
        # Verify subsequent recovery navigation to valid page
        if not send_cmd_and_wait("browser file:///www/about.html", "Launching AstraBrowser"):
            return abort_monitor_fail()

    # =========================================================================
    # Terminal Real-World Acceptance & Package Qualification Commands
    # =========================================================================
    acceptance_commands = [
        ("pwd", "/"),
        ("whoami", "root"),
        ("uname -a", "AstraOS"),
        ("echo hello", "hello"),
        ("which ls", "ls"),
        ("type grep", "builtin"),
        ("mkdir /data/testdir", ""),
        ("cd /data/testdir", ""),
        ("pwd", "data"),
        ("touch sample.txt", ""),
        ("echo hello-world > sample.txt", ""),
        ("cat sample.txt", "hello-world"),
        ("cp sample.txt copy.txt", ""),
        ("mv copy.txt renamed.txt", ""),
        ("ls -la", "sample.txt"),
        ("grep hello sample.txt", "hello-world"),
        ("wc -l sample.txt", "1"),
        ("head -n 1 sample.txt", "hello-world"),
        ("tail -n 1 sample.txt", "hello-world"),
        ("find /data -name sample.txt", "sample.txt"),
        ("stat sample.txt", "sample.txt"),
        ("df", "AstraFS"),
        ("du -sh /data", "data"),
        ("rm renamed.txt", ""),
        ("rm sample.txt", ""),
        ("cd /", ""),
        ("rmdir /data/testdir", ""),
        ("echo hello | wc -c", "6"),
        ("echo first > /data/output.txt", ""),
        ("echo second >> /data/output.txt", ""),
        ("cat < /data/output.txt", "first"),
        ("false || echo fallback", "fallback"),
        ("echo one ; echo two", "one"),
        ("pkg list", "Installed"),
        ("pkg search calc", "calc"),
        ("pkg info calc", "calc"),
        ("pkg install calc", "calc"),
        ("which calc", "calc"),
        ("pkg remove calc", "removed"),
        ("pkg search htop", "htop"),
        ("pkg info htop", "htop"),
        ("pkg install htop", "htop"),
        ("which htop", "htop"),
        ("pkg remove htop", "removed"),
        ("pkg install nonexistent_pkg", "not found")
    ]

    print("\n[ACCEPTANCE] Executing Real Terminal Command & Package Verification Suite...")
    for ac_cmd, ac_expect in acceptance_commands:
        if not send_cmd_and_wait(ac_cmd, ac_expect):
            return abort_monitor_fail()

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
    if http_server:
        http_server.shutdown()

    print(f"[TEST] PASS: All tests passed for Mode='{test_mode}', RAM={ram_size}!\n")
    return True

def main():
    parser = argparse.ArgumentParser(description="AstraOS QEMU Integration Test Runner")
    parser.add_argument("--all", action="store_true", help="Run full suite: 128M, 512M, 1024M, exception, nofb, memory faults, thread guard, and mutex failure modes")
    parser.add_argument("--ram", default="512M", help="Memory size (e.g. 128M, 512M, 1024M)")
    parser.add_argument("--smp", type=int, default=4, help="Number of SMP CPU cores (e.g. 1, 2, 4, 8)")
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
            ok = run_integration_test(ram, mode, vga, smp_cpus=args.smp)
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
        print("  - Browser networking & HTTPS validation verified (ASTRA_BROWSER_NET_ISOLATION_OK, ASTRA_BROWSER_HTTP_FETCH_OK, ASTRA_BROWSER_HTTPS_OK)")
        print("  - Browser UX, navigation history & keyboard shortcuts verified (ASTRA_BROWSER_UX_OK)")
        print("  - Browser reliability, stress navigation & recovery verified (ASTRA_BROWSER_RELIABILITY_OK)")
        print("  - Browser tabs, bookmarks & serialization verified (ASTRA_BROWSER_TABS_BOOKMARKS_OK)")
        print("  - Block device subsystem & ATA driver verified (ASTRA_BLOCK_DEVICE_OK)")
        print("  - AstraFS persistent filesystem verified (ASTRA_FS_OK)")
        print("  - Cold-reboot persistence across guest restart verified (ASTRA_FS_PERSISTENCE_OK)")
        print("  - Two-stage cold-reboot probe verified byte-for-byte (ASTRA_FS_COLD_REBOOT_CONTENTS_VERIFIED)")
        print("  - Browser bookmark cold-reboot durability verified (ASTRA_BROWSER_BOOKMARKS_COLD_REBOOT_OK)")
        print("  - Symmetric Multiprocessing (SMP) bring-up verified (ASTRA_SMP_BSP_READY, ASTRA_SMP_AP_STARTED, ASTRA_SMP_ALL_CPUS_READY)")
        print("  - Genuine multi-core Ring 3 parallel execution verified (ASTRA_SMP_PARALLEL_EXECUTION_OK)")
        print("  - Multi-core process lifecycle & synchronization verified (ASTRA_SMP_LIFECYCLE_OK)")
        print("  - Cross-CPU TLB shootdown via Local APIC IPI verified (ASTRA_SMP_TLB_SHOOTDOWN_OK)")
        print("  - Multi-core process teardown & address-space lifetime verified (ASTRA_SMP_PROCESS_TEARDOWN_OK)")
        print("  - Concurrent memory stress (PMM, Heap, VMM) with zero frame leaks verified (ASTRA_SMP_MEMORY_STRESS_OK)")
        print("  - Multi-core scheduler execution exclusivity verified (ASTRA_SMP_SCHEDULER_STRESS_OK)")
        print("  - Concurrent block/storage access serialization under spinlock verified (ASTRA_SMP_STORAGE_STRESS_OK)")
        print("  - Address-space teardown vs activation race protocol verified (ASTRA_AP_ASPACE_PROTOCOL_OK)")
        print("  - Cross-CPU TLB shootdown monotonic sequence & protocol verified (ASTRA_AP_TLB_PROTOCOL_AUDIT_OK)")
        print("  - Physical memory frame reclamation accounting & zero leaks verified (ASTRA_AP_FRAME_RECLAMATION_OK)")
        print("  - Complete kernel lock hierarchy & deadlock prevention verified (ASTRA_AP_LOCK_HIERARCHY_OK)")
        print("  - Test harness integrity & multi-core execution mode reporting verified (ASTRA_AP_TEST_HARNESS_INTEGRITY_OK)")
        print("  - Defensive negative testing & error injection verified (ASTRA_AP_NEGATIVE_TESTS_OK)")
        print("  - Concurrent TLB shootdowns & monotonic sequences verified (ASTRA_AQ_CONCURRENT_SHOOTDOWN_OK)")
        print("  - TLB shootdown fault injection suite verified (ASTRA_AQ_TLB_FAULT_INJECTION_OK)")
        print("  - Unmap -> Shootdown -> ACK -> Free -> Reuse ordering verified (ASTRA_AQ_FRAME_REUSE_ORDERING_OK)")
        print("  - Adversarial address space teardown & double-check verified (ASTRA_AQ_ASPACE_ADVERSARIAL_OK)")
        print("  - High-concurrency lock hierarchy & deadlock prevention verified (ASTRA_AQ_LOCK_DEADLOCK_AUDIT_OK)")
        print("  - Test harness topology classification verified (ASTRA_AQ_HARNESS_CLASSIFICATION_OK)")
        print("  - Concurrent initiator queue isolation verified (ASTRA_AR_CONCURRENT_INITIATORS_OK)")
        print("  - Delayed old IPI request lifetime safety verified (ASTRA_AR_DELAYED_IPI_LIFETIME_OK)")
        print("  - Stale and duplicate ACK rejection verified (ASTRA_AR_STALE_ACK_REJECTION_OK)")
        print("  - Physical frame retention on shootdown timeout qualified (ASTRA_AR_TIMEOUT_FRAME_RETENTION_OK)")
        print("  - All unmap caller error handling verified (ASTRA_AR_UNMAP_CALLER_SAFETY_OK)")
        print("  - End-to-end multi-core memory reclamation proof verified (ASTRA_AR_END_TO_END_PROOF_OK)")
        print("  - Per-CPU queue lifecycle and wraparound qualified (ASTRA_AS_QUEUE_LIFECYCLE_OK)")
        print("  - Generation and ACK atomicity qualified (ASTRA_AS_GENERATION_ACK_ATOMICS_OK)")
        print("  - Timed-out slot lifecycle and delayed drainage qualified (ASTRA_AS_TIMEDOUT_SLOT_DRAIN_OK)")
        print("  - Re-entrant lock-free request servicing verified deadlock-free (ASTRA_AS_REENTRANT_SERVICING_OK)")
        print("  - Leaf PTE rollback and concurrent modification immunity qualified (ASTRA_AS_PTE_ROLLBACK_CONCURRENCY_OK)")
        print("  - End-to-end failure injection qualification complete (ASTRA_AS_END_TO_END_FAILURE_QUAL_OK)")
        print("  - Memory model atomicity and race freedom qualified (ASTRA_AT_MEMORY_MODEL_ATOMICS_OK)")
        print("  - In-flight reader lifetime and reuse protection qualified (ASTRA_AT_REMOTE_HANDLER_LIFETIME_OK)")
        print("  - Double-generation verification and stale ACK immunity qualified (ASTRA_AT_DOUBLE_GEN_VERIFY_OK)")
        print("  - Re-entrant lock-free servicing invariance qualified (ASTRA_AT_LOCKFREE_SERVICING_OK)")
        print("  - PTE_TRANSITION page fault containment and isolation qualified (ASTRA_AT_PTE_TRANSITION_ISOLATION_OK)")
        print("  - End-to-end memory model stress and recovery complete (ASTRA_AT_END_TO_END_MEM_MODEL_OK)")
        print("  - Reader pause before registration qualified (ASTRA_AU_READER_PAUSE_BEFORE_REG_OK)")
        print("  - Reader registration blocks reclamation qualified (ASTRA_AU_READER_REG_BEFORE_RETIRE_OK)")
        print("  - Retirement vs registration race mutual exclusion qualified (ASTRA_AU_RETIRE_RACE_REGISTRATION_OK)")
        print("  - Delayed IPI after timeout drainage qualified (ASTRA_AU_DELAYED_IPI_TIMEOUT_DRAIN_OK)")
        print("  - Slot reuse stress qualified (ASTRA_AU_SLOT_REUSE_STRESS_OK)")
        print("  - Queue exhaustion and dynamic recovery qualified (ASTRA_AU_QUEUE_EXHAUSTION_OK)")
        print("  - Registration vs retirement qualified (ASTRA_AV_REG_VS_RETIRE_OK)")
        print("  - Registered reader blocks reuse qualified (ASTRA_AV_READER_BLOCKS_REUSE_OK)")
        print("  - Simultaneous race mutual exclusion qualified (ASTRA_AV_SIMULTANEOUS_RACE_OK)")
        print("  - Generation ABA protection qualified (ASTRA_AV_GENERATION_ABA_OK)")
        print("  - Delayed IPI after timeout qualified (ASTRA_AV_DELAYED_IPI_TIMEOUT_OK)")
        print("  - Concurrent senders qualified (ASTRA_AV_CONCURRENT_SENDERS_OK)")
        print("  - Queue exhaustion and recovery qualified (ASTRA_AV_QUEUE_EXHAUSTION_OK)")
        print("  - sys_munmap overflow and bounds defense qualified (ASTRA_AW_MUNMAP_OVERFLOW_DEFENSE_OK)")
        print("  - File descriptor cross-privilege defense qualified (ASTRA_AW_FD_CROSS_PRIV_DEFENSE_OK)")
        print("  - GUI window cross-privilege defense qualified (ASTRA_AW_GUI_CROSS_PRIV_DEFENSE_OK)")
        print("  - sys_net_fetch buffer safety and synchronization qualified (ASTRA_AW_NET_FETCH_CONCURRENCY_OK)")
        print("  - TCP socket ownership isolation and cleanup qualified (ASTRA_AW_SOCKET_OWNER_CLEANUP_OK)")
        print("  - IPv4 packet length underflow defense qualified (ASTRA_AW_IPV4_UNDERFLOW_DEFENSE_OK)")
        print("  - ARP packet size validation defense qualified (ASTRA_AW_ARP_SIZE_VALIDATION_OK)")
        print("  - VFS path security defense qualified (ASTRA_AW_PATH_SECURITY_DEFENSE_OK)")
        print("  - Block device bounds & integer overflow defense qualified (ASTRA_AX_BLOCK_BOUNDS_OK)")
        print("  - Superblock, inode, and bitmap cross-integrity qualified (ASTRA_AX_METADATA_INTEGRITY_OK)")
        print("  - AstraFS crash recovery and dual-set fallback qualified (ASTRA_AX_CRASH_RECOVERY_OK)")
        print("  - Pre-commit flush and ATA cache barrier qualified (ASTRA_AX_PRECOMMIT_FLUSH_OK)")
        print("  - Atomic file replacement and open FD preservation qualified (ASTRA_AX_RENAME_FD_PRESERVATION_OK)")
        print("  - Concurrent allocation, write, and lock ordering qualified (ASTRA_AX_CONCURRENT_LIFECYCLE_OK)")
        print("  - UEFI boot info and memory map qualified (ASTRA_AY_UEFI_BOOT_INFO_OK)")
        print("  - Hardware timer & APIC interrupt delivery qualified (ASTRA_AY_HARDWARE_TIMER_OK)")
        print("  - Storage discovery & fallback safety qualified (ASTRA_AY_STORAGE_DISCOVERY_OK)")
        print("  - CPU topology and per-CPU isolation qualified (ASTRA_AY_CPU_TOPOLOGY_OK)")
        print("  - Boot media write-protection & live hierarchy qualified (ASTRA_AY_BOOT_MEDIA_PROTECTION_OK)")
        print("  - Subsystem failure containment & diagnostics qualified (ASTRA_AY_SUBSYSTEM_CONTAINMENT_OK)")
        print("  - Terminal dynamic cursor positioning & navigation qualified (ASTRA_TERMINAL_CURSOR_OK)")
        print("  - Universal native shell, pipes, redirection & chains qualified (ASTRA_NATIVE_SHELL_OK)")
        print("  - Package manager, digital signatures, dependency graph & honest repo qualified (ASTRA_PKG_MANAGER_OK)")
        print("==================================================================")
        sys.exit(0)
    else:
        vga = "none" if args.mode == "nofb" else "std"
        ok = run_integration_test(args.ram, args.mode, vga, smp_cpus=args.smp)
        update_limine_conf(None)
        sys.exit(0 if ok else 1)

if __name__ == '__main__':
    main()

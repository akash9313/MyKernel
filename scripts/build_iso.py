#!/usr/bin/env python3
"""
build_iso.py - Reproducible ISO generation for AstraOS
Creates a standard ISO 9660 + El Torito bootable CD/DVD image with EFI support.
Contains an embedded FAT16 EFI System Partition image with BOOTX64.EFI, limine.conf,
kernel.elf, and initramfs.bin, compatible with both QEMU and VirtualBox.
"""

import os
import struct
import shutil

BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_DIR = os.path.join(BASE_DIR, 'build')
ESP_DIR = os.path.join(BUILD_DIR, 'esp')
ISO_OUT = os.path.join(BUILD_DIR, 'astraos.iso')

# ----------------------------------------------------------------------
# FAT16 Generation (Pure Python)
# ----------------------------------------------------------------------

def lfn_checksum(short_name_11):
    chk = 0
    for b in short_name_11:
        chk = (((chk & 1) << 7) + (chk >> 1) + b) & 0xFF
    return chk

def create_lfn_entries(long_name, short_name_11):
    chk = lfn_checksum(short_name_11)
    chars = [ord(c) for c in long_name] + [0]  # Null-terminated
    # Pad with 0xFFFF to multiple of 13
    while len(chars) % 13 != 0:
        chars.append(0xFFFF)
    
    entries = []
    num_entries = len(chars) // 13
    for i in range(num_entries):
        seq = i + 1
        if i == num_entries - 1:
            seq |= 0x40  # Last LFN entry marker
        
        chunk = chars[i * 13:(i + 1) * 13]
        part1 = struct.pack('<5H', *chunk[0:5])
        part2 = struct.pack('<6H', *chunk[5:11])
        part3 = struct.pack('<2H', *chunk[11:13])
        
        # 32-byte LFN record
        # Byte 0: Sequence number
        # 1..10: Name chars 1-5 (UTF-16LE)
        # 11: Attributes (0x0F)
        # 12: Reserved (0x00)
        # 13: Checksum of short name
        # 14..25: Name chars 6-11 (UTF-16LE)
        # 26..27: First cluster (0x0000)
        # 28..31: Name chars 12-13 (UTF-16LE)
        rec = bytearray(32)
        rec[0] = seq
        rec[1:11] = part1
        rec[11] = 0x0F
        rec[12] = 0x00
        rec[13] = chk
        rec[14:26] = part2
        rec[26:28] = b'\x00\x00'
        rec[28:32] = part3
        entries.append(bytes(rec))
    
    # LFN entries are written in reverse order (highest sequence first)
    entries.reverse()
    return entries

def make_short_dir_entry(name_11, attr, first_cluster, file_size):
    entry = bytearray(32)
    entry[0:11] = name_11
    entry[11] = attr
    # Time/Date: 2026-10-18 12:00:00
    # FAT time: bits 15-11 = hours (12), 10-5 = minutes (0), 4-0 = sec/2 (0) -> 0x6000
    # FAT date: bits 15-9 = year - 1980 (46), 8-5 = month (10), 4-0 = day (18) -> (46 << 9) | (10 << 5) | 18 = 0x5D52
    struct.pack_into('<H', entry, 14, 0x6000)
    struct.pack_into('<H', entry, 16, 0x5D52)
    struct.pack_into('<H', entry, 18, 0x5D52)
    struct.pack_into('<H', entry, 22, 0x6000)
    struct.pack_into('<H', entry, 24, 0x5D52)
    struct.pack_into('<H', entry, 26, first_cluster & 0xFFFF)
    struct.pack_into('<I', entry, 28, file_size)
    return bytes(entry)

def build_fat16_image(files_dict):
    """
    Build a valid FAT16 filesystem image containing files_dict.
    files_dict is a mapping of path (e.g. 'EFI/BOOT/BOOTX64.EFI') to file bytes.
    Returns the binary image as bytes.
    """
    total_size = 16 * 1024 * 1024  # 16 MB
    sector_size = 512
    total_sectors = total_size // sector_size  # 32768 sectors
    sectors_per_cluster = 4  # 2048 bytes per cluster
    cluster_size = sectors_per_cluster * sector_size
    reserved_sectors = 4
    num_fats = 2
    root_entries = 512
    root_dir_sectors = (root_entries * 32) // sector_size  # 32 sectors
    
    # Calculate sectors per FAT: (total_clusters + 2) * 2 bytes / 512
    total_clusters = (total_sectors - reserved_sectors - root_dir_sectors) // sectors_per_cluster
    fat_size_sectors = ((total_clusters + 2) * 2 + sector_size - 1) // sector_size
    
    # Data region start sector
    data_start_sector = reserved_sectors + (num_fats * fat_size_sectors) + root_dir_sectors
    actual_clusters = (total_sectors - data_start_sector) // sectors_per_cluster
    
    # Allocate image buffer
    img = bytearray(total_size)
    
    # 1. Boot Sector (BPB)
    img[0:3] = b'\xeb\x3c\x90'  # JMP short
    img[3:11] = b'MSWIN4.1'
    struct.pack_into('<H', img, 11, sector_size)
    img[13] = sectors_per_cluster
    struct.pack_into('<H', img, 14, reserved_sectors)
    img[16] = num_fats
    struct.pack_into('<H', img, 17, root_entries)
    struct.pack_into('<H', img, 19, total_sectors if total_sectors < 65536 else 0)
    img[21] = 0xF8  # Media: Fixed disk
    struct.pack_into('<H', img, 22, fat_size_sectors)
    struct.pack_into('<H', img, 24, 32)  # Sectors per track
    struct.pack_into('<H', img, 26, 64)  # Heads
    struct.pack_into('<I', img, 28, 0)   # Hidden sectors
    struct.pack_into('<I', img, 32, total_sectors if total_sectors >= 65536 else 0)
    
    # Extended BIOS Parameter Block
    img[36] = 0x80  # Drive number
    img[37] = 0x00  # Reserved
    img[38] = 0x29  # Extended boot signature
    struct.pack_into('<I', img, 39, 0x19940520)  # Volume serial number
    img[43:54] = b'ASTRAOS    '  # Volume label (11 chars)
    img[54:62] = b'FAT16   '    # File system type
    img[510:512] = b'\x55\xaa'  # Boot signature
    
    # Initialize FAT table
    fat = [0] * (actual_clusters + 2)
    fat[0] = 0xFFF8
    fat[1] = 0xFFFF
    
    next_cluster = 2
    
    def alloc_clusters(data_bytes):
        nonlocal next_cluster
        if len(data_bytes) == 0:
            return 0
        num_c = (len(data_bytes) + cluster_size - 1) // cluster_size
        start_c = next_cluster
        for i in range(num_c):
            c = start_c + i
            # Write cluster data
            c_offset = (data_start_sector + (c - 2) * sectors_per_cluster) * sector_size
            chunk = data_bytes[i * cluster_size:(i + 1) * cluster_size]
            img[c_offset:c_offset + len(chunk)] = chunk
            if i == num_c - 1:
                fat[c] = 0xFFFF
            else:
                fat[c] = c + 1
        next_cluster += num_c
        return start_c
    
    # Structure files into directory hierarchy
    # We need:
    # Root:
    #   EFI/ (dir)
    #   limine.conf
    #   kernel.elf
    #   initramfs.bin
    # EFI/:
    #   BOOT/ (dir)
    # EFI/BOOT/:
    #   BOOTX64.EFI
    
    # First allocate clusters for regular files
    file_clusters = {}
    for path, data in files_dict.items():
        file_clusters[path] = (alloc_clusters(data), len(data))
    
    # Directory entries builder
    def build_dir_table(items, self_cluster=0, parent_cluster=0):
        records = bytearray()
        if self_cluster != 0:
            # Add '.' and '..'
            records += make_short_dir_entry(b'.          ', 0x10, self_cluster, 0)
            records += make_short_dir_entry(b'..         ', 0x10, parent_cluster, 0)
        
        for item in items:
            name, is_dir, target_cluster, size, short_name_11 = item
            # Add LFN entries
            lfns = create_lfn_entries(name, short_name_11)
            for lfn in lfns:
                records += lfn
            attr = 0x10 if is_dir else 0x20
            records += make_short_dir_entry(short_name_11, attr, target_cluster, size)
        return bytes(records)
    
    # 1. EFI/BOOT directory
    boot_items = [
        ('BOOTX64.EFI', False, file_clusters.get('EFI/BOOT/BOOTX64.EFI', (0,0))[0],
         file_clusters.get('EFI/BOOT/BOOTX64.EFI', (0,0))[1], b'BOOTX64 EFI'),
    ]
    if 'limine.conf' in file_clusters:
        c, sz = file_clusters['limine.conf']
        boot_items.append(('limine.conf', False, c, sz, b'LIMINE  CNF'))
    # Reserve cluster for EFI/BOOT
    boot_dir_cluster = next_cluster
    next_cluster += 1
    
    # 2. EFI directory
    efi_dir_cluster = next_cluster
    next_cluster += 1
    
    # Now build EFI/BOOT contents
    boot_dir_data = build_dir_table(boot_items, boot_dir_cluster, efi_dir_cluster)
    b_off = (data_start_sector + (boot_dir_cluster - 2) * sectors_per_cluster) * sector_size
    img[b_off:b_off + len(boot_dir_data)] = boot_dir_data
    fat[boot_dir_cluster] = 0xFFFF
    
    # 3. EFI contents
    efi_items = [
        ('BOOT', True, boot_dir_cluster, 0, b'BOOT       ')
    ]
    efi_dir_data = build_dir_table(efi_items, efi_dir_cluster, 0)
    e_off = (data_start_sector + (efi_dir_cluster - 2) * sectors_per_cluster) * sector_size
    img[e_off:e_off + len(efi_dir_data)] = efi_dir_data
    fat[efi_dir_cluster] = 0xFFFF
    
    # 4. Root directory entries
    root_items = [
        ('EFI', True, efi_dir_cluster, 0, b'EFI        '),
    ]
    if 'limine.conf' in file_clusters:
        c, sz = file_clusters['limine.conf']
        root_items.append(('limine.conf', False, c, sz, b'LIMINE  CNF'))
    if 'kernel.elf' in file_clusters:
        c, sz = file_clusters['kernel.elf']
        root_items.append(('kernel.elf', False, c, sz, b'KERNEL  ELF'))
    if 'initramfs.bin' in file_clusters:
        c, sz = file_clusters['initramfs.bin']
        root_items.append(('initramfs.bin', False, c, sz, b'INITRAMFBIN'))
    
    root_dir_data = build_dir_table(root_items, 0, 0)
    root_start_offset = (reserved_sectors + (num_fats * fat_size_sectors)) * sector_size
    img[root_start_offset:root_start_offset + len(root_dir_data)] = root_dir_data
    
    # Write FAT tables
    fat_bytes = bytearray()
    for entry in fat:
        fat_bytes += struct.pack('<H', entry)
    
    for f in range(num_fats):
        f_offset = (reserved_sectors + (f * fat_size_sectors)) * sector_size
        img[f_offset:f_offset + len(fat_bytes)] = fat_bytes
    
    return bytes(img)

# ----------------------------------------------------------------------
# ISO 9660 + El Torito Generation (Pure Python)
# ----------------------------------------------------------------------

def write_both_endian_16(val):
    return struct.pack('<H', val) + struct.pack('>H', val)

def write_both_endian_32(val):
    return struct.pack('<I', val) + struct.pack('>I', val)

def make_iso_dir_record(name_str, extent_sector, data_len, is_dir=False):
    name_bytes = name_str.encode('ascii')
    name_len = len(name_bytes)
    rec_len = 33 + name_len
    if rec_len % 2 != 0:
        rec_len += 1
    
    rec = bytearray(rec_len)
    rec[0] = rec_len
    rec[1] = 0  # Extended attribute record length
    rec[2:10] = write_both_endian_32(extent_sector)
    rec[10:18] = write_both_endian_32(data_len)
    
    # Date: 2026-10-18 12:00:00 (offset from 1900)
    rec[18] = 2026 - 1900
    rec[19] = 10
    rec[20] = 18
    rec[21] = 12
    rec[22] = 0
    rec[23] = 0
    rec[24] = 0  # GMT offset
    
    rec[25] = 0x02 if is_dir else 0x00
    rec[26] = 0
    rec[27] = 0
    rec[28:32] = write_both_endian_16(1)
    rec[32] = name_len
    rec[33:33 + name_len] = name_bytes
    return bytes(rec)

def build_iso(efi_fat_img, extra_files=None):
    """
    Builds a compliant ISO 9660 image containing the El Torito boot catalog
    with an EFI System Partition (efi_fat_img).
    Also includes extra_files (e.g. kernel.elf, limine.conf, etc.) in the ISO root.
    """
    SECTOR_SIZE = 2048
    
    # Sectors layout:
    # 0..15: System area (16 sectors)
    # 16: Primary Volume Descriptor (PVD)
    # 17: El Torito Boot Record Volume Descriptor
    # 18: Volume Descriptor Set Terminator
    # 19: Boot Catalog (1 sector)
    # 20: ISO Root Directory (1 sector)
    # 21: Path Table L (1 sector)
    # 22: Path Table M (1 sector)
    # 23+: File data (EFI image, kernel.elf, limine.conf, initramfs.bin)
    
    cur_sector = 23
    
    # Place EFI FAT image
    efi_img_sector = cur_sector
    efi_img_sectors = (len(efi_fat_img) + SECTOR_SIZE - 1) // SECTOR_SIZE
    cur_sector += efi_img_sectors
    
    file_placements = {}
    if extra_files:
        for fname, fdata in extra_files.items():
            f_sec = cur_sector
            f_sec_count = (len(fdata) + SECTOR_SIZE - 1) // SECTOR_SIZE
            file_placements[fname] = (f_sec, len(fdata), fdata)
            cur_sector += f_sec_count
    
    total_iso_sectors = cur_sector
    total_iso_bytes = total_iso_sectors * SECTOR_SIZE
    iso = bytearray(total_iso_bytes)
    
    # 1. Primary Volume Descriptor (Sector 16)
    pvd = bytearray(SECTOR_SIZE)
    pvd[0] = 1  # Primary Volume Descriptor
    pvd[1:6] = b'CD001'
    pvd[6] = 1
    pvd[8:40] = b'ASTRAOS'.ljust(32)
    pvd[40:72] = b'ASTRAOS'.ljust(32)
    pvd[80:88] = write_both_endian_32(total_iso_sectors)
    pvd[120:124] = write_both_endian_16(1)
    pvd[124:128] = write_both_endian_16(1)
    pvd[128:132] = write_both_endian_16(SECTOR_SIZE)
    pvd[132:140] = write_both_endian_32(10)  # Path table size (small)
    struct.pack_into('<I', pvd, 140, 21)    # Path table L sector
    struct.pack_into('>I', pvd, 148, 22)    # Path table M sector
    
    # Root dir entry in PVD (34 bytes)
    root_record_pvd = make_iso_dir_record('\x00', 20, SECTOR_SIZE, is_dir=True)
    pvd[156:156 + len(root_record_pvd)] = root_record_pvd
    
    # Standard strings
    pvd[190:318] = b''.ljust(128)
    pvd[318:446] = b''.ljust(128)
    pvd[446:574] = b''.ljust(128)
    pvd[574:702] = b''.ljust(128)
    pvd[813:830] = b'2026101812000000\x00'
    pvd[830:847] = b'2026101812000000\x00'
    pvd[881] = 1
    iso[16 * SECTOR_SIZE:17 * SECTOR_SIZE] = pvd
    
    # 2. El Torito Boot Record Volume Descriptor (Sector 17)
    br = bytearray(SECTOR_SIZE)
    br[0] = 0  # Boot Record
    br[1:6] = b'CD001'
    br[6] = 1
    br[7:39] = b'EL TORITO SPECIFICATION'.ljust(32, b'\x00')
    struct.pack_into('<I', br, 71, 19)  # Boot Catalog at sector 19
    iso[17 * SECTOR_SIZE:18 * SECTOR_SIZE] = br
    
    # 3. Volume Descriptor Set Terminator (Sector 18)
    term = bytearray(SECTOR_SIZE)
    term[0] = 255
    term[1:6] = b'CD001'
    term[6] = 1
    iso[18 * SECTOR_SIZE:19 * SECTOR_SIZE] = term
    
    # 4. Boot Catalog (Sector 19)
    cat = bytearray(SECTOR_SIZE)
    
    # Validation Entry (32 bytes)
    cat[0] = 0x01  # Header ID
    cat[1] = 0x00  # Platform: x86 BIOS / standard
    cat[4:28] = b'AstraOS'.ljust(24, b'\x00')
    cat[30] = 0x55
    cat[31] = 0xAA
    # Compute checksum of validation entry: sum of 16 uint16s == 0
    words = struct.unpack('<16H', cat[0:32])
    chk = (0x10000 - (sum(words) & 0xFFFF)) & 0xFFFF
    struct.pack_into('<H', cat, 28, chk)
    
    # Calculate 512-byte blocks
    efi_512_blocks = (len(efi_fat_img) + 511) // 512
    sector_count_field = min(efi_512_blocks, 0xFFFF)

    # Initial / Default Entry (32 bytes at offset 32):
    # For UEFI-only discs, pointing the Initial/Default Entry directly to the EFI image
    # ensures compatibility with firmware implementations that only inspect the initial entry.
    cat[32] = 0x88  # Bootable
    cat[33] = 0x00  # No emulation
    cat[36] = 0xEF  # System type: EFI
    struct.pack_into('<H', cat, 38, sector_count_field)
    struct.pack_into('<I', cat, 40, efi_img_sector)
    
    # Section Header: EFI (32 bytes at offset 64)
    cat[64] = 0x91  # Final section header
    cat[65] = 0xEF  # Platform ID: EFI
    struct.pack_into('<H', cat, 66, 1)  # 1 section entry
    cat[68:96] = b'EFI'.ljust(28, b'\x00')
    
    # Section Entry: EFI Boot Image (32 bytes at offset 96)
    cat[96] = 0x88  # Bootable
    cat[97] = 0x00  # No emulation
    cat[100] = 0xEF # System type: EFI
    struct.pack_into('<H', cat, 102, sector_count_field)
    struct.pack_into('<I', cat, 104, efi_img_sector)
    
    iso[19 * SECTOR_SIZE:20 * SECTOR_SIZE] = cat
    
    # 5. Root Directory (Sector 20)
    root_dir = bytearray(SECTOR_SIZE)
    # '.' entry
    r1 = make_iso_dir_record('\x00', 20, SECTOR_SIZE, is_dir=True)
    # '..' entry
    r2 = make_iso_dir_record('\x01', 20, SECTOR_SIZE, is_dir=True)
    # EFI.IMG;1 entry
    r3 = make_iso_dir_record('EFI.IMG;1', efi_img_sector, len(efi_fat_img), is_dir=False)
    
    offset = 0
    for r in [r1, r2, r3]:
        root_dir[offset:offset + len(r)] = r
        offset += len(r)
    
    if extra_files:
        for fname, (f_sec, f_len, _) in file_placements.items():
            iso_fname = fname.upper() + ';1'
            rec = make_iso_dir_record(iso_fname, f_sec, f_len, is_dir=False)
            if offset + len(rec) > SECTOR_SIZE:
                break
            root_dir[offset:offset + len(rec)] = rec
            offset += len(rec)
            
    iso[20 * SECTOR_SIZE:21 * SECTOR_SIZE] = root_dir
    
    # 6. Path tables (Sectors 21 & 22)
    # Entry for root: len_di(1), ext_attr(0), extent(20), parent_dir_num(1), name("\x00") -> 10 bytes
    pt_l = struct.pack('<BBII', 1, 0, 20, 1) + b'\x00\x00'
    iso[21 * SECTOR_SIZE:21 * SECTOR_SIZE + len(pt_l)] = pt_l
    pt_m = struct.pack('>BBII', 1, 0, 20, 1) + b'\x00\x00'
    iso[22 * SECTOR_SIZE:22 * SECTOR_SIZE + len(pt_m)] = pt_m
    
    # 7. Write EFI Image and extra files
    iso[efi_img_sector * SECTOR_SIZE:efi_img_sector * SECTOR_SIZE + len(efi_fat_img)] = efi_fat_img
    
    if extra_files:
        for fname, (f_sec, f_len, fdata) in file_placements.items():
            iso[f_sec * SECTOR_SIZE:f_sec * SECTOR_SIZE + f_len] = fdata
            
    return bytes(iso)

def generate_iso():
    # Make sure all boot artifacts are present
    kernel_elf = os.path.join(BUILD_DIR, 'kernel.elf')
    initramfs_bin = os.path.join(BUILD_DIR, 'initramfs.bin')
    limine_conf = os.path.join(BASE_DIR, 'limine.conf')
    bootx64 = os.path.join(BASE_DIR, 'third_party', 'limine', 'BOOTX64.EFI')
    
    for path, desc in [
        (kernel_elf, 'Kernel ELF'),
        (initramfs_bin, 'Initramfs binary'),
        (limine_conf, 'Limine configuration'),
        (bootx64, 'Limine UEFI bootloader (BOOTX64.EFI)')
    ]:
        if not os.path.exists(path):
            raise FileNotFoundError(f"Missing {desc} at: {path}")
            
    with open(bootx64, 'rb') as f:
        bootx64_data = f.read()
    with open(limine_conf, 'rb') as f:
        limine_conf_data = f.read()
    with open(kernel_elf, 'rb') as f:
        kernel_elf_data = f.read()
    with open(initramfs_bin, 'rb') as f:
        initramfs_data = f.read()
        
    print("[ISO] Building FAT16 EFI System Partition image...")
    fat_files = {
        'EFI/BOOT/BOOTX64.EFI': bootx64_data,
        'limine.conf': limine_conf_data,
        'kernel.elf': kernel_elf_data,
        'initramfs.bin': initramfs_data,
    }
    efi_fat_img = build_fat16_image(fat_files)
    print(f"[ISO] Generated EFI FAT16 image: {len(efi_fat_img)} bytes ({len(efi_fat_img) // 1024 // 1024} MB)")
    
    print("[ISO] Building El Torito ISO 9660 image...")
    iso_extra = {
        'limine.conf': limine_conf_data,
        'kernel.elf': kernel_elf_data,
        'initramfs.bin': initramfs_data,
    }
    iso_bytes = build_iso(efi_fat_img, iso_extra)
    
    with open(ISO_OUT, 'wb') as f:
        f.write(iso_bytes)
        
    print(f"[ISO] Successfully created bootable ISO: {ISO_OUT} ({len(iso_bytes)} bytes, {len(iso_bytes) / 1024 / 1024:.2f} MB)")
    return ISO_OUT

if __name__ == '__main__':
    generate_iso()

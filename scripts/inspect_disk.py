import os
import struct

build_dir = r'C:\Users\Akash\Documents\MyKernel\build'
data_disk = os.path.join(build_dir, 'data_disk.img')
if os.path.exists(data_disk):
    with open(data_disk, 'rb') as f:
        f.seek(1 * 512)
        sbA = f.read(512)
        # struct astrafs_superblock:
        # u32 magic, u32 version, u64 generation, u32 sector_size, u32 total_sectors, u32 data_start_lba
        # u32 inode_table_lba, u32 inode_sectors, u32 bitmap_lba, u32 bitmap_sectors, u32 file_count, u32 used_sectors
        magic, ver, gen, ss, tot_s, d_lba, ino_lba, ino_s, bmp_lba, bmp_s, fc, used_s = struct.unpack_from('<IIQIIIIIIIII', sbA, 0)
        print(f"magic=0x{magic:08x}, ver={ver}, gen={gen}, fc={fc}, used_s={used_s}, ino_lba={ino_lba}")
        f.seek(ino_lba * 512)
        for i in range(64):
            ino_data = f.read(128)
            name_raw = ino_data[:48]
            name = name_raw.split(b'\x00')[0].decode('latin1', errors='replace')
            in_use, fsize, start_lba, sc, flags, csum = struct.unpack_from('<IIIIII', ino_data, 48)
            if in_use:
                print(f"  Inode {i:02d}: name='{name}', size={fsize}, start_lba={start_lba}, sectors={sc}")

#include <kernel/block.h>
#include <kernel/kprintf.h>
#include <kernel/mem.h>

static block_device_t g_block_devices[BLOCK_MAX_DEVICES];
static int g_block_device_count = 0;

void block_init(void) {
    g_block_device_count = 0;
    kmemset(g_block_devices, 0, sizeof(g_block_devices));
}

int block_register_device(block_device_t *dev) {
    if (!dev || !dev->ops || !dev->ops->read_sectors) {
        return BLOCK_ERR_INVALID_PARAM;
    }
    if (dev->sector_size == 0 || dev->total_sectors == 0) {
        return BLOCK_ERR_INVALID_PARAM;
    }
    if (g_block_device_count >= BLOCK_MAX_DEVICES) {
        return BLOCK_ERR_OUT_OF_BOUNDS;
    }

    int idx = g_block_device_count;
    block_device_t *slot = &g_block_devices[idx];
    kmemcpy(slot, dev, sizeof(block_device_t));

    slot->id = (u32)idx;
    slot->capacity_bytes = slot->total_sectors * (u64)slot->sector_size;
    slot->status = BLOCK_STATUS_READY;

    g_block_device_count++;

    u64 cap_mib = slot->capacity_bytes / (1024 * 1024);
    kprintf("[BLOCK] Registered device '%s' (id=%u): %llu MiB (%llu sectors, %u B/sector)%s%s\n",
            slot->name, slot->id,
            (unsigned long long)cap_mib,
            (unsigned long long)slot->total_sectors,
            slot->sector_size,
            slot->read_only ? " [READ-ONLY]" : "",
            slot->is_boot_device ? " [BOOT-MEDIA]" : "");

    return idx;
}

int block_get_device_count(void) {
    return g_block_device_count;
}

block_device_t *block_get_device(int idx) {
    if (idx < 0 || idx >= g_block_device_count) {
        return NULL;
    }
    return &g_block_devices[idx];
}

block_device_t *block_find_by_name(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < g_block_device_count; i++) {
        const char *a = g_block_devices[i].name;
        const char *b = name;
        bool match = true;
        while (*a && *b) {
            if (*a != *b) { match = false; break; }
            a++; b++;
        }
        if (match && *a == '\0' && *b == '\0') {
            return &g_block_devices[i];
        }
    }
    return NULL;
}

int block_read(block_device_t *dev, u64 lba, u32 count, void *buf) {
    if (!dev) return BLOCK_ERR_INVALID_PARAM;
    if (dev->status != BLOCK_STATUS_READY) return BLOCK_ERR_NO_DEVICE;
    if (!buf) return BLOCK_ERR_INVALID_PARAM;
    if (count == 0) return 0;

    /* Integer overflow check */
    if (lba + (u64)count < lba) {
        return BLOCK_ERR_OVERFLOW;
    }

    /* Range check */
    if (lba + (u64)count > dev->total_sectors) {
        return BLOCK_ERR_OUT_OF_BOUNDS;
    }

    if (!dev->ops || !dev->ops->read_sectors) {
        return BLOCK_ERR_IO;
    }

    return dev->ops->read_sectors(dev, lba, count, buf);
}

int block_write(block_device_t *dev, u64 lba, u32 count, const void *buf) {
    if (!dev) return BLOCK_ERR_INVALID_PARAM;
    if (dev->status != BLOCK_STATUS_READY) return BLOCK_ERR_NO_DEVICE;
    if (!buf) return BLOCK_ERR_INVALID_PARAM;
    if (count == 0) return 0;

    /* Protection guards */
    if (dev->read_only || dev->is_boot_device) {
        return BLOCK_ERR_READ_ONLY;
    }

    /* Integer overflow check */
    if (lba + (u64)count < lba) {
        return BLOCK_ERR_OVERFLOW;
    }

    /* Range check */
    if (lba + (u64)count > dev->total_sectors) {
        return BLOCK_ERR_OUT_OF_BOUNDS;
    }

    if (!dev->ops || !dev->ops->write_sectors) {
        return BLOCK_ERR_IO;
    }

    return dev->ops->write_sectors(dev, lba, count, buf);
}

int block_flush(block_device_t *dev) {
    if (!dev) return BLOCK_ERR_INVALID_PARAM;
    if (dev->status != BLOCK_STATUS_READY) return BLOCK_ERR_NO_DEVICE;
    if (!dev->ops) return BLOCK_ERR_IO;

    if (dev->ops->flush) {
        return dev->ops->flush(dev);
    }
    return BLOCK_OK;
}

const char *block_strerror(int err) {
    switch (err) {
        case BLOCK_OK:                  return "Success";
        case BLOCK_ERR_INVALID_PARAM:   return "Invalid parameter";
        case BLOCK_ERR_OUT_OF_BOUNDS:   return "Out of bounds";
        case BLOCK_ERR_IO:              return "I/O error";
        case BLOCK_ERR_TIMEOUT:         return "Operation timed out";
        case BLOCK_ERR_NO_DEVICE:       return "Device not found / offline";
        case BLOCK_ERR_READ_ONLY:       return "Device is read-only / boot protected";
        case BLOCK_ERR_OVERFLOW:        return "Integer overflow in address calculation";
        default:                        return "Unknown block error";
    }
}

bool block_self_test(void) {
    kprintf("=======================================================\n");
    kprintf("     AstraOS Phase AD: Block Device Subsystem Tests    \n");
    kprintf("=======================================================\n");

    /* 1. Parameter Validation & Boundary Guard Tests */
    kprintf("[BLOCK TEST] 1. Testing Parameter Validation & Guard Boundaries...\n");
    if (block_read(NULL, 0, 1, (void *)0x1000) != BLOCK_ERR_INVALID_PARAM) return false;
    if (block_write(NULL, 0, 1, (const void *)0x1000) != BLOCK_ERR_INVALID_PARAM) return false;
    if (block_flush(NULL) != BLOCK_ERR_INVALID_PARAM) return false;

    u8 dummy[BLOCK_SECTOR_SIZE];
    block_device_t *boot_dev = NULL;
    block_device_t *data_dev = NULL;

    for (int i = 0; i < g_block_device_count; i++) {
        if (g_block_devices[i].is_boot_device) {
            boot_dev = &g_block_devices[i];
        } else if (!g_block_devices[i].read_only) {
            data_dev = &g_block_devices[i];
        }
    }

    if (boot_dev) {
        /* Verify boot device is strictly protected against writes */
        kprintf("[BLOCK TEST] 2. Verifying Boot Device Write Protection ('%s')...\n", boot_dev->name);
        int w_res = block_write(boot_dev, 0, 1, dummy);
        if (w_res != BLOCK_ERR_READ_ONLY) {
            kprintf("[FATAL] Boot device write was not rejected! Error: %d\n", w_res);
            return false;
        }
        kprintf("  PASS: Boot device '%s' writes fail-closed with BLOCK_ERR_READ_ONLY.\n", boot_dev->name);

        /* Boundary and overflow checks on boot device */
        if (block_read(boot_dev, boot_dev->total_sectors, 1, dummy) != BLOCK_ERR_OUT_OF_BOUNDS) return false;
        if (block_read(boot_dev, 0xFFFFFFFFFFFFFFFFULL, 1, dummy) != BLOCK_ERR_OVERFLOW) return false;
        if (block_read(boot_dev, 0, 0, dummy) != 0) return false;
        if (block_read(boot_dev, 0, 1, NULL) != BLOCK_ERR_INVALID_PARAM) return false;
    }

    if (data_dev) {
        kprintf("[BLOCK TEST] 3. Testing Dedicated Data Disk ('%s', %llu MiB)...\n",
                data_dev->name, (unsigned long long)(data_dev->capacity_bytes / (1024 * 1024)));

        /* Boundary checks on data device */
        if (block_write(data_dev, data_dev->total_sectors, 1, dummy) != BLOCK_ERR_OUT_OF_BOUNDS) return false;
        if (block_write(data_dev, 0xFFFFFFFFFFFFFFFFULL, 1, dummy) != BLOCK_ERR_OVERFLOW) return false;
        if (block_write(data_dev, 0, 0, dummy) != 0) return false;
        if (block_write(data_dev, 0, 1, NULL) != BLOCK_ERR_INVALID_PARAM) return false;

        /* Designate test sector: LBA 2048 (offset 1 MiB) */
        u64 test_lba = 2048;
        if (test_lba < data_dev->total_sectors) {
            kprintf("[BLOCK TEST] 4. Testing Sector Write & Read-Back at LBA %llu...\n",
                    (unsigned long long)test_lba);

            static u8 write_pat[BLOCK_SECTOR_SIZE];
            static u8 read_buf[BLOCK_SECTOR_SIZE];

            /* Prepare deterministic pattern */
            *(u64 *)&write_pat[0] = 0x4153545241424C4BULL; /* "ASTRABLK" */
            *(u64 *)&write_pat[8] = test_lba;
            for (int i = 16; i < BLOCK_SECTOR_SIZE; i++) {
                write_pat[i] = (u8)((i * 37 + 13) & 0xFF);
            }

            int w_res = block_write(data_dev, test_lba, 1, write_pat);
            if (w_res != BLOCK_OK) {
                kprintf("[FATAL] Failed to write test sector %llu! err=%d (%s)\n",
                        (unsigned long long)test_lba, w_res, block_strerror(w_res));
                return false;
            }

            int f_res = block_flush(data_dev);
            if (f_res != BLOCK_OK) {
                kprintf("[FATAL] Flush failed! err=%d\n", f_res);
                return false;
            }

            kmemset(read_buf, 0, sizeof(read_buf));
            int r_res = block_read(data_dev, test_lba, 1, read_buf);
            if (r_res != BLOCK_OK) {
                kprintf("[FATAL] Failed to read test sector %llu! err=%d (%s)\n",
                        (unsigned long long)test_lba, r_res, block_strerror(r_res));
                return false;
            }

            if (kmemcmp(write_pat, read_buf, BLOCK_SECTOR_SIZE) != 0) {
                kprintf("[FATAL] Read-back byte mismatch at LBA %llu!\n", (unsigned long long)test_lba);
                return false;
            }
            kprintf("  PASS: Single-sector write, flush, and read-back verified byte-for-byte.\n");

            /* Multi-sector test: 2 sectors at LBA 2050 */
            u64 multi_lba = 2050;
            if (multi_lba + 2 <= data_dev->total_sectors) {
                kprintf("[BLOCK TEST] 5. Testing Multi-Sector I/O (2 sectors at LBA %llu)...\n",
                        (unsigned long long)multi_lba);

                static u8 multi_write[BLOCK_SECTOR_SIZE * 2];
                static u8 multi_read[BLOCK_SECTOR_SIZE * 2];

                for (int i = 0; i < BLOCK_SECTOR_SIZE * 2; i++) {
                    multi_write[i] = (u8)((i ^ 0xAA) & 0xFF);
                }

                if (block_write(data_dev, multi_lba, 2, multi_write) != BLOCK_OK) return false;
                if (block_flush(data_dev) != BLOCK_OK) return false;
                kmemset(multi_read, 0, sizeof(multi_read));
                if (block_read(data_dev, multi_lba, 2, multi_read) != BLOCK_OK) return false;

                if (kmemcmp(multi_write, multi_read, BLOCK_SECTOR_SIZE * 2) != 0) {
                    kprintf("[FATAL] Multi-sector read-back mismatch!\n");
                    return false;
                }
                kprintf("  PASS: Multi-sector read and write verified byte-for-byte.\n");
            }
        }
    } else {
        kprintf("[BLOCK] Note: No secondary writable data disk attached.\n");
    }

    kprintf("[BLOCK TEST] All block device subsystem tests passed successfully!\n");
    kprintf("ASTRA_BLOCK_DEVICE_OK\n\n");
    return true;
}


#include <kernel/ata.h>
#include <kernel/arch/x86_64/cpu.h>
#include <kernel/kprintf.h>
#include <kernel/mem.h>
#include <kernel/spinlock.h>

spinlock_t g_ata_lock = SPINLOCK_INIT;

static ata_drive_t g_ata_drives[4];
static int g_ata_drive_count = 0;

static void ata_delay_400ns(u16 ctrl_base) {
    /* Reading Alternate Status 4 times guarantees >= 400ns delay */
    inb(ctrl_base);
    inb(ctrl_base);
    inb(ctrl_base);
    inb(ctrl_base);
}

static int ata_wait_bsy(u16 ctrl_base) {
    for (int i = 0; i < ATA_TIMEOUT_CYCLES; i++) {
        u8 status = inb(ctrl_base);
        if (!(status & ATA_STATUS_BSY)) {
            return 0;
        }
        cpu_pause();
    }
    return BLOCK_ERR_TIMEOUT;
}

static int ata_wait_drq(u16 ctrl_base) {
    for (int i = 0; i < ATA_TIMEOUT_CYCLES; i++) {
        u8 status = inb(ctrl_base);
        if (status & (ATA_STATUS_ERR | ATA_STATUS_DF)) {
            return BLOCK_ERR_IO;
        }
        if (!(status & ATA_STATUS_BSY) && (status & ATA_STATUS_DRQ)) {
            return 0;
        }
        cpu_pause();
    }
    return BLOCK_ERR_TIMEOUT;
}

static void ata_select_drive(u16 io_base, u16 ctrl_base, u8 drive, u8 lba_hi4) {
    u8 val = (drive == 0 ? 0xE0 : 0xF0) | (lba_hi4 & 0x0F);
    outb(io_base + ATA_REG_DRIVE_HEAD, val);
    ata_delay_400ns(ctrl_base);
}

static int ata_read_sectors_internal(ata_drive_t *d, u64 lba, u32 count, void *buf) {

    u8 *dst = (u8 *)buf;
    u64 curr_lba = lba;
    u32 remaining = count;

    while (remaining > 0) {
        u32 chunk = remaining > 256 ? 256 : remaining;
        u8 sec_cnt = (chunk == 256) ? 0 : (u8)chunk;

        if (ata_wait_bsy(d->ctrl_base) != 0) {
            return BLOCK_ERR_TIMEOUT;
        }

        ata_select_drive(d->io_base, d->ctrl_base, d->drive_idx, (u8)((curr_lba >> 24) & 0x0F));

        outb(d->io_base + ATA_REG_SECCOUNT, sec_cnt);
        outb(d->io_base + ATA_REG_LBA_LO, (u8)(curr_lba & 0xFF));
        outb(d->io_base + ATA_REG_LBA_MID, (u8)((curr_lba >> 8) & 0xFF));
        outb(d->io_base + ATA_REG_LBA_HI, (u8)((curr_lba >> 16) & 0xFF));

        outb(d->io_base + ATA_REG_COMMAND, ATA_CMD_READ_PIO);

        for (u32 s = 0; s < chunk; s++) {
            int drq_res = ata_wait_drq(d->ctrl_base);
            if (drq_res != 0) {
                return drq_res;
            }

            u16 *word_ptr = (u16 *)(dst + (s * BLOCK_SECTOR_SIZE));
            insw(d->io_base + ATA_REG_DATA, word_ptr, 256);
        }

        dst += (chunk * BLOCK_SECTOR_SIZE);
        curr_lba += chunk;
        remaining -= chunk;
    }

    return BLOCK_OK;
}

static int ata_op_read_sectors(block_device_t *dev, u64 lba, u32 count, void *buf) {
    if (!dev || !dev->priv || !buf) return BLOCK_ERR_INVALID_PARAM;
    u64 flags = spinlock_acquire_irqsave(&g_ata_lock);
    int res = ata_read_sectors_internal((ata_drive_t *)dev->priv, lba, count, buf);
    spinlock_release_irqrestore(&g_ata_lock, flags);
    return res;
}

static int ata_write_sectors_internal(ata_drive_t *d, u64 lba, u32 count, const void *buf) {

    const u8 *src = (const u8 *)buf;
    u64 curr_lba = lba;
    u32 remaining = count;

    while (remaining > 0) {
        u32 chunk = remaining > 256 ? 256 : remaining;
        u8 sec_cnt = (chunk == 256) ? 0 : (u8)chunk;

        if (ata_wait_bsy(d->ctrl_base) != 0) {
            return BLOCK_ERR_TIMEOUT;
        }

        ata_select_drive(d->io_base, d->ctrl_base, d->drive_idx, (u8)((curr_lba >> 24) & 0x0F));

        outb(d->io_base + ATA_REG_SECCOUNT, sec_cnt);
        outb(d->io_base + ATA_REG_LBA_LO, (u8)(curr_lba & 0xFF));
        outb(d->io_base + ATA_REG_LBA_MID, (u8)((curr_lba >> 8) & 0xFF));
        outb(d->io_base + ATA_REG_LBA_HI, (u8)((curr_lba >> 16) & 0xFF));

        outb(d->io_base + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);

        for (u32 s = 0; s < chunk; s++) {
            int drq_res = ata_wait_drq(d->ctrl_base);
            if (drq_res != 0) {
                return drq_res;
            }

            const u16 *word_ptr = (const u16 *)(src + (s * BLOCK_SECTOR_SIZE));
            outsw(d->io_base + ATA_REG_DATA, word_ptr, 256);
        }

        src += (chunk * BLOCK_SECTOR_SIZE);
        curr_lba += chunk;
        remaining -= chunk;
    }

    return BLOCK_OK;
}

static int ata_op_write_sectors(block_device_t *dev, u64 lba, u32 count, const void *buf) {
    if (!dev || !dev->priv || !buf) return BLOCK_ERR_INVALID_PARAM;
    u64 flags = spinlock_acquire_irqsave(&g_ata_lock);
    int res = ata_write_sectors_internal((ata_drive_t *)dev->priv, lba, count, buf);
    spinlock_release_irqrestore(&g_ata_lock, flags);
    return res;
}

static int ata_flush_internal(block_device_t *dev, ata_drive_t *d) {

    if (!d->present || dev->status != BLOCK_STATUS_READY) {
        return BLOCK_ERR_NO_DEVICE;
    }

    if (ata_wait_bsy(d->ctrl_base) != 0) {
        return BLOCK_ERR_TIMEOUT;
    }

    ata_select_drive(d->io_base, d->ctrl_base, d->drive_idx, 0);

    /*
     * Select flush command according to device capabilities and capacity:
     * - FLUSH CACHE EXT (0xEA) for 48-bit LBA drives exceeding 128 GiB if supported.
     * - FLUSH CACHE (0xE7) for standard drives.
     * - Return BLOCK_ERR_UNSUPPORTED if the device supports neither flush command.
     */
    u8 cmd = 0;
    if (d->lba48_supported && d->total_sectors > 0x0FFFFFFF && d->flush_ext_supported) {
        cmd = ATA_CMD_FLUSH_CACHE_EXT;
    } else if (d->flush_cache_supported) {
        cmd = ATA_CMD_FLUSH_CACHE;
    } else if (d->lba48_supported && d->total_sectors > 0x0FFFFFFF) {
        cmd = ATA_CMD_FLUSH_CACHE_EXT;
    } else {
        return BLOCK_ERR_UNSUPPORTED;
    }

    outb(d->io_base + ATA_REG_COMMAND, cmd);
    ata_delay_400ns(d->ctrl_base);

    if (ata_wait_bsy(d->ctrl_base) != 0) {
        return BLOCK_ERR_TIMEOUT;
    }

    u8 status = inb(d->ctrl_base);

    /*
     * Flush is a NON-DATA command. The ATA controller must NEVER assert DRQ.
     * If DRQ is asserted or Drive Fault (DF) is reported, treat as hardware error.
     */
    if (status & ATA_STATUS_DRQ) {
        (void)inb(d->io_base + ATA_REG_ERROR);
        return BLOCK_ERR_IO;
    }

    if (status & (ATA_STATUS_ERR | ATA_STATUS_DF)) {
        u8 err_reg = inb(d->io_base + ATA_REG_ERROR);
        /* If 0xEA was aborted by the controller, retry with standard 0xE7 if supported */
        if (cmd == ATA_CMD_FLUSH_CACHE_EXT && (err_reg & ATA_ERR_ABRT) && d->flush_cache_supported) {
            ata_select_drive(d->io_base, d->ctrl_base, d->drive_idx, 0);
            outb(d->io_base + ATA_REG_COMMAND, ATA_CMD_FLUSH_CACHE);
            ata_delay_400ns(d->ctrl_base);
            if (ata_wait_bsy(d->ctrl_base) != 0) {
                return BLOCK_ERR_TIMEOUT;
            }
            status = inb(d->ctrl_base);
            if ((status & ATA_STATUS_DRQ) || (status & (ATA_STATUS_ERR | ATA_STATUS_DF))) {
                (void)inb(d->io_base + ATA_REG_ERROR);
                return BLOCK_ERR_IO;
            }
            return BLOCK_OK;
        }
        return BLOCK_ERR_IO;
    }

    return BLOCK_OK;
}

static int ata_op_flush(block_device_t *dev) {
    if (!dev || !dev->priv) return BLOCK_ERR_INVALID_PARAM;
    u64 flags = spinlock_acquire_irqsave(&g_ata_lock);
    int res = ata_flush_internal(dev, (ata_drive_t *)dev->priv);
    spinlock_release_irqrestore(&g_ata_lock, flags);
    return res;
}

static const block_ops_t s_ata_ops = {
    .read_sectors = ata_op_read_sectors,
    .write_sectors = ata_op_write_sectors,
    .flush = ata_op_flush
};

static void clean_ata_string(char *dst, const u16 *words, int word_count, size_t max_dst) {
    int dst_idx = 0;
    for (int i = 0; i < word_count && dst_idx + 1 < (int)max_dst; i++) {
        u16 w = words[i];
        char c1 = (char)((w >> 8) & 0xFF);
        char c2 = (char)(w & 0xFF);
        if (dst_idx + 1 < (int)max_dst) dst[dst_idx++] = c1;
        if (dst_idx + 1 < (int)max_dst) dst[dst_idx++] = c2;
    }
    dst[dst_idx] = '\0';

    /* Trim trailing spaces */
    for (int i = dst_idx - 1; i >= 0 && dst[i] == ' '; i--) {
        dst[i] = '\0';
    }
}

static bool ata_identify(ata_drive_t *d) {
    /* Disable interrupts on channel */
    outb(d->ctrl_base, ATA_CTRL_NIEN);

    /* Check floating bus */
    u8 status = inb(d->io_base + ATA_REG_STATUS);
    if (status == 0xFF) {
        return false;
    }

    /* Select drive */
    outb(d->io_base + ATA_REG_DRIVE_HEAD, d->drive_idx == 0 ? 0xA0 : 0xB0);
    ata_delay_400ns(d->ctrl_base);

    outb(d->io_base + ATA_REG_SECCOUNT, 0);
    outb(d->io_base + ATA_REG_LBA_LO, 0);
    outb(d->io_base + ATA_REG_LBA_MID, 0);
    outb(d->io_base + ATA_REG_LBA_HI, 0);

    outb(d->io_base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

    status = inb(d->io_base + ATA_REG_STATUS);
    if (status == 0) {
        return false;
    }

    if (ata_wait_bsy(d->ctrl_base) != 0) {
        return false;
    }

    /* Check for ATAPI signatures */
    u8 mid = inb(d->io_base + ATA_REG_LBA_MID);
    u8 hi = inb(d->io_base + ATA_REG_LBA_HI);
    if ((mid == 0x14 && hi == 0xEB) || (mid == 0x69 && hi == 0x96)) {
        d->is_atapi = true;
        return false; /* ATAPI CD-ROM/DVD skipped for Phase AD ATA disk driver */
    }

    /* Wait for DRQ */
    int drq_res = ata_wait_drq(d->ctrl_base);
    if (drq_res != 0) {
        return false;
    }

    u16 ident[256];
    for (int i = 0; i < 256; i++) {
        ident[i] = inw(d->io_base + ATA_REG_DATA);
    }

    clean_ata_string(d->model, &ident[27], 20, sizeof(d->model));
    clean_ata_string(d->serial, &ident[10], 10, sizeof(d->serial));

    /* Extract capacity and feature sets */
    u32 lba28 = (u32)ident[60] | ((u32)ident[61] << 16);
    u64 lba48 = 0;
    d->flush_cache_supported = true; /* Per ATA-4+ specification, standard ATA drives support FLUSH CACHE (0xE7) */
    d->lba48_supported = (ident[83] & (1 << 10)) != 0;
    d->flush_ext_supported = (ident[83] & (1 << 13)) != 0;

    if (d->lba48_supported) {
        lba48 = (u64)ident[100] |
                ((u64)ident[101] << 16) |
                ((u64)ident[102] << 32) |
                ((u64)ident[103] << 48);
    }

    d->total_sectors = (lba48 > 0) ? lba48 : (u64)lba28;
    if (d->total_sectors == 0) {
        return false;
    }

    return true;
}

void ata_init(void) {
    kprintf("[ATA] Initializing ATA/IDE PIO Storage Driver...\n");
    g_ata_drive_count = 0;
    kmemset(g_ata_drives, 0, sizeof(g_ata_drives));

    struct {
        u16 io;
        u16 ctrl;
    } channels[2] = {
        { ATA_PRIMARY_IO, ATA_PRIMARY_CTRL },
        { ATA_SECONDARY_IO, ATA_SECONDARY_CTRL }
    };

    for (int c = 0; c < 2; c++) {
        u16 io_base = channels[c].io;
        u16 ctrl_base = channels[c].ctrl;

        /* Disable interrupts on channel */
        outb(ctrl_base, ATA_CTRL_NIEN);

        for (int d = 0; d < 2; d++) {
            int slot = c * 2 + d;
            ata_drive_t *drv = &g_ata_drives[slot];

            drv->channel_idx = (u8)c;
            drv->drive_idx = (u8)d;
            drv->io_base = io_base;
            drv->ctrl_base = ctrl_base;

            if (ata_identify(drv)) {
                drv->present = true;
                g_ata_drive_count++;

                /* Format name: hd0, hd1, hd2, hd3 */
                drv->block_dev.name[0] = 'h';
                drv->block_dev.name[1] = 'd';
                drv->block_dev.name[2] = '0' + (char)slot;
                drv->block_dev.name[3] = '\0';

                drv->block_dev.sector_size = BLOCK_SECTOR_SIZE;
                drv->block_dev.total_sectors = drv->total_sectors;
                drv->block_dev.capacity_bytes = drv->total_sectors * BLOCK_SECTOR_SIZE;
                drv->block_dev.ops = &s_ata_ops;
                drv->block_dev.priv = drv;

                /* Primary Master (c=0, d=0) in QEMU is the FAT EFI boot partition */
                if (c == 0 && d == 0) {
                    drv->block_dev.is_boot_device = true;
                    drv->block_dev.read_only = true;
                } else {
                    drv->block_dev.is_boot_device = false;
                    drv->block_dev.read_only = false;
                }

                u64 cap_mib = drv->block_dev.capacity_bytes / (1024 * 1024);
                kprintf("[ATA] Channel %d Drive %d: Model '%s', Serial '%s', Capacity %llu MiB (%llu sectors)%s\n",
                        c, d, drv->model, drv->serial,
                        (unsigned long long)cap_mib,
                        (unsigned long long)drv->total_sectors,
                        drv->block_dev.is_boot_device ? " [BOOT DRIVE - PROTECTED]" : "");

                block_register_device(&drv->block_dev);
            }
        }
    }

    kprintf("[ATA] Discovered %d ATA block device(s).\n", g_ata_drive_count);
}

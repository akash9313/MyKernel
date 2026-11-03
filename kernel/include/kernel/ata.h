#ifndef ASTRA_ATA_H
#define ASTRA_ATA_H

#include <kernel/types.h>
#include <kernel/block.h>
#include <kernel/spinlock.h>

extern spinlock_t g_ata_lock;

/* Standard ATA I/O Port Bases */
#define ATA_PRIMARY_IO        0x1F0
#define ATA_PRIMARY_CTRL      0x3F6
#define ATA_SECONDARY_IO      0x170
#define ATA_SECONDARY_CTRL    0x376

/* Register Offsets (from I/O Base) */
#define ATA_REG_DATA          0
#define ATA_REG_ERROR         1   /* Read */
#define ATA_REG_FEATURES      1   /* Write */
#define ATA_REG_SECCOUNT      2
#define ATA_REG_LBA_LO        3
#define ATA_REG_LBA_MID       4
#define ATA_REG_LBA_HI        5
#define ATA_REG_DRIVE_HEAD    6
#define ATA_REG_STATUS        7   /* Read */
#define ATA_REG_COMMAND       7   /* Write */

/* Device Control Register Bits (from Control Base) */
#define ATA_CTRL_NIEN         0x02 /* Interrupt Disable */
#define ATA_CTRL_SRST         0x04 /* Software Reset */

/* Status Register Flags */
#define ATA_STATUS_ERR        0x01 /* Error */
#define ATA_STATUS_INDEX      0x02 /* Index */
#define ATA_STATUS_CORR       0x04 /* Corrected data */
#define ATA_STATUS_DRQ        0x08 /* Data Request */
#define ATA_STATUS_SRV        0x10 /* Service request */
#define ATA_STATUS_DF         0x20 /* Drive Fault */
#define ATA_STATUS_RDY        0x40 /* Drive Ready */
#define ATA_STATUS_BSY        0x80 /* Busy */

/* Error Register Flags */
#define ATA_ERR_AMNF          0x01 /* Address Mark Not Found */
#define ATA_ERR_TK0NF         0x02 /* Track 0 Not Found */
#define ATA_ERR_ABRT          0x04 /* Command Aborted */
#define ATA_ERR_MCR           0x08 /* Media Change Request */
#define ATA_ERR_IDNF          0x10 /* ID Not Found */
#define ATA_ERR_MC            0x20 /* Media Changed */
#define ATA_ERR_UNC           0x40 /* Uncorrectable Data */
#define ATA_ERR_BBK           0x80 /* Bad Block */

/* ATA Commands */
#define ATA_CMD_READ_PIO      0x20 /* Read Sectors (LBA28, with retry) */
#define ATA_CMD_WRITE_PIO     0x30 /* Write Sectors (LBA28, with retry) */
#define ATA_CMD_FLUSH_CACHE   0xE7 /* Flush Cache (LBA28) */
#define ATA_CMD_FLUSH_CACHE_EXT 0xEA /* Flush Cache EXT (LBA48) */
#define ATA_CMD_IDENTIFY      0xEC /* Identify Device */

/* Maximum Timeout Iterations for Bounded Polling */
#define ATA_TIMEOUT_CYCLES    200000

typedef struct ata_channel {
    u16 io_base;
    u16 ctrl_base;
    bool present;
} ata_channel_t;

typedef struct ata_drive {
    u8 channel_idx;      /* 0 = Primary, 1 = Secondary */
    u8 drive_idx;        /* 0 = Master, 1 = Slave */
    u16 io_base;
    u16 ctrl_base;
    bool present;
    bool is_atapi;
    bool lba48_supported;
    bool flush_cache_supported;  /* Word 82 bit 12: FLUSH CACHE (0xE7) supported */
    bool flush_ext_supported;    /* Word 83 bit 13: FLUSH CACHE EXT (0xEA) supported */
    char model[41];
    char serial[21];
    u64 total_sectors;
    block_device_t block_dev;
} ata_drive_t;

/* Public ATA Driver API */
void ata_init(void);

#endif /* ASTRA_ATA_H */


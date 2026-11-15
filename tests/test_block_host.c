#ifdef HOST_TEST

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdarg.h>

void kprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fflush(stdout);
}

#include "../kernel/include/kernel/block.h"
#include "../kernel/drivers/block.c"

/* Mock ATA Hardware Port Environment */
#define MOCK_IO_BASE   0x1F0
#define MOCK_CTRL_BASE 0x3F6

static u8   s_mock_reg_status = 0x40; /* ATA_STATUS_RDY */
static u8   s_mock_reg_error = 0;
static u8   s_mock_reg_command = 0;
static u8   s_mock_last_drive_head = 0;
static int  s_mock_bsy_cycles = 0;
static bool s_mock_permanent_bsy = false;
static int  s_mock_cmd_count = 0;
static u8   s_mock_cmd_log[16];
static bool s_mock_abort_first_ext = false;
static bool s_mock_fail_next_cmd = false;
static u8   s_mock_fail_status = 0;
static u8   s_mock_fail_err = 0;

static inline void io_wait(void) { }
#define kmemset memset
#define kmemcpy memcpy

static inline u8 inb(u16 port) {
    if (port == MOCK_CTRL_BASE || port == MOCK_IO_BASE + 7) {
        if (s_mock_permanent_bsy) {
            return 0x80; /* ATA_STATUS_BSY */
        }
        if (s_mock_bsy_cycles > 0) {
            s_mock_bsy_cycles--;
            return 0x80; /* ATA_STATUS_BSY */
        }
        return s_mock_reg_status;
    }
    if (port == MOCK_IO_BASE + 1) { /* ATA_REG_ERROR */
        return s_mock_reg_error;
    }
    return 0;
}

static inline void outb(u16 port, u8 val) {
    if (port == MOCK_IO_BASE + 7) { /* ATA_REG_COMMAND */
        s_mock_reg_command = val;
        if (s_mock_cmd_count < 16) {
            s_mock_cmd_log[s_mock_cmd_count++] = val;
        }
        if (s_mock_fail_next_cmd) {
            s_mock_reg_status = s_mock_fail_status;
            s_mock_reg_error = s_mock_fail_err;
            s_mock_fail_next_cmd = false;
        } else if (val == 0xEA && s_mock_abort_first_ext) {
            /* Simulate controller aborting 0xEA */
            s_mock_reg_status = 0x01; /* ATA_STATUS_ERR */
            s_mock_reg_error = 0x04;  /* ATA_ERR_ABRT */
        } else {
            /* Standard command response: ready without error */
            if (!s_mock_permanent_bsy) {
                s_mock_reg_status = 0x40; /* ATA_STATUS_RDY */
                s_mock_reg_error = 0;
            }
        }
    } else if (port == MOCK_IO_BASE + 6) { /* ATA_REG_DRIVE_HEAD */
        s_mock_last_drive_head = val;
    }
}

static inline u16 inw(u16 port) { (void)port; return 0; }
static inline void insw(u16 port, void *addr, size_t count) { (void)port; (void)addr; (void)count; }
static inline void outsw(u16 port, const void *addr, size_t count) { (void)port; (void)addr; (void)count; }

#include "../kernel/include/kernel/ata.h"
#include "../kernel/drivers/ata.c"

/* Mock Storage Backend */
#define MOCK_TOTAL_SECTORS 64
static u8 s_mock_storage[MOCK_TOTAL_SECTORS * BLOCK_SECTOR_SIZE];
static bool s_mock_fail_io = false;
static bool s_mock_timeout = false;
static int s_mock_flushes = 0;

static int mock_read_sectors(block_device_t *dev, u64 lba, u32 count, void *buf) {
    (void)dev;
    if (s_mock_fail_io) return BLOCK_ERR_IO;
    if (s_mock_timeout) return BLOCK_ERR_TIMEOUT;
    memcpy(buf, s_mock_storage + (lba * BLOCK_SECTOR_SIZE), count * BLOCK_SECTOR_SIZE);
    return BLOCK_OK;
}

static int mock_write_sectors(block_device_t *dev, u64 lba, u32 count, const void *buf) {
    (void)dev;
    if (s_mock_fail_io) return BLOCK_ERR_IO;
    if (s_mock_timeout) return BLOCK_ERR_TIMEOUT;
    memcpy(s_mock_storage + (lba * BLOCK_SECTOR_SIZE), buf, count * BLOCK_SECTOR_SIZE);
    return BLOCK_OK;
}

static int mock_flush(block_device_t *dev) {
    (void)dev;
    if (s_mock_fail_io) return BLOCK_ERR_IO;
    if (s_mock_timeout) return BLOCK_ERR_TIMEOUT;
    s_mock_flushes++;
    return BLOCK_OK;
}

static block_ops_t s_mock_ops = {
    .read_sectors = mock_read_sectors,
    .write_sectors = mock_write_sectors,
    .flush = mock_flush
};

static void setup_mock_device(block_device_t *dev, const char *name, bool ro, bool boot) {
    memset(dev, 0, sizeof(block_device_t));
    strncpy(dev->name, name, BLOCK_NAME_MAX - 1);
    dev->sector_size = BLOCK_SECTOR_SIZE;
    dev->total_sectors = MOCK_TOTAL_SECTORS;
    dev->capacity_bytes = MOCK_TOTAL_SECTORS * BLOCK_SECTOR_SIZE;
    dev->read_only = ro;
    dev->is_boot_device = boot;
    dev->ops = &s_mock_ops;
}

#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
int run_all_block_host_tests(void) {
    printf("=================================================================\n");
    printf("     AstraOS Phase AD: Block Device Subsystem Host Unit Tests    \n");
    printf("=================================================================\n");

    /* TEST 1: Subsystem Initialization and Registration */
    printf("[TEST 1] Testing Device Registration and Lookup...\n");
    block_init();
    assert(block_get_device_count() == 0);
    assert(block_get_device(0) == NULL);
    assert(block_find_by_name("disk0") == NULL);

    block_device_t raw_dev;
    setup_mock_device(&raw_dev, "disk0", false, false);
    int reg_id = block_register_device(&raw_dev);
    assert(reg_id == 0);
    assert(block_get_device_count() == 1);

    block_device_t *d0 = block_get_device(0);
    assert(d0 != NULL);
    assert(strcmp(d0->name, "disk0") == 0);
    assert(d0->total_sectors == MOCK_TOTAL_SECTORS);
    assert(d0->capacity_bytes == MOCK_TOTAL_SECTORS * BLOCK_SECTOR_SIZE);
    assert(d0->status == BLOCK_STATUS_READY);
    assert(block_find_by_name("disk0") == d0);
    printf("  PASS: Device registration, naming, and lookup verified.\n");

    /* TEST 2: First Valid Sector (LBA 0) Read and Write */
    printf("[TEST 2] Testing Sector 0 (First Sector) Read and Write...\n");
    u8 pat0[BLOCK_SECTOR_SIZE];
    u8 r_buf0[BLOCK_SECTOR_SIZE];
    for (int i = 0; i < BLOCK_SECTOR_SIZE; i++) pat0[i] = (u8)(i ^ 0x5A);

    int w0 = block_write(d0, 0, 1, pat0);
    assert(w0 == BLOCK_OK);
    int r0 = block_read(d0, 0, 1, r_buf0);
    assert(r0 == BLOCK_OK);
    assert(memcmp(pat0, r_buf0, BLOCK_SECTOR_SIZE) == 0);
    printf("  PASS: LBA 0 read/write matches byte-for-byte.\n");

    /* TEST 3: Last Valid Sector (LBA total_sectors - 1) */
    printf("[TEST 3] Testing Last Valid Sector (LBA %d)...\n", MOCK_TOTAL_SECTORS - 1);
    u8 pat_last[BLOCK_SECTOR_SIZE];
    u8 r_last[BLOCK_SECTOR_SIZE];
    for (int i = 0; i < BLOCK_SECTOR_SIZE; i++) pat_last[i] = (u8)(i ^ 0x33);

    u64 last_lba = MOCK_TOTAL_SECTORS - 1;
    assert(block_write(d0, last_lba, 1, pat_last) == BLOCK_OK);
    assert(block_read(d0, last_lba, 1, r_last) == BLOCK_OK);
    assert(memcmp(pat_last, r_last, BLOCK_SECTOR_SIZE) == 0);
    printf("  PASS: Last sector read/write verified.\n");

    /* TEST 4: Exact Sector Boundary Behavior */
    printf("[TEST 4] Testing Exact Sector Boundary Conditions...\n");
    u8 multi_buf[BLOCK_SECTOR_SIZE * 2];
    /* Exactly touches last sector: LBA 62, count 2 -> sectors 62, 63 -> MUST PASS */
    assert(block_read(d0, MOCK_TOTAL_SECTORS - 2, 2, multi_buf) == BLOCK_OK);
    assert(block_write(d0, MOCK_TOTAL_SECTORS - 2, 2, multi_buf) == BLOCK_OK);
    /* Straddles boundary: LBA 62, count 3 -> sectors 62, 63, 64 -> MUST FAIL OUT_OF_BOUNDS */
    assert(block_read(d0, MOCK_TOTAL_SECTORS - 2, 3, multi_buf) == BLOCK_ERR_OUT_OF_BOUNDS);
    assert(block_write(d0, MOCK_TOTAL_SECTORS - 2, 3, multi_buf) == BLOCK_ERR_OUT_OF_BOUNDS);
    printf("  PASS: Exact boundary conditions enforced.\n");

    /* TEST 5: Out-of-Range Requests */
    printf("[TEST 5] Testing Out-of-Range Requests...\n");
    assert(block_read(d0, MOCK_TOTAL_SECTORS, 1, multi_buf) == BLOCK_ERR_OUT_OF_BOUNDS);
    assert(block_write(d0, MOCK_TOTAL_SECTORS, 1, multi_buf) == BLOCK_ERR_OUT_OF_BOUNDS);
    assert(block_read(d0, 10000, 1, multi_buf) == BLOCK_ERR_OUT_OF_BOUNDS);
    assert(block_write(d0, 10000, 1, multi_buf) == BLOCK_ERR_OUT_OF_BOUNDS);
    printf("  PASS: Out-of-bounds requests safely rejected.\n");

    /* TEST 6: Zero-Sector Requests */
    printf("[TEST 6] Testing Zero-Sector Requests...\n");
    assert(block_read(d0, 0, 0, multi_buf) == 0);
    assert(block_write(d0, 0, 0, multi_buf) == 0);
    assert(block_read(d0, 10, 0, multi_buf) == 0);
    assert(block_write(d0, 10, 0, multi_buf) == 0);
    printf("  PASS: Zero-sector requests return 0 without I/O.\n");

    /* TEST 7: Integer Overflow in LBA Calculation */
    printf("[TEST 7] Testing Integer Overflow Handling...\n");
    assert(block_read(d0, 0xFFFFFFFFFFFFFFFFULL, 1, multi_buf) == BLOCK_ERR_OVERFLOW);
    assert(block_write(d0, 0xFFFFFFFFFFFFFFFFULL, 1, multi_buf) == BLOCK_ERR_OVERFLOW);
    assert(block_read(d0, 0xFFFFFFFFFFFFFFFFULL, 100, multi_buf) == BLOCK_ERR_OVERFLOW);
    assert(block_write(d0, 0xFFFFFFFFFFFFFFFFULL, 100, multi_buf) == BLOCK_ERR_OVERFLOW);
    printf("  PASS: Integer overflow trapped before dispatch.\n");

    /* TEST 8: Invalid Buffers and Parameters */
    printf("[TEST 8] Testing NULL Buffer and NULL Device Handling...\n");
    assert(block_read(NULL, 0, 1, multi_buf) == BLOCK_ERR_INVALID_PARAM);
    assert(block_write(NULL, 0, 1, multi_buf) == BLOCK_ERR_INVALID_PARAM);
    assert(block_flush(NULL) == BLOCK_ERR_INVALID_PARAM);
    assert(block_read(d0, 0, 1, NULL) == BLOCK_ERR_INVALID_PARAM);
    assert(block_write(d0, 0, 1, NULL) == BLOCK_ERR_INVALID_PARAM);
    printf("  PASS: NULL buffer/device checks verified.\n");

    /* TEST 9: Read-Only and Boot Device Protections */
    printf("[TEST 9] Testing Read-Only and Boot Device Write Protection...\n");
    block_device_t ro_dev;
    setup_mock_device(&ro_dev, "ro_disk", true, false);
    block_register_device(&ro_dev);

    block_device_t boot_dev;
    setup_mock_device(&boot_dev, "boot_disk", false, true);
    block_register_device(&boot_dev);

    block_device_t *d_ro = block_find_by_name("ro_disk");
    block_device_t *d_boot = block_find_by_name("boot_disk");
    assert(d_ro != NULL && d_boot != NULL);

    /* Reads succeed on RO and Boot devices */
    assert(block_read(d_ro, 0, 1, multi_buf) == BLOCK_OK);
    assert(block_read(d_boot, 0, 1, multi_buf) == BLOCK_OK);

    /* Writes MUST fail with BLOCK_ERR_READ_ONLY */
    assert(block_write(d_ro, 0, 1, multi_buf) == BLOCK_ERR_READ_ONLY);
    assert(block_write(d_boot, 0, 1, multi_buf) == BLOCK_ERR_READ_ONLY);
    printf("  PASS: Read-only and boot devices reject writes with BLOCK_ERR_READ_ONLY.\n");

    /* TEST 10: Error Propagation and Flush */
    printf("[TEST 10] Testing Device Error Propagation and Cache Flush...\n");
    s_mock_fail_io = true;
    assert(block_read(d0, 0, 1, multi_buf) == BLOCK_ERR_IO);
    assert(block_write(d0, 0, 1, multi_buf) == BLOCK_ERR_IO);
    assert(block_flush(d0) == BLOCK_ERR_IO);
    s_mock_fail_io = false;

    s_mock_timeout = true;
    assert(block_read(d0, 0, 1, multi_buf) == BLOCK_ERR_TIMEOUT);
    assert(block_write(d0, 0, 1, multi_buf) == BLOCK_ERR_TIMEOUT);
    assert(block_flush(d0) == BLOCK_ERR_TIMEOUT);
    s_mock_timeout = false;

    s_mock_flushes = 0;
    assert(block_flush(d0) == BLOCK_OK);
    assert(s_mock_flushes == 1);
    printf("  PASS: I/O errors and timeouts cleanly propagated; flush operational.\n");

    /* TEST 11: Device Registration Capacity Limit */
    printf("[TEST 11] Testing Device Registration Capacity Bound (MAX_DEVICES=4)...\n");
    block_device_t dev4;
    setup_mock_device(&dev4, "disk3", false, false);
    assert(block_register_device(&dev4) == 3);
    assert(block_get_device_count() == 4);

    block_device_t dev_overflow;
    setup_mock_device(&dev_overflow, "disk4_overflow", false, false);
    assert(block_register_device(&dev_overflow) == BLOCK_ERR_OUT_OF_BOUNDS);
    assert(block_get_device_count() == 4);
    printf("  PASS: Device registration bounds strictly enforced.\n");

    /* =================================================================
     * Phase AJ Task 1: Realistic ATA Flush Command Correctness Tests
     * ================================================================= */
    printf("\n--- Phase AJ: ATA Flush Command Correctness & Hardware Fault Tests ---\n");

    ata_drive_t ata_test_drv;
    memset(&ata_test_drv, 0, sizeof(ata_test_drv));
    ata_test_drv.io_base = MOCK_IO_BASE;
    ata_test_drv.ctrl_base = MOCK_CTRL_BASE;
    ata_test_drv.present = true;
    ata_test_drv.block_dev.ops = &s_ata_ops;
    ata_test_drv.block_dev.priv = &ata_test_drv;
    ata_test_drv.block_dev.status = BLOCK_STATUS_READY;
    strncpy(ata_test_drv.block_dev.name, "ata_test", sizeof(ata_test_drv.block_dev.name) - 1);

    /* TEST 12: Standard LBA28 ATA Flush (0xE7 command) */
    printf("[TEST 12] Testing standard LBA28 ATA Flush Cache (0xE7 command)...\n");
    s_mock_reg_status = 0x40; /* ATA_STATUS_RDY */
    s_mock_reg_error = 0;
    s_mock_cmd_count = 0;
    s_mock_reg_command = 0;
    ata_test_drv.flush_cache_supported = true;
    ata_test_drv.flush_ext_supported = false;
    ata_test_drv.lba48_supported = false;
    ata_test_drv.total_sectors = 10000;

    assert(block_flush(&ata_test_drv.block_dev) == BLOCK_OK);
    assert(s_mock_reg_command == ATA_CMD_FLUSH_CACHE);
    assert(s_mock_cmd_count == 1);
    assert(s_mock_cmd_log[0] == 0xE7);
    printf("  PASS: 0xE7 FLUSH CACHE emitted and completed successfully.\n");

    /* TEST 13: Large Capacity LBA48 ATA Flush (0xEA command) */
    printf("[TEST 13] Testing 48-bit LBA (> 128 GiB) ATA Flush Cache EXT (0xEA command)...\n");
    s_mock_reg_status = 0x40;
    s_mock_reg_error = 0;
    s_mock_cmd_count = 0;
    s_mock_reg_command = 0;
    ata_test_drv.flush_cache_supported = true;
    ata_test_drv.flush_ext_supported = true;
    ata_test_drv.lba48_supported = true;
    ata_test_drv.total_sectors = 0x20000000ULL; /* Exceeds 128 GiB */

    assert(block_flush(&ata_test_drv.block_dev) == BLOCK_OK);
    assert(s_mock_reg_command == ATA_CMD_FLUSH_CACHE_EXT);
    assert(s_mock_cmd_count == 1);
    assert(s_mock_cmd_log[0] == 0xEA);
    printf("  PASS: 0xEA FLUSH CACHE EXT emitted for large LBA48 volume.\n");

    /* TEST 14: Unsupported Flush Command Handling */
    printf("[TEST 14] Testing device without flush command support...\n");
    s_mock_cmd_count = 0;
    s_mock_reg_command = 0;
    ata_test_drv.flush_cache_supported = false;
    ata_test_drv.flush_ext_supported = false;
    ata_test_drv.lba48_supported = false;
    ata_test_drv.total_sectors = 1000;

    assert(block_flush(&ata_test_drv.block_dev) == BLOCK_ERR_UNSUPPORTED);
    assert(s_mock_cmd_count == 0); /* No invalid command sent down wire */
    printf("  PASS: Device without flush capabilities cleanly returns BLOCK_ERR_UNSUPPORTED.\n");

    /* TEST 15: Controller Abort of 0xEA with Automatic Fallback to 0xE7 */
    printf("[TEST 15] Testing 0xEA abort with automatic fallback to 0xE7...\n");
    s_mock_cmd_count = 0;
    s_mock_reg_command = 0;
    s_mock_abort_first_ext = true; /* Abort 0xEA, subsequent 0xE7 succeeds */
    ata_test_drv.flush_cache_supported = true;
    ata_test_drv.flush_ext_supported = true;
    ata_test_drv.lba48_supported = true;
    ata_test_drv.total_sectors = 0x20000000ULL;

    assert(block_flush(&ata_test_drv.block_dev) == BLOCK_OK);
    assert(s_mock_cmd_count == 2);
    assert(s_mock_cmd_log[0] == 0xEA); /* First attempted 0xEA */
    assert(s_mock_cmd_log[1] == 0xE7); /* Abort caught; retried with 0xE7 and succeeded */
    s_mock_abort_first_ext = false;
    printf("  PASS: 0xEA abort correctly triggered fallback to 0xE7 with successful commit.\n");

    /* TEST 16: Controller Abort of Standard Flush (0xE7) Without Fallback */
    printf("[TEST 16] Testing abort of 0xE7 command (unsupported/invalid command)...\n");
    s_mock_cmd_count = 0;
    s_mock_fail_next_cmd = true;
    s_mock_fail_status = ATA_STATUS_ERR;
    s_mock_fail_err = ATA_ERR_ABRT;
    ata_test_drv.flush_cache_supported = true;
    ata_test_drv.flush_ext_supported = false;
    ata_test_drv.lba48_supported = false;
    ata_test_drv.total_sectors = 1000;

    assert(block_flush(&ata_test_drv.block_dev) == BLOCK_ERR_IO);
    assert(s_mock_cmd_count == 1);
    assert(s_mock_cmd_log[0] == 0xE7);
    printf("  PASS: Aborted 0xE7 flush returned BLOCK_ERR_IO without false success.\n");

    /* TEST 17: Hardware Device Error (ERR) and Drive Fault (DF) */
    printf("[TEST 17] Testing hardware ERR and Drive Fault (DF) flags...\n");
    s_mock_fail_next_cmd = true;
    s_mock_fail_status = ATA_STATUS_ERR;
    s_mock_fail_err = ATA_ERR_UNC;
    assert(block_flush(&ata_test_drv.block_dev) == BLOCK_ERR_IO);

    s_mock_fail_next_cmd = true;
    s_mock_fail_status = ATA_STATUS_DF;
    s_mock_fail_err = 0;
    assert(block_flush(&ata_test_drv.block_dev) == BLOCK_ERR_IO);
    printf("  PASS: Hardware ERR and DF flags return BLOCK_ERR_IO.\n");

    /* TEST 18: Illegal DRQ Assertion During Flush Command */
    printf("[TEST 18] Testing illegal DRQ assertion on non-data flush command...\n");
    s_mock_fail_next_cmd = true;
    s_mock_fail_status = ATA_STATUS_RDY | ATA_STATUS_DRQ;
    s_mock_fail_err = 0;
    assert(block_flush(&ata_test_drv.block_dev) == BLOCK_ERR_IO);
    printf("  PASS: Illegal DRQ on non-data command trapped as BLOCK_ERR_IO.\n");

    /* TEST 19: Permanent Busy (BSY) Condition & Timeout */
    printf("[TEST 19] Testing permanent BSY condition timeout...\n");
    s_mock_permanent_bsy = true;
    assert(block_flush(&ata_test_drv.block_dev) == BLOCK_ERR_TIMEOUT);
    s_mock_permanent_bsy = false;
    s_mock_reg_status = 0x40;
    printf("  PASS: Bounded timeout loop enforces termination with BLOCK_ERR_TIMEOUT.\n");

    /* TEST 20: Detached Device & Null Parameter Guards */
    printf("[TEST 20] Testing detached device & parameter validations...\n");
    assert(block_flush(NULL) == BLOCK_ERR_INVALID_PARAM);

    ata_test_drv.present = false;
    assert(block_flush(&ata_test_drv.block_dev) == BLOCK_ERR_NO_DEVICE);
    ata_test_drv.present = true;

    ata_test_drv.block_dev.status = BLOCK_STATUS_OFFLINE;
    assert(block_flush(&ata_test_drv.block_dev) == BLOCK_ERR_NO_DEVICE);
    ata_test_drv.block_dev.status = BLOCK_STATUS_READY;
    printf("  PASS: Detached device, offline status, and NULL pointers safely guarded.\n");

    printf("\n=================================================================\n");
    printf("   [SUCCESS] ALL 20 BLOCK DEVICE & ATA FLUSH HOST TESTS PASSED   \n");
    printf("=================================================================\n");
    return 0;
}

#ifndef HOST_DLL
int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    return run_all_block_host_tests();
}
#endif

#endif /* HOST_TEST */

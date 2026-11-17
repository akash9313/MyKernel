#ifndef ASTRA_KERNEL_ACPI_H
#define ASTRA_KERNEL_ACPI_H

#include <kernel/types.h>

/* Generic Address Structure (GAS) */
typedef struct {
    u8  address_space_id; /* 0 = System Memory, 1 = System I/O, 2 = PCI Config Space */
    u8  register_bit_width;
    u8  register_bit_offset;
    u8  access_size;
    u64 address;
} __attribute__((packed)) acpi_gas_t;

/* Standard ACPI Table Header */
typedef struct {
    char signature[4];
    u32  length;
    u8   revision;
    u8   checksum;
    char oem_id[6];
    char oem_table_id[8];
    u32  oem_revision;
    u32  creator_id;
    u32  creator_revision;
} __attribute__((packed)) acpi_sdt_header_t;

/* Root System Description Pointer (RSDP) */
typedef struct {
    char signature[8];       /* "RSD PTR " */
    u8   checksum;
    char oem_id[6];
    u8   revision;           /* 0 = ACPI 1.0, 2 = ACPI 2.0+ */
    u32  rsdt_address;
    /* ACPI 2.0+ fields */
    u32  length;
    u64  xsdt_address;
    u8   extended_checksum;
    u8   reserved[3];
} __attribute__((packed)) acpi_rsdp_t;

/* Fixed ACPI Description Table (FADT) */
typedef struct {
    acpi_sdt_header_t header;
    u32 firmware_ctrl;
    u32 dsdt;                /* 32-bit physical address of DSDT */
    u8  reserved;
    u8  preferred_pm_profile;
    u16 sci_int;
    u32 smi_cmd;             /* SMI command port */
    u8  acpi_enable;         /* Value to write to smi_cmd to enable ACPI */
    u8  acpi_disable;        /* Value to write to smi_cmd to disable ACPI */
    u8  s4bios_req;
    u8  pstate_cnt;
    u32 pm1a_evt_blk;
    u32 pm1b_evt_blk;
    u32 pm1a_cnt_blk;        /* Port for PM1a control */
    u32 pm1b_cnt_blk;        /* Port for PM1b control */
    u32 pm2_cnt_blk;
    u32 pm_tmr_blk;
    u32 gpe0_blk;
    u32 gpe1_blk;
    u8  pm1_evt_len;
    u8  pm1_cnt_len;
    u8  pm2_cnt_len;
    u8  pm_tmr_len;
    u8  gpe0_blk_len;
    u8  gpe1_blk_len;
    u8  gpe1_base;
    u8  cst_cnt;
    u16 p_lvl2_lat;
    u16 p_lvl3_lat;
    u16 flush_size;
    u16 flush_stride;
    u8  duty_offset;
    u8  duty_width;
    u8  day_alrm;
    u8  mon_alrm;
    u8  century;
    u16 iapc_boot_arch;
    u8  reserved1;
    u32 flags;               /* Bit 10: RESET_REG_SUP */
    acpi_gas_t reset_reg;    /* Generic Address Structure for Reset Register (offset 116..127) */
    u8  reset_value;         /* Value to write to reset_reg (offset 128) */
    u8  reserved2[3];        /* Reserved / ARM boot arch / minor version (offset 129..131) */
    u64 x_firmware_ctrl;     /* 64-bit physical address of Firmware Control (offset 132..139) */
    u64 x_dsdt;              /* 64-bit physical address of DSDT (offset 140..147) */
    acpi_gas_t x_pm1a_evt_blk; /* offset 148..159 */
    acpi_gas_t x_pm1b_evt_blk; /* offset 160..171 */
    acpi_gas_t x_pm1a_cnt_blk; /* offset 172..183 */
    acpi_gas_t x_pm1b_cnt_blk; /* offset 184..195 */
} __attribute__((packed)) acpi_fadt_t;

/* ACPI Subsystem Public API */
int  acpi_init(void);
bool acpi_is_available(void);
bool acpi_is_s5_supported(void);
bool acpi_is_s3_supported(void);
bool acpi_is_reset_supported(void);
int  acpi_poweroff(void);
int  acpi_reboot(void);

#endif /* ASTRA_KERNEL_ACPI_H */

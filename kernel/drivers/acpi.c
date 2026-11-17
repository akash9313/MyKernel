#include <kernel/acpi.h>
#include <kernel/boot.h>
#include <kernel/kprintf.h>
#include <kernel/arch/x86_64/cpu.h>
#include <kernel/mem.h>

typedef struct {
    bool       available;
    bool       s5_supported;
    bool       s3_supported;
    bool       reset_supported;
    u16        pm1a_cnt;
    u16        pm1b_cnt;
    u16        smi_cmd;
    u8         acpi_enable;
    u8         acpi_disable;
    u16        slp_typa_s5;
    u16        slp_typb_s5;
    u16        slp_typa_s3;
    u16        slp_typb_s3;
    acpi_gas_t reset_reg;
    u8         reset_value;
} acpi_state_t;

static acpi_state_t g_acpi = {0};

static inline void *phys_to_virt(u64 phys_addr) {
    struct limine_hhdm_response *hhdm = boot_get_hhdm_response();
    u64 offset = hhdm ? hhdm->offset : 0;
    if (phys_addr >= offset) {
        return (void *)phys_addr;
    }
    return (void *)(offset + phys_addr);
}

static bool acpi_validate_checksum(const void *ptr, size_t length) {
    const u8 *bytes = (const u8 *)ptr;
    u8 sum = 0;
    for (size_t i = 0; i < length; i++) {
        sum = (u8)(sum + bytes[i]);
    }
    return (sum == 0);
}

static const acpi_rsdp_t *acpi_find_rsdp(void) {
    /* 1. Check Limine bootloader response */
    struct limine_rsdp_response *resp = boot_get_rsdp_response();
    if (resp && resp->address) {
        const acpi_rsdp_t *rsdp = (const acpi_rsdp_t *)phys_to_virt((u64)(uintptr_t)resp->address);
        if (kmemcmp(rsdp->signature, "RSD PTR ", 8) == 0 && acpi_validate_checksum(rsdp, 20)) {
            return rsdp;
        }
    }

    /* 2. Fallback scan EBDA (Extended BIOS Data Area) */
    u16 ebda_seg = *(const u16 *)phys_to_virt(0x40E);
    if (ebda_seg) {
        u64 ebda_phys = (u64)ebda_seg << 4;
        const u8 *p = (const u8 *)phys_to_virt(ebda_phys);
        for (size_t i = 0; i < 1024; i += 16) {
            const acpi_rsdp_t *r = (const acpi_rsdp_t *)(p + i);
            if (kmemcmp(r->signature, "RSD PTR ", 8) == 0 && acpi_validate_checksum(r, 20)) {
                return r;
            }
        }
    }

    /* 3. Fallback scan Main BIOS ROM Area (0xE0000 - 0xFFFFF) */
    const u8 *rom = (const u8 *)phys_to_virt(0xE0000);
    for (size_t i = 0; i < 0x20000; i += 16) {
        const acpi_rsdp_t *r = (const acpi_rsdp_t *)(rom + i);
        if (kmemcmp(r->signature, "RSD PTR ", 8) == 0 && acpi_validate_checksum(r, 20)) {
            return r;
        }
    }

    return NULL;
}

static void parse_aml_sleep_package(const u8 *aml, size_t len, const char *pkg_name, u16 *out_typa, u16 *out_typb, bool *out_ok) {
    *out_ok = false;
    for (size_t i = 0; i + 8 < len; i++) {
        /* Match NameOp (0x08) followed by 4-byte package name and PackageOp (0x12) */
        if (aml[i] == 0x08 &&
            aml[i+1] == pkg_name[0] && aml[i+2] == pkg_name[1] &&
            aml[i+3] == pkg_name[2] && aml[i+4] == pkg_name[3] &&
            aml[i+5] == 0x12) {

            const u8 *p = &aml[i + 6];
            /* Skip AML package length encoding */
            u8 lead = *p++;
            if ((lead & 0xC0) == 0) {
                /* 1-byte length already consumed */
            } else if ((lead & 0xC0) == 0x40) {
                p += 1;
            } else if ((lead & 0xC0) == 0x80) {
                p += 2;
            } else if ((lead & 0xC0) == 0xC0) {
                p += 3;
            }

            u8 num_elements = *p++;
            if (num_elements >= 1) {
                /* Element 0: SLP_TYPa */
                u16 typa = 0;
                if (*p == 0x0A) { p++; typa = *p++; }
                else if (*p == 0x00) { typa = 0; p++; }
                else if (*p == 0x01) { typa = 1; p++; }
                else if (*p == 0x0B) { p++; typa = (u16)(p[0] | (p[1] << 8)); p += 2; }
                else { typa = *p++; }

                /* Element 1: SLP_TYPb */
                u16 typb = 0;
                if (num_elements >= 2) {
                    if (*p == 0x0A) { p++; typb = *p++; }
                    else if (*p == 0x00) { typb = 0; p++; }
                    else if (*p == 0x01) { typb = 1; p++; }
                    else if (*p == 0x0B) { p++; typb = (u16)(p[0] | (p[1] << 8)); p += 2; }
                    else { typb = *p++; }
                }

                *out_typa = typa;
                *out_typb = typb;
                *out_ok = true;
                return;
            }
        }
    }
}

int acpi_init(void) {
    kprintf("[ACPI] Probing ACPI tables...\n");
    const acpi_rsdp_t *rsdp = acpi_find_rsdp();
    if (!rsdp) {
        kprintf("[ACPI] No ACPI RSDP table found. Platform power management will use fallback ports.\n");
        return -1;
    }

    kprintf("[ACPI] Found RSDP (OEM: %.6s, Rev: %u)\n", rsdp->oem_id, rsdp->revision);

    const acpi_fadt_t *fadt = NULL;

    /* Check XSDT (ACPI 2.0+) first, then RSDT */
    if (rsdp->revision >= 2 && rsdp->xsdt_address != 0) {
        const acpi_sdt_header_t *xsdt = (const acpi_sdt_header_t *)phys_to_virt(rsdp->xsdt_address);
        if (acpi_validate_checksum(xsdt, xsdt->length)) {
            size_t count = (xsdt->length - sizeof(acpi_sdt_header_t)) / sizeof(u64);
            const u64 *tables = (const u64 *)(const void *)((const u8 *)xsdt + sizeof(acpi_sdt_header_t));
            for (size_t i = 0; i < count; i++) {
                const acpi_sdt_header_t *hdr = (const acpi_sdt_header_t *)phys_to_virt(tables[i]);
                if (kmemcmp(hdr->signature, "FACP", 4) == 0 && acpi_validate_checksum(hdr, hdr->length)) {
                    fadt = (const acpi_fadt_t *)hdr;
                    break;
                }
            }
        }
    }

    if (!fadt && rsdp->rsdt_address != 0) {
        const acpi_sdt_header_t *rsdt = (const acpi_sdt_header_t *)phys_to_virt((u64)rsdp->rsdt_address);
        if (acpi_validate_checksum(rsdt, rsdt->length)) {
            size_t count = (rsdt->length - sizeof(acpi_sdt_header_t)) / sizeof(u32);
            const u32 *tables = (const u32 *)(const void *)((const u8 *)rsdt + sizeof(acpi_sdt_header_t));
            for (size_t i = 0; i < count; i++) {
                const acpi_sdt_header_t *hdr = (const acpi_sdt_header_t *)phys_to_virt((u64)tables[i]);
                if (kmemcmp(hdr->signature, "FACP", 4) == 0 && acpi_validate_checksum(hdr, hdr->length)) {
                    fadt = (const acpi_fadt_t *)hdr;
                    break;
                }
            }
        }
    }

    if (!fadt) {
        kprintf("[ACPI] FADT (FACP) not found!\n");
        return -2;
    }

    g_acpi.available = true;
    g_acpi.pm1a_cnt = (u16)fadt->pm1a_cnt_blk;
    g_acpi.pm1b_cnt = (u16)fadt->pm1b_cnt_blk;
    g_acpi.smi_cmd = (u16)fadt->smi_cmd;
    g_acpi.acpi_enable = fadt->acpi_enable;
    g_acpi.acpi_disable = fadt->acpi_disable;

    kprintf("[ACPI] FADT parsed: PM1a_CNT=0x%x, PM1b_CNT=0x%x, SMI_CMD=0x%x, ACPI_ENABLE=0x%x\n",
            g_acpi.pm1a_cnt, g_acpi.pm1b_cnt, g_acpi.smi_cmd, g_acpi.acpi_enable);

    /* ACPI 2.0+ Reset Register support (bit 10 of flags) */
    if (fadt->header.length >= 129 && (fadt->flags & (1 << 10))) {
        g_acpi.reset_reg = fadt->reset_reg;
        g_acpi.reset_value = fadt->reset_value;
        if (g_acpi.reset_reg.address != 0) {
            g_acpi.reset_supported = true;
            kprintf("[ACPI] Reset register supported: Space=%u, Addr=0x%llx, Val=0x%x\n",
                    g_acpi.reset_reg.address_space_id, (unsigned long long)g_acpi.reset_reg.address, g_acpi.reset_value);
        }
    }

    /* Locate and parse DSDT */
    u64 dsdt_addr = 0;
    if (fadt->header.length >= 148 && fadt->x_dsdt != 0) {
        dsdt_addr = fadt->x_dsdt;
    } else if (fadt->dsdt != 0) {
        dsdt_addr = (u64)fadt->dsdt;
    }

    if (dsdt_addr) {
        kprintf("[ACPI] DSDT address: 0x%llx (x_dsdt=0x%llx, dsdt=0x%x)\n",
                (unsigned long long)dsdt_addr, (unsigned long long)fadt->x_dsdt, fadt->dsdt);
        const acpi_sdt_header_t *dsdt = (const acpi_sdt_header_t *)phys_to_virt(dsdt_addr);
        if (kmemcmp(dsdt->signature, "DSDT", 4) == 0 && acpi_validate_checksum(dsdt, dsdt->length)) {
            const u8 *aml = (const u8 *)dsdt + sizeof(acpi_sdt_header_t);
            size_t aml_len = dsdt->length - sizeof(acpi_sdt_header_t);

            /* Parse _S5_ */
            parse_aml_sleep_package(aml, aml_len, "_S5_", &g_acpi.slp_typa_s5, &g_acpi.slp_typb_s5, &g_acpi.s5_supported);
            if (!g_acpi.s5_supported) {
                parse_aml_sleep_package(aml, aml_len, "\\_S5", &g_acpi.slp_typa_s5, &g_acpi.slp_typb_s5, &g_acpi.s5_supported);
            }
            if (g_acpi.s5_supported) {
                kprintf("[ACPI] S5 Sleep state supported (SLP_TYPa=%u, SLP_TYPb=%u)\n",
                        g_acpi.slp_typa_s5, g_acpi.slp_typb_s5);
            }

            /* Parse _S3_ */
            parse_aml_sleep_package(aml, aml_len, "_S3_", &g_acpi.slp_typa_s3, &g_acpi.slp_typb_s3, &g_acpi.s3_supported);
            if (!g_acpi.s3_supported) {
                parse_aml_sleep_package(aml, aml_len, "\\_S3", &g_acpi.slp_typa_s3, &g_acpi.slp_typb_s3, &g_acpi.s3_supported);
            }
            if (g_acpi.s3_supported) {
                kprintf("[ACPI] S3 Sleep state found in DSDT (SLP_TYPa=%u, SLP_TYPb=%u)\n",
                        g_acpi.slp_typa_s3, g_acpi.slp_typb_s3);
            }
        }
    }

    return 0;
}

bool acpi_is_available(void) {
    return g_acpi.available;
}

bool acpi_is_s5_supported(void) {
    return g_acpi.available && g_acpi.s5_supported && (g_acpi.pm1a_cnt != 0);
}

bool acpi_is_s3_supported(void) {
    return g_acpi.available && g_acpi.s3_supported && (g_acpi.pm1a_cnt != 0);
}

bool acpi_is_reset_supported(void) {
    return g_acpi.available && g_acpi.reset_supported;
}

int acpi_poweroff(void) {
    if (!acpi_is_s5_supported()) {
        return -1;
    }

    /* 1. Enable ACPI mode if not already active */
    if (g_acpi.smi_cmd && g_acpi.acpi_enable) {
        if ((inw(g_acpi.pm1a_cnt) & 1) == 0) {
            outb(g_acpi.smi_cmd, g_acpi.acpi_enable);
            /* Bounded wait for SCI_EN (bit 0) */
            for (int i = 0; i < 30000; i++) {
                if (inw(g_acpi.pm1a_cnt) & 1) break;
                io_wait();
            }
        }
    }

    /* 2. Program PM1a / PM1b control registers */
    /* Bit 13 is SLP_EN, bits 10-12 are SLP_TYP */
    u16 val_a = (u16)((g_acpi.slp_typa_s5 << 10) | (1 << 13));
    outw(g_acpi.pm1a_cnt, val_a);

    if (g_acpi.pm1b_cnt) {
        u16 val_b = (u16)((g_acpi.slp_typb_s5 << 10) | (1 << 13));
        outw(g_acpi.pm1b_cnt, val_b);
    }

    return 0;
}

int acpi_reboot(void) {
    if (!acpi_is_reset_supported()) {
        return -1;
    }

    if (g_acpi.reset_reg.address_space_id == 1) {
        /* System I/O */
        outb((u16)g_acpi.reset_reg.address, g_acpi.reset_value);
        return 0;
    } else if (g_acpi.reset_reg.address_space_id == 0) {
        /* System Memory */
        volatile u8 *reg = (volatile u8 *)phys_to_virt(g_acpi.reset_reg.address);
        *reg = g_acpi.reset_value;
        return 0;
    }

    return -2;
}

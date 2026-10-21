#include <kernel/pci.h>
#include <kernel/arch/x86_64/cpu.h>
#include <kernel/kprintf.h>
#include <kernel/mem.h>

static pci_device_t g_pci_devices[PCI_MAX_DEVICES];
static int g_pci_device_count = 0;

static u32 pci_config_addr(u8 bus, u8 slot, u8 func, u8 offset) {
    return ((u32)1 << 31) |
           ((u32)bus << 16) |
           ((u32)(slot & 0x1F) << 11) |
           ((u32)(func & 0x07) << 8) |
           (offset & 0xFC);
}

u32 pci_read32(u8 bus, u8 slot, u8 func, u8 offset) {
    outl(PCI_CONFIG_ADDR, pci_config_addr(bus, slot, func, offset));
    return inl(PCI_CONFIG_DATA);
}

u16 pci_read16(u8 bus, u8 slot, u8 func, u8 offset) {
    outl(PCI_CONFIG_ADDR, pci_config_addr(bus, slot, func, offset));
    u32 val = inl(PCI_CONFIG_DATA);
    return (u16)((val >> ((offset & 2) * 8)) & 0xFFFF);
}

u8 pci_read8(u8 bus, u8 slot, u8 func, u8 offset) {
    outl(PCI_CONFIG_ADDR, pci_config_addr(bus, slot, func, offset));
    u32 val = inl(PCI_CONFIG_DATA);
    return (u8)((val >> ((offset & 3) * 8)) & 0xFF);
}

void pci_write32(u8 bus, u8 slot, u8 func, u8 offset, u32 val) {
    outl(PCI_CONFIG_ADDR, pci_config_addr(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, val);
}

void pci_write16(u8 bus, u8 slot, u8 func, u8 offset, u16 val) {
    u32 orig = pci_read32(bus, slot, func, offset & 0xFC);
    int shift = (offset & 2) * 8;
    orig &= ~(0xFFFF << shift);
    orig |= ((u32)val << shift);
    pci_write32(bus, slot, func, offset & 0xFC, orig);
}

void pci_write8(u8 bus, u8 slot, u8 func, u8 offset, u8 val) {
    u32 orig = pci_read32(bus, slot, func, offset & 0xFC);
    int shift = (offset & 3) * 8;
    orig &= ~(0xFF << shift);
    orig |= ((u32)val << shift);
    pci_write32(bus, slot, func, offset & 0xFC, orig);
}

void pci_enable_bus_master(const pci_device_t *dev) {
    if (!dev) return;
    u16 cmd = pci_read16(dev->bus, dev->slot, dev->func, PCI_REG_COMMAND);
    cmd |= (PCI_CMD_BUS_MASTER | PCI_CMD_MEM_SPACE);
    pci_write16(dev->bus, dev->slot, dev->func, PCI_REG_COMMAND, cmd);
}

void pci_enable_mem_space(const pci_device_t *dev) {
    if (!dev) return;
    u16 cmd = pci_read16(dev->bus, dev->slot, dev->func, PCI_REG_COMMAND);
    cmd |= PCI_CMD_MEM_SPACE;
    pci_write16(dev->bus, dev->slot, dev->func, PCI_REG_COMMAND, cmd);
}

static void pci_probe_device(u8 bus, u8 slot, u8 func) {
    u16 vendor = pci_read16(bus, slot, func, PCI_REG_VENDOR_ID);
    if (vendor == 0xFFFF || vendor == 0x0000) return;

    u16 device = pci_read16(bus, slot, func, PCI_REG_DEVICE_ID);

    if (g_pci_device_count >= PCI_MAX_DEVICES) return;

    pci_device_t *dev = &g_pci_devices[g_pci_device_count++];
    kmemset(dev, 0, sizeof(pci_device_t));

    dev->bus = bus;
    dev->slot = slot;
    dev->func = func;
    dev->vendor_id = vendor;
    dev->device_id = device;
    dev->revision = pci_read8(bus, slot, func, PCI_REG_REVISION);
    dev->prog_if = pci_read8(bus, slot, func, PCI_REG_PROG_IF);
    dev->subclass = pci_read8(bus, slot, func, PCI_REG_SUBCLASS);
    dev->class_code = pci_read8(bus, slot, func, PCI_REG_CLASS);
    dev->irq_line = pci_read8(bus, slot, func, PCI_REG_INTERRUPT_LINE);
    dev->irq_pin = pci_read8(bus, slot, func, PCI_REG_INTERRUPT_PIN);

    /* Inspect BARs 0..5 */
    for (int bar_idx = 0; bar_idx < 6; bar_idx++) {
        u8 bar_offset = PCI_REG_BAR0 + (bar_idx * 4);
        u32 bar_val = pci_read32(bus, slot, func, bar_offset);
        if (bar_val == 0) continue;

        dev->bar_is_io[bar_idx] = (bar_val & 0x01) != 0;
        dev->bar[bar_idx] = dev->bar_is_io[bar_idx] ? (bar_val & ~0x03) : (bar_val & ~0x0F);

        /* Measure BAR size by writing all 1s */
        pci_write32(bus, slot, func, bar_offset, 0xFFFFFFFF);
        u32 size_mask = pci_read32(bus, slot, func, bar_offset);
        pci_write32(bus, slot, func, bar_offset, bar_val); /* Restore */

        if (dev->bar_is_io[bar_idx]) {
            size_mask &= ~0x03;
        } else {
            size_mask &= ~0x0F;
        }
        dev->bar_size[bar_idx] = (~size_mask) + 1;
    }

    kprintf("[PCI] %02x:%02x.%d - Vendor: %04x Device: %04x Class: %02x.%02x (BAR0: 0x%08x, IRQ: %d)\n",
            bus, slot, func, vendor, device, dev->class_code, dev->subclass, dev->bar[0], dev->irq_line);
}

void pci_init(void) {
    kprintf("[PCI] Enumerating PCI configuration space...\n");
    g_pci_device_count = 0;

    for (u16 bus = 0; bus < 256; bus++) {
        for (u8 slot = 0; slot < 32; slot++) {
            u16 vendor = pci_read16((u8)bus, slot, 0, PCI_REG_VENDOR_ID);
            if (vendor == 0xFFFF || vendor == 0x0000) continue;

            pci_probe_device((u8)bus, slot, 0);

            /* Check header type bit 7 (multi-function device) */
            u8 header_type = pci_read8((u8)bus, slot, 0, PCI_REG_HEADER_TYPE);
            if (header_type & 0x80) {
                for (u8 func = 1; func < 8; func++) {
                    pci_probe_device((u8)bus, slot, func);
                }
            }
        }
    }
    kprintf("[PCI] Discovered %d PCI devices.\n", g_pci_device_count);
}

int pci_get_device_count(void) {
    return g_pci_device_count;
}

const pci_device_t *pci_get_device(int idx) {
    if (idx < 0 || idx >= g_pci_device_count) return NULL;
    return &g_pci_devices[idx];
}

pci_device_t *pci_find_device(u16 vendor_id, u16 device_id) {
    for (int i = 0; i < g_pci_device_count; i++) {
        if (g_pci_devices[i].vendor_id == vendor_id &&
            g_pci_devices[i].device_id == device_id) {
            return &g_pci_devices[i];
        }
    }
    return NULL;
}

pci_device_t *pci_find_class(u8 class_code, u8 subclass) {
    for (int i = 0; i < g_pci_device_count; i++) {
        if (g_pci_devices[i].class_code == class_code &&
            g_pci_devices[i].subclass == subclass) {
            return &g_pci_devices[i];
        }
    }
    return NULL;
}

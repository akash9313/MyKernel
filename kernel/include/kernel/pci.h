#ifndef ASTRA_KERNEL_PCI_H
#define ASTRA_KERNEL_PCI_H

#include <kernel/types.h>

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

/* Common PCI Register Offsets */
#define PCI_REG_VENDOR_ID     0x00
#define PCI_REG_DEVICE_ID     0x02
#define PCI_REG_COMMAND       0x04
#define PCI_REG_STATUS        0x06
#define PCI_REG_REVISION      0x08
#define PCI_REG_PROG_IF       0x09
#define PCI_REG_SUBCLASS      0x0A
#define PCI_REG_CLASS         0x0B
#define PCI_REG_HEADER_TYPE   0x0E
#define PCI_REG_BAR0          0x10
#define PCI_REG_BAR1          0x14
#define PCI_REG_BAR2          0x18
#define PCI_REG_BAR3          0x1C
#define PCI_REG_BAR4          0x20
#define PCI_REG_BAR5          0x24
#define PCI_REG_INTERRUPT_LINE 0x3C
#define PCI_REG_INTERRUPT_PIN  0x3D

/* Command Register Bits */
#define PCI_CMD_IO_SPACE      (1 << 0)
#define PCI_CMD_MEM_SPACE     (1 << 1)
#define PCI_CMD_BUS_MASTER    (1 << 2)

/* Maximum PCI devices tracked */
#define PCI_MAX_DEVICES 32

typedef struct pci_device {
    u8 bus;
    u8 slot;
    u8 func;
    u16 vendor_id;
    u16 device_id;
    u8 class_code;
    u8 subclass;
    u8 prog_if;
    u8 revision;
    u32 bar[6];
    u32 bar_size[6];
    bool bar_is_io[6];
    u8 irq_line;
    u8 irq_pin;
} pci_device_t;

/* PCI Configuration Access */
u32 pci_read32(u8 bus, u8 slot, u8 func, u8 offset);
u16 pci_read16(u8 bus, u8 slot, u8 func, u8 offset);
u8  pci_read8(u8 bus, u8 slot, u8 func, u8 offset);
void pci_write32(u8 bus, u8 slot, u8 func, u8 offset, u32 val);
void pci_write16(u8 bus, u8 slot, u8 func, u8 offset, u16 val);
void pci_write8(u8 bus, u8 slot, u8 func, u8 offset, u8 val);

void pci_enable_bus_master(const pci_device_t *dev);
void pci_enable_mem_space(const pci_device_t *dev);

/* PCI Enumeration & Discovery */
void pci_init(void);
int pci_get_device_count(void);
const pci_device_t *pci_get_device(int idx);
pci_device_t *pci_find_device(u16 vendor_id, u16 device_id);
pci_device_t *pci_find_class(u8 class_code, u8 subclass);

#endif /* ASTRA_KERNEL_PCI_H */

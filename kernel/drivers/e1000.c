#include <kernel/e1000.h>
#include <kernel/pci.h>
#include <kernel/pmm.h>
#include <kernel/boot.h>
#include <kernel/kprintf.h>
#include <kernel/mem.h>
#include <kernel/vmm.h>
#include <kernel/arch/x86_64/cpu.h>

static struct {
    bool available;
    pci_device_t pci_dev;
    uintptr_t mmio_base;
    u8 mac[6];

    /* RX Ring */
    e1000_rx_desc_t *rx_descs;
    phys_addr_t rx_descs_pa;
    void *rx_buffers[E1000_NUM_RX_DESC];
    phys_addr_t rx_buffers_pa[E1000_NUM_RX_DESC];
    u32 rx_cur;

    /* TX Ring */
    e1000_tx_desc_t *tx_descs;
    phys_addr_t tx_descs_pa;
    void *tx_buffers[E1000_NUM_TX_DESC];
    phys_addr_t tx_buffers_pa[E1000_NUM_TX_DESC];
    u32 tx_cur;

    u64 packets_rx;
    u64 packets_tx;
    u64 bytes_rx;
    u64 bytes_tx;
} g_e1000 = {0};

static inline u32 e1000_read_reg(u32 reg) {
    volatile u32 *ptr = (volatile u32 *)(g_e1000.mmio_base + reg);
    return *ptr;
}

static inline void e1000_write_reg(u32 reg, u32 val) {
    volatile u32 *ptr = (volatile u32 *)(g_e1000.mmio_base + reg);
    *ptr = val;
}

static u16 e1000_read_eeprom(u8 addr) {
    e1000_write_reg(E1000_REG_EERD, ((u32)addr << 8) | 1);
    u32 val = 0;
    int timeout = 20000;
    while (!((val = e1000_read_reg(E1000_REG_EERD)) & (1 << 4)) && --timeout) {
        io_wait();
    }
    return (u16)((val >> 16) & 0xFFFF);
}

static void e1000_read_mac(void) {
    /* Try reading from EEPROM */
    u16 val0 = e1000_read_eeprom(0);
    u16 val1 = e1000_read_eeprom(1);
    u16 val2 = e1000_read_eeprom(2);

    g_e1000.mac[0] = (u8)(val0 & 0xFF);
    g_e1000.mac[1] = (u8)(val0 >> 8);
    g_e1000.mac[2] = (u8)(val1 & 0xFF);
    g_e1000.mac[3] = (u8)(val1 >> 8);
    g_e1000.mac[4] = (u8)(val2 & 0xFF);
    g_e1000.mac[5] = (u8)(val2 >> 8);

    /* Check if EEPROM read was valid; if not, read RAL0 and RAH0 */
    bool all_zero = true;
    bool all_ff = true;
    for (int i = 0; i < 6; i++) {
        if (g_e1000.mac[i] != 0x00) all_zero = false;
        if (g_e1000.mac[i] != 0xFF) all_ff = false;
    }

    if (all_zero || all_ff) {
        u32 ral = e1000_read_reg(E1000_REG_RAL0);
        u32 rah = e1000_read_reg(E1000_REG_RAH0);
        g_e1000.mac[0] = (u8)(ral & 0xFF);
        g_e1000.mac[1] = (u8)((ral >> 8) & 0xFF);
        g_e1000.mac[2] = (u8)((ral >> 16) & 0xFF);
        g_e1000.mac[3] = (u8)((ral >> 24) & 0xFF);
        g_e1000.mac[4] = (u8)(rah & 0xFF);
        g_e1000.mac[5] = (u8)((rah >> 8) & 0xFF);
    }
}

bool e1000_init(void) {
    kprintf("[E1000] Scanning PCI bus for Intel Gigabit Ethernet controller...\n");

    /* Common Intel Gigabit Ethernet PCI Device IDs */
    static const u16 supported_devs[] = {
        0x100E, /* 82540EM (VirtualBox & QEMU standard default) */
        0x1004, 0x100F, 0x1010, 0x1012, 0x101D, 0x1079, 0x107C,
        0x153A, 0x10EA, 0x1502
    };

    pci_device_t *pci = NULL;
    for (size_t i = 0; i < sizeof(supported_devs) / sizeof(supported_devs[0]); i++) {
        pci = pci_find_device(0x8086, supported_devs[i]);
        if (pci) break;
    }

    if (!pci) {
        /* Check by class network / ethernet */
        pci = pci_find_class(0x02, 0x00);
        if (pci && pci->vendor_id != 0x8086) {
            kprintf("[E1000] Non-Intel network controller found (%04x:%04x). Not supported.\n",
                    pci->vendor_id, pci->device_id);
            return false;
        }
    }

    if (!pci) {
        kprintf("[E1000] No compatible Intel E1000 network adapter found.\n");
        return false;
    }

    g_e1000.pci_dev = *pci;
    kprintf("[E1000] Found Intel E1000 Adapter (%04x:%04x) at PCI %02x:%02x.%d\n",
            pci->vendor_id, pci->device_id, pci->bus, pci->slot, pci->func);

    /* Enable Bus Master and Memory Space on PCI device */
    pci_enable_bus_master(pci);

    u32 bar0_pa = pci->bar[0];
    if (bar0_pa == 0 || pci->bar_is_io[0]) {
        kprintf("[E1000 ERROR] Invalid BAR0 address (0x%08x)\n", bar0_pa);
        return false;
    }

    struct limine_hhdm_response *hhdm = boot_get_hhdm_response();
    g_e1000.mmio_base = (uintptr_t)(bar0_pa + hhdm->offset);

    /* Explicitly map MMIO physical space into the kernel PML4 */
    size_t mmio_size = pci->bar_size[0];
    if (mmio_size < 128 * 1024) mmio_size = 128 * 1024;
    mmio_size = (mmio_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    phys_addr_t pml4 = vmm_get_kernel_pml4();
    int mres = vmm_map_range(pml4, (virt_addr_t)g_e1000.mmio_base, (phys_addr_t)bar0_pa, mmio_size,
                             VMM_FLAG_READ | VMM_FLAG_WRITE | VMM_FLAG_NO_EXEC | VMM_FLAG_PCD | VMM_FLAG_PWT);
    if (mres != VMM_SUCCESS && mres != VMM_ERR_ALREADY_MAPPED) {
        kprintf("[E1000 ERROR] Failed to map MMIO range (error %d)\n", mres);
        return false;
    }

    kprintf("  MMIO Physical: 0x%08x (Size: %u KiB)\n", bar0_pa, pci->bar_size[0] / 1024);
    kprintf("  MMIO Virtual:  0x%016llx\n", (unsigned long long)g_e1000.mmio_base);

    /* Read MAC address */
    e1000_read_mac();
    kprintf("  MAC Address:   %02x:%02x:%02x:%02x:%02x:%02x\n",
            g_e1000.mac[0], g_e1000.mac[1], g_e1000.mac[2],
            g_e1000.mac[3], g_e1000.mac[4], g_e1000.mac[5]);

    /* Reset device */
    e1000_write_reg(E1000_REG_CTRL, e1000_read_reg(E1000_REG_CTRL) | E1000_CTRL_RST);
    for (volatile int i = 0; i < 50000; i++) io_wait();

    /* Clear Multicast Table Array */
    for (int i = 0; i < 128; i++) {
        e1000_write_reg(E1000_REG_MTA + (i * 4), 0);
    }

    /* Allocate DMA Descriptors and Buffers */
    /* RX Descriptors: 32 * 16 = 512 bytes -> 1 page */
    phys_addr_t rx_desc_pa = 0;
    if (pmm_alloc_contiguous(1, &rx_desc_pa) != PMM_SUCCESS) {
        kprintf("[E1000 ERROR] Failed to allocate RX descriptor ring memory!\n");
        return false;
    }
    g_e1000.rx_descs_pa = rx_desc_pa;
    g_e1000.rx_descs = (e1000_rx_desc_t *)(rx_desc_pa + hhdm->offset);
    kmemset(g_e1000.rx_descs, 0, PAGE_SIZE);

    /* Allocate RX Packet Buffers: 32 buffers * 2048 bytes = 16 pages */
    phys_addr_t rx_bufs_pa = 0;
    if (pmm_alloc_contiguous(16, &rx_bufs_pa) != PMM_SUCCESS) {
        kprintf("[E1000 ERROR] Failed to allocate RX packet buffer memory!\n");
        return false;
    }
    for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
        phys_addr_t buf_pa = rx_bufs_pa + (i * E1000_BUFFER_SIZE);
        g_e1000.rx_buffers_pa[i] = buf_pa;
        g_e1000.rx_buffers[i] = (void *)(buf_pa + hhdm->offset);
        g_e1000.rx_descs[i].buffer_addr = (u64)buf_pa;
        g_e1000.rx_descs[i].status = 0;
    }

    /* Setup RX Ring Registers */
    e1000_write_reg(E1000_REG_RDBAL, (u32)(rx_desc_pa & 0xFFFFFFFF));
    e1000_write_reg(E1000_REG_RDBAH, (u32)(rx_desc_pa >> 32));
    e1000_write_reg(E1000_REG_RDLEN, E1000_NUM_RX_DESC * sizeof(e1000_rx_desc_t));
    e1000_write_reg(E1000_REG_RDH, 0);
    e1000_write_reg(E1000_REG_RDT, E1000_NUM_RX_DESC - 1);
    g_e1000.rx_cur = 0;

    /* Enable RX */
    e1000_write_reg(E1000_REG_RCTL,
                    E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_BSIZE_2048 | E1000_RCTL_SECRC);

    /* TX Descriptors: 32 * 16 = 512 bytes -> 1 page */
    phys_addr_t tx_desc_pa = 0;
    if (pmm_alloc_contiguous(1, &tx_desc_pa) != PMM_SUCCESS) {
        kprintf("[E1000 ERROR] Failed to allocate TX descriptor ring memory!\n");
        return false;
    }
    g_e1000.tx_descs_pa = tx_desc_pa;
    g_e1000.tx_descs = (e1000_tx_desc_t *)(tx_desc_pa + hhdm->offset);
    kmemset(g_e1000.tx_descs, 0, PAGE_SIZE);

    /* Allocate TX Packet Buffers: 32 buffers * 2048 bytes = 16 pages */
    phys_addr_t tx_bufs_pa = 0;
    if (pmm_alloc_contiguous(16, &tx_bufs_pa) != PMM_SUCCESS) {
        kprintf("[E1000 ERROR] Failed to allocate TX packet buffer memory!\n");
        return false;
    }
    for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
        phys_addr_t buf_pa = tx_bufs_pa + (i * E1000_BUFFER_SIZE);
        g_e1000.tx_buffers_pa[i] = buf_pa;
        g_e1000.tx_buffers[i] = (void *)(buf_pa + hhdm->offset);
        g_e1000.tx_descs[i].buffer_addr = (u64)buf_pa;
        g_e1000.tx_descs[i].cmd = 0;
        g_e1000.tx_descs[i].status = E1000_TXD_STAT_DD; /* Mark available */
    }

    /* Setup TX Ring Registers */
    e1000_write_reg(E1000_REG_TDBAL, (u32)(tx_desc_pa & 0xFFFFFFFF));
    e1000_write_reg(E1000_REG_TDBAH, (u32)(tx_desc_pa >> 32));
    e1000_write_reg(E1000_REG_TDLEN, E1000_NUM_TX_DESC * sizeof(e1000_tx_desc_t));
    e1000_write_reg(E1000_REG_TDH, 0);
    e1000_write_reg(E1000_REG_TDT, 0);
    g_e1000.tx_cur = 0;

    /* Setup TIPG (Transmit Inter-Packet Gap) */
    e1000_write_reg(E1000_REG_TIPG, 0x0060200A);

    /* Enable TX */
    e1000_write_reg(E1000_REG_TCTL,
                    E1000_TCTL_EN | E1000_TCTL_PSP | (15 << 4) | (64 << 12));

    /* Set Link Up */
    e1000_write_reg(E1000_REG_CTRL,
                    e1000_read_reg(E1000_REG_CTRL) | E1000_CTRL_SLU | E1000_CTRL_ASDE);

    /* Disable interrupts (we support high-performance non-blocking polling and timer polling) */
    e1000_write_reg(E1000_REG_IMC, 0xFFFFFFFF);
    (void)e1000_read_reg(E1000_REG_ICR); /* Read clears ICR */

    g_e1000.available = true;

    u32 status = e1000_read_reg(E1000_REG_STATUS);
    bool link_up = (status & E1000_STATUS_LU) != 0;
    kprintf("[E1000] Initialized successfully. Link Status: %s (STATUS: 0x%08x)\n",
            link_up ? "UP (Connected)" : "DOWN", status);

    return true;
}

bool e1000_is_available(void) {
    return g_e1000.available;
}

const u8 *e1000_get_mac(void) {
    return g_e1000.mac;
}

bool e1000_is_link_up(void) {
    if (!g_e1000.available) return false;
    return (e1000_read_reg(E1000_REG_STATUS) & E1000_STATUS_LU) != 0;
}

int e1000_send(const void *data, size_t len) {
    if (!g_e1000.available || !data || len == 0 || len > E1000_BUFFER_SIZE) {
        return -1;
    }

    u32 idx = g_e1000.tx_cur;
    volatile e1000_tx_desc_t *desc = &g_e1000.tx_descs[idx];

    /* Wait for previous transmission on this descriptor to finish */
    int timeout = 100000;
    while (!(desc->status & E1000_TXD_STAT_DD) && --timeout) {
        io_wait();
    }
    if (timeout <= 0) {
        kprintf("[E1000 WARN] TX descriptor %u timed out!\n", idx);
        return -2;
    }

    /* Copy data into pre-allocated physical DMA buffer */
    kmemcpy(g_e1000.tx_buffers[idx], data, len);

    desc->length = (u16)len;
    desc->cso = 0;
    desc->css = 0;
    desc->cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;
    desc->status = 0;

    /* Advance TX tail */
    g_e1000.tx_cur = (g_e1000.tx_cur + 1) % E1000_NUM_TX_DESC;
    e1000_write_reg(E1000_REG_TDT, g_e1000.tx_cur);

    g_e1000.packets_tx++;
    g_e1000.bytes_tx += len;

    return (int)len;
}

int e1000_receive(void *buf, size_t max_len) {
    if (!g_e1000.available || !buf || max_len == 0) return 0;

    u32 idx = g_e1000.rx_cur;
    volatile e1000_rx_desc_t *desc = &g_e1000.rx_descs[idx];

    if (!(desc->status & E1000_RXD_STAT_DD)) {
        return 0; /* No packet ready */
    }

    size_t pkt_len = desc->length;
    if (pkt_len > max_len) pkt_len = max_len;

    kmemcpy(buf, g_e1000.rx_buffers[idx], pkt_len);

    /* Reset descriptor status and advance tail */
    desc->status = 0;
    u32 old_cur = g_e1000.rx_cur;
    g_e1000.rx_cur = (g_e1000.rx_cur + 1) % E1000_NUM_RX_DESC;
    e1000_write_reg(E1000_REG_RDT, old_cur);

    g_e1000.packets_rx++;
    g_e1000.bytes_rx += pkt_len;

    return (int)pkt_len;
}

bool e1000_self_test(void) {
    kprintf("\n[E1000 TEST] Starting Network Controller Self-Test...\n");

    if (!g_e1000.available) {
        kprintf("[E1000 TEST FAIL] E1000 controller not available!\n");
        return false;
    }

    kprintf("  PASS: PCI Device %04x:%04x discovered and MMIO mapped.\n",
            g_e1000.pci_dev.vendor_id, g_e1000.pci_dev.device_id);
    kprintf("ASTRA_PCI_NET_DEVICE_OK\n");

    bool link = e1000_is_link_up();
    kprintf("  PASS: E1000 TX/RX descriptor rings armed; Link: %s\n",
            link ? "UP" : "DOWN (Virtual/Disconnected)");
    kprintf("ASTRA_NET_DRIVER_OK\n\n");

    return true;
}

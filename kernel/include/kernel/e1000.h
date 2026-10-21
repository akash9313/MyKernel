#ifndef ASTRA_KERNEL_E1000_H
#define ASTRA_KERNEL_E1000_H

#include <kernel/types.h>
#include <kernel/pci.h>

#define E1000_NUM_RX_DESC 32
#define E1000_NUM_TX_DESC 32
#define E1000_BUFFER_SIZE 2048

/* E1000 Registers */
#define E1000_REG_CTRL     0x0000
#define E1000_REG_STATUS   0x0008
#define E1000_REG_EERD     0x0014
#define E1000_REG_ICR      0x00C0
#define E1000_REG_ITR      0x00C4
#define E1000_REG_ICS      0x00C8
#define E1000_REG_IMS      0x00D0
#define E1000_REG_IMC      0x00D8
#define E1000_REG_RCTL     0x0100
#define E1000_REG_TCTL     0x0400
#define E1000_REG_TIPG     0x0410
#define E1000_REG_RDBAL    0x2800
#define E1000_REG_RDBAH    0x2804
#define E1000_REG_RDLEN    0x2808
#define E1000_REG_RDH      0x2810
#define E1000_REG_RDT      0x2818
#define E1000_REG_TDBAL    0x3800
#define E1000_REG_TDBAH    0x3804
#define E1000_REG_TDLEN    0x3808
#define E1000_REG_TDH      0x3810
#define E1000_REG_TDT      0x3818
#define E1000_REG_MTA      0x5200
#define E1000_REG_RAL0     0x5400
#define E1000_REG_RAH0     0x5404

/* Control Register Bits */
#define E1000_CTRL_FD      (1 << 0)
#define E1000_CTRL_ASDE    (1 << 5)
#define E1000_CTRL_SLU     (1 << 6)
#define E1000_CTRL_RST     (1 << 26)

/* Status Register Bits */
#define E1000_STATUS_FD    (1 << 0)
#define E1000_STATUS_LU    (1 << 1)

/* Receive Control Bits */
#define E1000_RCTL_EN      (1 << 1)
#define E1000_RCTL_SBP     (1 << 2)
#define E1000_RCTL_UPE     (1 << 3)
#define E1000_RCTL_MPE     (1 << 4)
#define E1000_RCTL_BAM     (1 << 15)
#define E1000_RCTL_BSIZE_2048 (0 << 16)
#define E1000_RCTL_SECRC   (1 << 26)

/* Transmit Control Bits */
#define E1000_TCTL_EN      (1 << 1)
#define E1000_TCTL_PSP     (1 << 3)

/* RX / TX Descriptor Status and Command Bits */
#define E1000_RXD_STAT_DD  (1 << 0)
#define E1000_RXD_STAT_EOP (1 << 1)

#define E1000_TXD_CMD_EOP  (1 << 0)
#define E1000_TXD_CMD_IFCS (1 << 1)
#define E1000_TXD_CMD_RS   (1 << 3)
#define E1000_TXD_STAT_DD  (1 << 0)

/* E1000 Descriptors */
typedef struct e1000_rx_desc {
    u64 buffer_addr;
    u16 length;
    u16 checksum;
    u8 status;
    u8 errors;
    u16 special;
} __attribute__((packed)) e1000_rx_desc_t;

typedef struct e1000_tx_desc {
    u64 buffer_addr;
    u16 length;
    u8 cso;
    u8 cmd;
    u8 status;
    u8 css;
    u16 special;
} __attribute__((packed)) e1000_tx_desc_t;

/* Driver API */
bool e1000_init(void);
bool e1000_is_available(void);
const u8 *e1000_get_mac(void);
bool e1000_is_link_up(void);

int e1000_send(const void *data, size_t len);
int e1000_receive(void *buf, size_t max_len);

/* Self-test */
bool e1000_self_test(void);

#endif /* ASTRA_KERNEL_E1000_H */

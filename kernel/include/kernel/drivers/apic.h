#ifndef ASTRA_DRIVERS_APIC_H
#define ASTRA_DRIVERS_APIC_H

#include <kernel/types.h>

#define APIC_TIMER_VECTOR        32
#define APIC_IPI_GENERIC_VECTOR  250
#define APIC_TLB_IPI_VECTOR      251
#define APIC_SPURIOUS_VECTOR     255

void apic_init(void);
void apic_eoi(void);
void apic_timer_handler(void);
u64 apic_get_ticks(void);
void apic_send_ipi(u32 lapic_id, u8 vector);
void apic_send_ipi_all_ex_self(u8 vector);

#endif /* ASTRA_DRIVERS_APIC_H */

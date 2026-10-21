#ifndef ASTRA_DRIVERS_APIC_H
#define ASTRA_DRIVERS_APIC_H

#include <kernel/types.h>

#define APIC_TIMER_VECTOR    32
#define APIC_SPURIOUS_VECTOR 255

void apic_init(void);
void apic_eoi(void);
void apic_timer_handler(void);
u64 apic_get_ticks(void);

#endif /* ASTRA_DRIVERS_APIC_H */

#ifndef ASTRA_KERNEL_RTC_H
#define ASTRA_KERNEL_RTC_H

#include <kernel/types.h>

typedef struct {
    u16 year;   /* 4-digit year, e.g. 2026 */
    u8  month;  /* 1-12 */
    u8  day;    /* 1-31 */
    u8  hour;   /* 0-23 */
    u8  minute; /* 0-59 */
    u8  second; /* 0-59 */
} rtc_time_t;

void rtc_init(void);
bool rtc_get_time(rtc_time_t *out);
u64  rtc_get_unix_time(void);

#endif /* ASTRA_KERNEL_RTC_H */

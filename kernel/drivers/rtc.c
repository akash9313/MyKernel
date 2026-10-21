#include <kernel/rtc.h>
#include <kernel/arch/x86_64/cpu.h>
#include <kernel/kprintf.h>

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static u8 cmos_read(u8 reg) {
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static bool is_updating(void) {
    outb(CMOS_ADDR, 0x0A);
    return (inb(CMOS_DATA) & 0x80) != 0;
}

static u8 bcd_to_bin(u8 val) {
    return (val & 0x0F) + ((val >> 4) * 10);
}

void rtc_init(void) {
    rtc_time_t t;
    if (rtc_get_time(&t)) {
        kprintf("[RTC] System RTC clock initialized: %04u-%02u-%02u %02u:%02u:%02u UTC\n",
                (unsigned)t.year, (unsigned)t.month, (unsigned)t.day,
                (unsigned)t.hour, (unsigned)t.minute, (unsigned)t.second);
    } else {
        kprintf("[RTC WARN] CMOS RTC clock read failed.\n");
    }
}

bool rtc_get_time(rtc_time_t *out) {
    if (!out) return false;

    /* Wait for any RTC update in progress to complete (timeout at 100000 spins) */
    int timeout = 100000;
    while (is_updating() && --timeout > 0);
    if (timeout <= 0) return false;

    u8 sec   = cmos_read(0x00);
    u8 min   = cmos_read(0x02);
    u8 hour  = cmos_read(0x04);
    u8 day   = cmos_read(0x07);
    u8 month = cmos_read(0x08);
    u8 year  = cmos_read(0x09);
    u8 century = cmos_read(0x32);
    u8 reg_b = cmos_read(0x0B);

    /* Read again to ensure consistency */
    u8 sec2 = cmos_read(0x00);
    if (sec != sec2) {
        timeout = 100000;
        while (is_updating() && --timeout > 0);
        sec   = cmos_read(0x00);
        min   = cmos_read(0x02);
        hour  = cmos_read(0x04);
        day   = cmos_read(0x07);
        month = cmos_read(0x08);
        year  = cmos_read(0x09);
        century = cmos_read(0x32);
        reg_b = cmos_read(0x0B);
    }

    bool bcd = ((reg_b & 0x04) == 0);
    bool is_24h = ((reg_b & 0x02) != 0);

    if (bcd) {
        sec   = bcd_to_bin(sec);
        min   = bcd_to_bin(min);
        hour  = ((hour & 0x80) ? 0x80 : 0) | bcd_to_bin(hour & 0x7F);
        day   = bcd_to_bin(day);
        month = bcd_to_bin(month);
        year  = bcd_to_bin(year);
        century = bcd_to_bin(century);
    }

    if (!is_24h) {
        bool pm = (hour & 0x80) != 0;
        hour &= 0x7F;
        if (hour == 12) hour = 0;
        if (pm) hour += 12;
    }

    u16 full_year;
    if (century >= 19 && century <= 21) {
        full_year = (u16)century * 100 + year;
    } else {
        full_year = (year >= 70) ? (1900 + year) : (2000 + year);
    }

    out->second = sec;
    out->minute = min;
    out->hour   = hour;
    out->day    = day;
    out->month  = month;
    out->year   = full_year;
    return true;
}

static bool is_leap_year(u16 y) {
    return (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
}

u64 rtc_get_unix_time(void) {
    rtc_time_t t;
    if (!rtc_get_time(&t)) {
        return 0;
    }

    if (t.year < 1970 || t.month < 1 || t.month > 12 || t.day < 1 || t.day > 31) {
        return 0;
    }

    static const u16 days_before_month[12] = {
        0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
    };

    u64 days = 365ULL * (t.year - 1970);
    /* Add leap days between 1970 and year */
    for (u16 y = 1970; y < t.year; y++) {
        if (is_leap_year(y)) days++;
    }

    days += days_before_month[t.month - 1];
    if (t.month > 2 && is_leap_year(t.year)) {
        days++;
    }
    days += (t.day - 1);

    return (days * 86400ULL) + ((u64)t.hour * 3600ULL) + ((u64)t.minute * 60ULL) + (u64)t.second;
}

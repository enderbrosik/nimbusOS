/*
 * NimbusOS - CMOS real time clock (Motorola MC146818).
 * OSDev wiki "CMOS#Getting Current Date and Time from RTC": wait for the
 * "update in progress" flag (status A bit 7) to clear, read twice until two
 * consecutive reads agree, convert from BCD / 12-hour mode per status B.
 */
#include "drivers/rtc.h"
#include "arch/x86/io.h"

static u8 cmos(u8 reg) { outb(0x70, reg); return inb(0x71); }
static bool updating(void) { return cmos(0x0A) & 0x80; }

static void raw_read(struct rtc_time *t)
{
    while (updating()) {}
    t->second = cmos(0x00);
    t->minute = cmos(0x02);
    t->hour = cmos(0x04);
    t->day = cmos(0x07);
    t->month = cmos(0x08);
    t->year = cmos(0x09);
}

void rtc_read(struct rtc_time *t)
{
    struct rtc_time a, b;
    u32 f = irq_save();
    raw_read(&b);
    do { a = b; raw_read(&b); }
    while (a.second != b.second || a.minute != b.minute || a.hour != b.hour || a.day != b.day);
    u8 regb = cmos(0x0B);
    irq_restore(f);
    if (!(regb & 0x04)) {   /* BCD */
#define BCD(x) (((x) & 0x0F) + (((x) >> 4) & 0x0F) * 10)
        b.second = BCD(b.second);
        b.minute = BCD(b.minute);
        b.hour = BCD(b.hour & 0x7F) | (b.hour & 0x80);
        b.day = BCD(b.day);
        b.month = BCD(b.month);
        b.year = BCD(b.year);
    }
    if (!(regb & 0x02) && (b.hour & 0x80)) b.hour = ((b.hour & 0x7F) + 12) % 24;
    b.year += 2000;
    *t = b;
}

static const int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
static bool leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

u32 rtc_unix_time(void)
{
    struct rtc_time t;
    rtc_read(&t);
    u32 days = 0;
    for (int y = 1970; y < t.year; y++) days += leap(y) ? 366 : 365;
    for (int m = 1; m < t.month; m++) days += mdays[m - 1] + (m == 2 && leap(t.year));
    days += t.day - 1;
    return days * 86400 + t.hour * 3600 + t.minute * 60 + t.second;
}

void unix_to_rtc(u32 ts, struct rtc_time *o)
{
    u32 days = ts / 86400, rem = ts % 86400;
    o->hour = rem / 3600; o->minute = (rem % 3600) / 60; o->second = rem % 60;
    int y = 1970;
    while (days >= (u32)(leap(y) ? 366 : 365)) { days -= leap(y) ? 366 : 365; y++; }
    int m = 0;
    while (days >= (u32)(mdays[m] + (m == 1 && leap(y)))) { days -= mdays[m] + (m == 1 && leap(y)); m++; }
    o->year = y; o->month = m + 1; o->day = days + 1;
}

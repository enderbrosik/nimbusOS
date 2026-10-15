#ifndef NIMBUS_RTC_H
#define NIMBUS_RTC_H
#include "kernel/types.h"
struct rtc_time { int year, month, day, hour, minute, second; };
void rtc_read(struct rtc_time *t);
u32  rtc_unix_time(void);                          /* seconds since 1970 (UTC) */
void unix_to_rtc(u32 t, struct rtc_time *out);
#endif

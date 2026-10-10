// Real-time clock backed by the browser's local time.

#include <time.h>
#include "port_internal.h"
#include "siirtc.h"

static u8 ToBcd(u32 value)
{
    return ((value / 10) << 4) | (value % 10);
}

void SiiRtcUnprotect(void)
{
}

void SiiRtcProtect(void)
{
}

u8 SiiRtcProbe(void)
{
    // Low nibble 1: RTC present; high nibble 0: no power failure.
    return 1;
}

bool8 SiiRtcReset(void)
{
    return TRUE;
}

bool8 SiiRtcGetStatus(struct SiiRtcInfo *rtc)
{
    rtc->status = SIIRTCINFO_24HOUR;
    return TRUE;
}

bool8 SiiRtcSetStatus(struct SiiRtcInfo *rtc)
{
    return TRUE;
}

bool8 SiiRtcGetDateTime(struct SiiRtcInfo *rtc)
{
    time_t now = time(NULL);
    struct tm *local = localtime(&now);
    // The RTC only covers 2000-2099.
    int year = local->tm_year + 1900;
    if (year < 2000)
        year = 2000;

    rtc->year = ToBcd(year % 100);
    rtc->month = ToBcd(local->tm_mon + 1);
    rtc->day = ToBcd(local->tm_mday);
    rtc->dayOfWeek = ToBcd(local->tm_wday);
    rtc->hour = ToBcd(local->tm_hour);
    rtc->minute = ToBcd(local->tm_min);
    rtc->second = ToBcd(local->tm_sec);
    return TRUE;
}

bool8 SiiRtcSetDateTime(struct SiiRtcInfo *rtc)
{
    return TRUE;
}

bool8 SiiRtcGetTime(struct SiiRtcInfo *rtc)
{
    struct SiiRtcInfo full;
    SiiRtcGetDateTime(&full);
    rtc->hour = full.hour;
    rtc->minute = full.minute;
    rtc->second = full.second;
    return TRUE;
}

bool8 SiiRtcSetTime(struct SiiRtcInfo *rtc)
{
    return TRUE;
}

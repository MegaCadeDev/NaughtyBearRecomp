/*
 * Two libc functions tools/radv-link.sh binds to ps5_ versions that the pinned payload
 * SDK's platform library doesn't have yet, and three the SDK's libc lacks that DXC's LLVM
 * support library refers to (for diagnostics only: malloc statistics, signal names and
 * stack traces).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stddef.h>
#include <stdint.h>
#include <time.h>

int ps5_ffs(int value)
{
    return __builtin_ffs(value);
}

/* struct tm in UTC to seconds since 1970 (days from the civil date, Howard Hinnant's
 * algorithm), normalising out-of-range months as timegm does. */
time_t ps5_timegm(struct tm *tm)
{
    int64_t year = (int64_t)tm->tm_year + 1900;
    int64_t month = tm->tm_mon;
    year += month / 12;
    month %= 12;
    if (month < 0)
    {
        month += 12;
        year -= 1;
    }
    int64_t m = month + 1;
    int64_t y = m <= 2 ? year - 1 : year;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + tm->tm_mday - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + doe - 719468;
    return (time_t)(days * 86400 + (int64_t)tm->tm_hour * 3600 + (int64_t)tm->tm_min * 60 + tm->tm_sec);
}

/* No program break to report: LLVM then reports no malloc usage. */
void *sbrk(intptr_t increment)
{
    (void)increment;
    return (void *)-1;
}

char *strsignal(int sig)
{
    (void)sig;
    return (char *)"Signal";
}

/* No loaded-object list: LLVM's stack traces go without symbol names. */
struct dl_phdr_info;
int dl_iterate_phdr(int (*callback)(struct dl_phdr_info *, size_t, void *), void *data)
{
    (void)callback;
    (void)data;
    return 0;
}

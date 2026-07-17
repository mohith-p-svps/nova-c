// The `datetime` module — see Chapter 32 of the NovaLang book.
//
// Date arithmetic (daysBetween/addDays/addMonths/addYears/dayOfWeek for
// arbitrary dates) is built on Howard Hinnant's well-known "days from
// civil" algorithm — a small, widely-used, public-domain technique for
// converting between a (year, month, day) triple and a day count in the
// proleptic Gregorian calendar, correct for any year (including
// negative/BC years and the far future) without relying on the C
// library's <time.h> functions, which are only guaranteed to behave
// sensibly within a much narrower, platform-dependent range.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "module_native_datetime.h"
#include "../error.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/time.h>
#endif

static long long daysFromCivil(long long y, int m, int d) {
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    long long yoe = y - era * 400;
    long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civilFromDays(long long z, int* y, int* m, int* d) {
    z += 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    long long doe = z - era * 146097;
    long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long yy  = yoe + era * 400;
    long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long long mp  = (5 * doy + 2) / 153;
    long long dd  = doy - (153 * mp + 2) / 5 + 1;
    long long mm  = mp + (mp < 10 ? 3 : -9);
    *y = (int)(yy + (mm <= 2));
    *m = (int)mm;
    *d = (int)dd;
}

static int isLeap(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static int daysInMonthOf(int month, int year) {
    static const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (month < 1 || month > 12) return 0;
    if (month == 2 && isLeap(year)) return 29;
    return days[month - 1];
}

static const char* weekdayName(int wd) {
    static const char* names[] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
    return names[wd];
}

static int64_t asPlainInt(Value v) {
    if (v.type == VAL_INT64) return v.as.i64;
    if (v.type == VAL_INT32) return v.as.i32;
    if (v.type == VAL_INT16) return v.as.i16;
    return 0;
}

static struct tm currentLocalTime(void) {
    time_t t = time(NULL);
    struct tm result;
#if defined(_WIN32)
    localtime_s(&result, &t);
#else
    localtime_r(&t, &result);
#endif
    return result;
}

static int currentMillisecond(void) {
#if defined(_WIN32)
    SYSTEMTIME st;
    GetLocalTime(&st);
    return st.wMilliseconds;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int)(tv.tv_usec / 1000);
#endif
}

static Value dt_now(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)args; (void)argCount; (void)line;
    struct tm tmNow = currentLocalTime();
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmNow);
    return makeString(buf, (int)strlen(buf));
}

static Value dt_date(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)args; (void)argCount; (void)line;
    struct tm tmNow = currentLocalTime();
    char buf[16];
    strftime(buf, sizeof(buf), "%Y-%m-%d", &tmNow);
    return makeString(buf, (int)strlen(buf));
}

static Value dt_time(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)args; (void)argCount; (void)line;
    struct tm tmNow = currentLocalTime();
    char buf[16];
    strftime(buf, sizeof(buf), "%H:%M:%S", &tmNow);
    return makeString(buf, (int)strlen(buf));
}

static Value dt_year(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)args; (void)argCount; (void)line;
    return makeInt64(currentLocalTime().tm_year + 1900);
}
static Value dt_month(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)args; (void)argCount; (void)line;
    return makeInt64(currentLocalTime().tm_mon + 1);
}
static Value dt_day(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)args; (void)argCount; (void)line;
    return makeInt64(currentLocalTime().tm_mday);
}
static Value dt_hour(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)args; (void)argCount; (void)line;
    return makeInt64(currentLocalTime().tm_hour);
}
static Value dt_minute(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)args; (void)argCount; (void)line;
    return makeInt64(currentLocalTime().tm_min);
}
static Value dt_second(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)args; (void)argCount; (void)line;
    return makeInt64(currentLocalTime().tm_sec);
}
static Value dt_millisecond(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)args; (void)argCount; (void)line;
    return makeInt64(currentMillisecond());
}

static Value dt_dayOfWeek(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)args; (void)argCount; (void)line;
    int wd = currentLocalTime().tm_wday;
    return makeString(weekdayName(wd), (int)strlen(weekdayName(wd)));
}
static Value dt_dayOfYear(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)args; (void)argCount; (void)line;
    return makeInt64(currentLocalTime().tm_yday + 1);
}
static Value dt_weekOfYear(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)args; (void)argCount; (void)line;
    int doy = currentLocalTime().tm_yday + 1;
    return makeInt64((doy - 1) / 7 + 1);
}
static Value dt_isWeekend(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)args; (void)argCount; (void)line;
    int wd = currentLocalTime().tm_wday;
    return (wd == 0 || wd == 6) ? TRUE_VAL : FALSE_VAL;
}
static Value dt_isWeekday(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)args; (void)argCount; (void)line;
    int wd = currentLocalTime().tm_wday;
    return (wd == 0 || wd == 6) ? FALSE_VAL : TRUE_VAL;
}

static Value dt_isLeapYear(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount; (void)line;
    return isLeap((int)asPlainInt(args[0])) ? TRUE_VAL : FALSE_VAL;
}

static Value dt_daysInMonth(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    int month = (int)asPlainInt(args[0]);
    int year  = (int)asPlainInt(args[1]);
    int result = daysInMonthOf(month, year);
    if (result == 0) {
        novaError(ERR_ARGUMENT, line, "datetime.daysInMonth: month must be 1-12 (got %d)", month);
        return makeNull();
    }
    return makeInt64(result);
}

static Value dt_daysBetween(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount; (void)line;
    long long d1 = daysFromCivil(asPlainInt(args[0]), (int)asPlainInt(args[1]), (int)asPlainInt(args[2]));
    long long d2 = daysFromCivil(asPlainInt(args[3]), (int)asPlainInt(args[4]), (int)asPlainInt(args[5]));
    return makeInt64(d2 - d1);
}

static Value dt_addDays(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount; (void)line;
    long long d = daysFromCivil(asPlainInt(args[0]), (int)asPlainInt(args[1]), (int)asPlainInt(args[2]));
    d += asPlainInt(args[3]);
    int y, m, dd;
    civilFromDays(d, &y, &m, &dd);
    char buf[16];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m, dd);
    return makeString(buf, (int)strlen(buf));
}

static Value dt_addMonths(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount; (void)line;
    int y = (int)asPlainInt(args[0]);
    int m = (int)asPlainInt(args[1]);
    int d = (int)asPlainInt(args[2]);
    int64_t n = asPlainInt(args[3]);

    long long totalMonths = (long long)(y) * 12 + (m - 1) + n;
    int newYear  = (int)(totalMonths >= 0 ? totalMonths / 12 : (totalMonths - 11) / 12);
    int newMonth = (int)(totalMonths - (long long)newYear * 12) + 1;

    int maxDay = daysInMonthOf(newMonth, newYear);
    int newDay = d > maxDay ? maxDay : d;

    char buf[16];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d", newYear, newMonth, newDay);
    return makeString(buf, (int)strlen(buf));
}

static Value dt_addYears(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount; (void)line;
    int y = (int)asPlainInt(args[0]);
    int m = (int)asPlainInt(args[1]);
    int d = (int)asPlainInt(args[2]);
    int64_t n = asPlainInt(args[3]);

    int newYear = (int)(y + n);
    int maxDay = daysInMonthOf(m, newYear);
    int newDay = d > maxDay ? maxDay : d;

    char buf[16];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d", newYear, m, newDay);
    return makeString(buf, (int)strlen(buf));
}

static Value dt_format(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount; (void)line;
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d/%02d/%04d",
             (int)asPlainInt(args[0]), (int)asPlainInt(args[1]), (int)asPlainInt(args[2]));
    return makeString(buf, (int)strlen(buf));
}

static Value dt_formatTime(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount; (void)line;
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
             (int)asPlainInt(args[0]), (int)asPlainInt(args[1]), (int)asPlainInt(args[2]));
    return makeString(buf, (int)strlen(buf));
}

static Value dt_formatFull(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount; (void)line;
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d/%02d/%04d %02d:%02d:%02d",
             (int)asPlainInt(args[0]), (int)asPlainInt(args[1]), (int)asPlainInt(args[2]),
             (int)asPlainInt(args[3]), (int)asPlainInt(args[4]), (int)asPlainInt(args[5]));
    return makeString(buf, (int)strlen(buf));
}

static NativeFnEntry datetimeFunctions[] = {
    {"now",           dt_now,           0},
    {"date",          dt_date,          0},
    {"time",          dt_time,          0},
    {"year",          dt_year,          0},
    {"month",         dt_month,         0},
    {"day",           dt_day,           0},
    {"hour",          dt_hour,          0},
    {"minute",        dt_minute,        0},
    {"second",        dt_second,        0},
    {"millisecond",   dt_millisecond,   0},
    {"dayOfWeek",     dt_dayOfWeek,     0},
    {"dayOfYear",     dt_dayOfYear,     0},
    {"weekOfYear",    dt_weekOfYear,    0},
    {"isWeekend",     dt_isWeekend,     0},
    {"isWeekday",     dt_isWeekday,     0},
    {"isLeapYear",    dt_isLeapYear,    1},
    {"daysInMonth",   dt_daysInMonth,   2},
    {"daysBetween",   dt_daysBetween,   6},
    {"addDays",       dt_addDays,       4},
    {"addMonths",     dt_addMonths,     4},
    {"addYears",      dt_addYears,      4},
    {"format",        dt_format,        3},
    {"formatTime",    dt_formatTime,    3},
    {"formatFull",    dt_formatFull,    6},
};

NativeModule datetimeModule = {
    "datetime",
    datetimeFunctions,
    sizeof(datetimeFunctions) / sizeof(datetimeFunctions[0])
};

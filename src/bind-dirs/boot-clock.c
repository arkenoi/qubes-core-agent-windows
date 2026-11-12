/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (c) Invisible Things Lab
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

// Boot-time DST correction for a local-time RTC (docs/ADR-clock.md).
//
// The RTC holds dom0's local time (timezone=localtime) and the loader converts it to UTC with the
// ActiveTimeBias stored at the last shutdown. When a DST transition happened while the guest was
// off, that bias is the old season's: the kernel then writes the new season's bias during init but
// leaves the clock one DST step off, and nothing later in the boot repairs it.
//
// Two signals, both read here at BootExecute and neither kept by us:
//   1. the TimeZoneInformation key was last written inside this boot - the kernel replaced the
//      stored bias during init (it writes only when the bias changes);
//   2. the zone's bias at ShutdownTime - the bias the loader used - is exactly one DST step off
//      the kernel's current bias.
// Only then is the clock moved, by the difference between the bias the loader used and the zone's
// bias for the RTC's own local reading. NtSetSystemTime makes the kernel refresh its zone and
// rewrite the RTC with the corrected local time. Any other combination changes nothing and says why.

#include "boot-clock.h"

__declspec(dllimport)
int _snwprintf(
    wchar_t *buffer,
    size_t count,
    const wchar_t *format,
    ...
    );

#define CLK_MINUTE (60LL * 10000000LL)   // 100 ns ticks
#define CLK_DAY    (1440LL * CLK_MINUTE)

#define CLK_ZONE_KEY    L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Control\\TimeZoneInformation"
#define CLK_WINDOWS_KEY L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Control\\Windows"

// SystemTimeOfDayInformation; the km headers carry neither the class nor the structure.
#define CLK_TIMEOFDAY_CLASS 3

typedef struct _CLK_TIMEOFDAY
{
    LARGE_INTEGER BootTime;       // UTC; moves with every later clock set
    LARGE_INTEGER CurrentTime;
    LARGE_INTEGER TimeZoneBias;   // the kernel's current bias, 100 ns
    ULONG TimeZoneId;
    ULONG Reserved;
    ULONGLONG BootTimeBias;
    ULONGLONG SleepTimeBias;
} CLK_TIMEOFDAY;

// The zone as the kernel reads it. Biases in minutes, UTC = local + bias. The two rules are stored
// in TIME_FIELDS order: Day is the occurrence (5 = last), Weekday 0 is Sunday, Year 0 = every year.
typedef struct _CLK_ZONE
{
    LONG Bias;
    LONG StandardBias;
    LONG DaylightBias;
    TIME_FIELDS StandardStart;    // read on the daylight clock
    TIME_FIELDS DaylightStart;    // read on the standard clock
} CLK_ZONE;

static NTSTATUS ClkOpenKey(IN const WCHAR *path, IN ACCESS_MASK access, OUT HANDLE *key)
{
    UNICODE_STRING pathU;
    OBJECT_ATTRIBUTES oa;

    RtlInitUnicodeString(&pathU, path);
    InitializeObjectAttributes(&oa, &pathU, OBJ_CASE_INSENSITIVE, NULL, NULL);
    return NtOpenKey(key, access, &oa);
}

// A value of exactly this type and size; STATUS_OBJECT_TYPE_MISMATCH for any other,
// STATUS_OBJECT_NAME_NOT_FOUND when absent.
static NTSTATUS ClkReadValue(IN HANDLE key, IN const WCHAR *name, IN ULONG type, OUT void *data, IN ULONG size)
{
    ULONGLONG buffer[8];   // KEY_VALUE_PARTIAL_INFORMATION and its data, aligned
    KEY_VALUE_PARTIAL_INFORMATION *info = (KEY_VALUE_PARTIAL_INFORMATION *)buffer;
    UNICODE_STRING nameU;
    ULONG length = 0;
    NTSTATUS status;

    RtlInitUnicodeString(&nameU, name);
    status = NtQueryValueKey(key, &nameU, KeyValuePartialInformation, info, sizeof(buffer), &length);
    if (!NT_SUCCESS(status))
        return status;
    if (info->Type != type || info->DataLength != size)
        return STATUS_OBJECT_TYPE_MISMATCH;
    RtlCopyMemory(data, info->Data, size);
    return STATUS_SUCCESS;
}

// ISO-8601 date and time with milliseconds, no zone suffix.
static const WCHAR *ClkFormatTime(IN LARGE_INTEGER instant, OUT WCHAR *buffer, IN size_t capacity)
{
    TIME_FIELDS tf;

    RtlTimeToTimeFields(&instant, &tf);
    _snwprintf(buffer, capacity, L"%04d-%02d-%02dT%02d:%02d:%02d.%03d",
               tf.Year, tf.Month, tf.Day, tf.Hour, tf.Minute, tf.Second, tf.Milliseconds);
    buffer[capacity - 1] = L'\0';
    return buffer;
}

// When a recurring rule fires in `year`, on the clock the rule is read on: the Day-th Weekday of
// Month (Day 5 = the last one) at the rule's time of day.
static BOOLEAN ClkCutover(IN CSHORT year, IN const TIME_FIELDS *rule, OUT LARGE_INTEGER *when)
{
    TIME_FIELDS tf;
    LARGE_INTEGER first;

    if (rule->Year != 0 || rule->Month < 1 || rule->Month > 12 || rule->Day < 1 || rule->Day > 5 ||
        rule->Weekday < 0 || rule->Weekday > 6)
        return FALSE;
    tf = *rule;
    tf.Year = year;
    tf.Day = 1;
    if (!RtlTimeFieldsToTime(&tf, &first))
        return FALSE;
    RtlTimeToTimeFields(&first, &tf);   // the weekday of the 1st
    when->QuadPart = first.QuadPart + ((rule->Weekday - tf.Weekday + 7) % 7 + 7 * (rule->Day - 1)) * CLK_DAY;
    RtlTimeToTimeFields(when, &tf);
    if (tf.Month != rule->Month)        // no 5th occurrence this month: the last is a week earlier
        when->QuadPart -= 7 * CLK_DAY;
    return TRUE;
}

// Whether `utc` falls in the zone's daylight period, the rules evaluated in that instant's year.
static BOOLEAN ClkIsDaylight(IN const CLK_ZONE *zone, IN LARGE_INTEGER utc, OUT BOOLEAN *daylight)
{
    LONGLONG standard = (LONGLONG)(zone->Bias + zone->StandardBias) * CLK_MINUTE;
    LONGLONG summer = (LONGLONG)(zone->Bias + zone->DaylightBias) * CLK_MINUTE;
    LARGE_INTEGER local, daylightStart, standardStart;
    TIME_FIELDS tf;

    local.QuadPart = utc.QuadPart - standard;
    RtlTimeToTimeFields(&local, &tf);
    if (!ClkCutover(tf.Year, &zone->DaylightStart, &daylightStart) ||
        !ClkCutover(tf.Year, &zone->StandardStart, &standardStart))
        return FALSE;
    daylightStart.QuadPart += standard;   // to UTC
    standardStart.QuadPart += summer;
    if (daylightStart.QuadPart < standardStart.QuadPart)   // northern: daylight inside the year
        *daylight = utc.QuadPart >= daylightStart.QuadPart && utc.QuadPart < standardStart.QuadPart;
    else                                                   // southern: daylight across the new year
        *daylight = utc.QuadPart >= daylightStart.QuadPart || utc.QuadPart < standardStart.QuadPart;
    return TRUE;
}

// The zone's bias for local wall time `local`: the candidate whose UTC falls in its own season.
// FALSE when both do (the repeated autumn hour) or neither does (the spring gap).
static BOOLEAN ClkBiasForLocal(IN const CLK_ZONE *zone, IN LARGE_INTEGER local, OUT LONG *bias)
{
    LONG candidate[2];
    BOOLEAN fits[2], daylight;
    LARGE_INTEGER utc;
    int i;

    candidate[0] = zone->Bias + zone->StandardBias;
    candidate[1] = zone->Bias + zone->DaylightBias;
    for (i = 0; i < 2; i++)
    {
        utc.QuadPart = local.QuadPart + (LONGLONG)candidate[i] * CLK_MINUTE;
        if (!ClkIsDaylight(zone, utc, &daylight))
            return FALSE;
        fits[i] = (i == 0) ? !daylight : daylight;
    }
    if (fits[0] == fits[1])
        return FALSE;
    *bias = fits[0] ? candidate[0] : candidate[1];
    return TRUE;
}

const WCHAR *BootClockCorrect(void)
{
    HANDLE zoneKey = NULL, windowsKey = NULL;
    CLK_ZONE zone;
    CLK_TIMEOFDAY tod;
    ULONGLONG keyBuffer[32];   // KEY_BASIC_INFORMATION and the key's name, aligned
    KEY_BASIC_INFORMATION *keyInfo = (KEY_BASIC_INFORMATION *)keyBuffer;
    LARGE_INTEGER shutdown, local, now, target, previous;
    LONG flag = 0, dynamicDisabled = 0, active = 0, kernelBias, loaderBias, bias = 0, step;
    BOOLEAN daylight = FALSE;
    ULONG length;
    UNICODE_STRING activeU;
    const WCHAR *name = L"";
    WCHAR time1[32], time2[32];
    const WCHAR *outcome = L"error";
    NTSTATUS status;

    RtlZeroMemory(&zone, sizeof(zone));
    RtlZeroMemory(&tod, sizeof(tod));
    shutdown.QuadPart = previous.QuadPart = 0;

    status = ClkOpenKey(CLK_ZONE_KEY, KEY_QUERY_VALUE | KEY_SET_VALUE, &zoneKey);
    if (!NT_SUCCESS(status))
    {
        NtLog(TRUE, L"[ERROR] clock: open TimeZoneInformation: %x\n", status);
        goto cleanup;
    }

    // A UTC RTC is not this correction's case, and our guests never set it (ADR-clock, lint L26).
    status = ClkReadValue(zoneKey, L"RealTimeIsUniversal", REG_DWORD, &flag, sizeof(flag));
    if (status != STATUS_OBJECT_NAME_NOT_FOUND && (!NT_SUCCESS(status) || flag != 0))
    {
        NtLog(TRUE, L"[WARN] clock: refused: RealTimeIsUniversal present (%x, %ld)\n", status, flag);
        outcome = L"refused";
        goto cleanup;
    }
    status = ClkReadValue(zoneKey, name = L"DynamicDaylightTimeDisabled", REG_DWORD, &dynamicDisabled, sizeof(LONG));
    if (status == STATUS_OBJECT_NAME_NOT_FOUND)
        status = STATUS_SUCCESS;
    if (!NT_SUCCESS(status) ||
        !NT_SUCCESS(status = ClkReadValue(zoneKey, name = L"Bias", REG_DWORD, &zone.Bias, sizeof(LONG))) ||
        !NT_SUCCESS(status = ClkReadValue(zoneKey, name = L"StandardBias", REG_DWORD, &zone.StandardBias, sizeof(LONG))) ||
        !NT_SUCCESS(status = ClkReadValue(zoneKey, name = L"DaylightBias", REG_DWORD, &zone.DaylightBias, sizeof(LONG))) ||
        !NT_SUCCESS(status = ClkReadValue(zoneKey, name = L"StandardStart", REG_BINARY, &zone.StandardStart, sizeof(TIME_FIELDS))) ||
        !NT_SUCCESS(status = ClkReadValue(zoneKey, name = L"DaylightStart", REG_BINARY, &zone.DaylightStart, sizeof(TIME_FIELDS))) ||
        !NT_SUCCESS(status = ClkReadValue(zoneKey, name = L"ActiveTimeBias", REG_DWORD, &active, sizeof(LONG))))
    {
        NtLog(TRUE, L"[ERROR] clock: read %s: %x\n", name, status);
        goto cleanup;
    }

    step = zone.DaylightBias - zone.StandardBias;
    if (dynamicDisabled != 0 || zone.StandardStart.Month == 0 || zone.DaylightStart.Month == 0 || step == 0)
    {
        NtLog(FALSE, L"[INFO] clock: no-dst: ActiveTimeBias %ld\n", active);
        outcome = L"no-dst";
        goto cleanup;
    }

    status = NtQueryKey(zoneKey, KeyBasicInformation, keyInfo, sizeof(keyBuffer), &length);
    if (!NT_SUCCESS(status))
    {
        NtLog(TRUE, L"[ERROR] clock: NtQueryKey: %x\n", status);
        goto cleanup;
    }
    status = NtQuerySystemInformation(CLK_TIMEOFDAY_CLASS, &tod, sizeof(tod), &length);
    if (!NT_SUCCESS(status))
    {
        NtLog(TRUE, L"[ERROR] clock: NtQuerySystemInformation: %x\n", status);
        goto cleanup;
    }
    kernelBias = (LONG)(tod.TimeZoneBias.QuadPart / CLK_MINUTE);

    // Signal 1: the kernel wrote the zone during init of this boot.
    if (keyInfo->LastWriteTime.QuadPart < tod.BootTime.QuadPart || keyInfo->LastWriteTime.QuadPart > tod.CurrentTime.QuadPart)
    {
        NtLog(FALSE, L"[INFO] clock: already-correct: ActiveTimeBias %ld, kernel %ld, %sZ\n",
              active, kernelBias, ClkFormatTime(tod.CurrentTime, time1, RTL_NUMBER_OF(time1)));
        outcome = L"already-correct";
        goto cleanup;
    }
    if (kernelBias != active)
    {
        NtLog(TRUE, L"[WARN] clock: refused: ActiveTimeBias %ld, kernel %ld\n", active, kernelBias);
        outcome = L"refused";
        goto cleanup;
    }

    // Signal 2: the bias the loader used - the season's at ShutdownTime - is one DST step off.
    status = ClkOpenKey(CLK_WINDOWS_KEY, KEY_QUERY_VALUE, &windowsKey);
    if (NT_SUCCESS(status))
        status = ClkReadValue(windowsKey, L"ShutdownTime", REG_BINARY, &shutdown, sizeof(shutdown));
    if (!NT_SUCCESS(status))
    {
        NtLog(TRUE, L"[WARN] clock: refused: zone rewritten at boot, ShutdownTime unreadable: %x\n", status);
        outcome = L"refused";
        goto cleanup;
    }
    if (!ClkIsDaylight(&zone, shutdown, &daylight))
    {
        NtLog(TRUE, L"[WARN] clock: refused: zone rule unusable\n");
        outcome = L"refused";
        goto cleanup;
    }
    loaderBias = zone.Bias + (daylight ? zone.DaylightBias : zone.StandardBias);
    if (loaderBias - active != step && active - loaderBias != step)
    {
        NtLog(TRUE, L"[WARN] clock: refused: zone rewritten at boot, bias at ShutdownTime %sZ was %ld, now %ld\n",
              ClkFormatTime(shutdown, time1, RTL_NUMBER_OF(time1)), loaderBias, active);
        outcome = L"refused";
        goto cleanup;
    }

    // The RTC's local reading, advanced to now; its own bias gives the true UTC.
    local.QuadPart = tod.CurrentTime.QuadPart - (LONGLONG)loaderBias * CLK_MINUTE;
    if (!ClkBiasForLocal(&zone, local, &bias) || bias == loaderBias)
    {
        NtLog(TRUE, L"[WARN] clock: refused: local %s has no single bias other than %ld\n",
              ClkFormatTime(local, time1, RTL_NUMBER_OF(time1)), loaderBias);
        outcome = L"refused";
        goto cleanup;
    }

    NtQuerySystemTime(&now);
    target.QuadPart = now.QuadPart + (LONGLONG)(bias - loaderBias) * CLK_MINUTE;
    status = NtSetSystemTime(&target, &previous);
    if (!NT_SUCCESS(status))
    {
        NtLog(TRUE, L"[ERROR] clock: NtSetSystemTime: %x\n", status);
        goto cleanup;
    }

    // Read back. The kernel refreshed its zone on the set; the registry is written only if it
    // still disagrees.
    NtQuerySystemTime(&now);
    status = NtQuerySystemInformation(CLK_TIMEOFDAY_CLASS, &tod, sizeof(tod), &length);
    if (NT_SUCCESS(status))
        status = ClkReadValue(zoneKey, L"ActiveTimeBias", REG_DWORD, &active, sizeof(LONG));
    if (!NT_SUCCESS(status))
    {
        NtLog(TRUE, L"[ERROR] clock: read back after the move: %x\n", status);
        goto cleanup;
    }
    kernelBias = (LONG)(tod.TimeZoneBias.QuadPart / CLK_MINUTE);
    NtLog(FALSE, L"[INFO] clock: corrected: bias %ld -> %ld, %sZ -> %sZ (%I64d s)\n", loaderBias, bias,
          ClkFormatTime(previous, time1, RTL_NUMBER_OF(time1)), ClkFormatTime(now, time2, RTL_NUMBER_OF(time2)),
          (target.QuadPart - previous.QuadPart) / 10000000LL);
    if (active != bias)
    {
        RtlInitUnicodeString(&activeU, L"ActiveTimeBias");
        status = NtSetValueKey(zoneKey, &activeU, 0, REG_DWORD, &bias, sizeof(bias));
        NtLog(TRUE, L"[WARN] clock: ActiveTimeBias %ld after the move, wrote %ld: %x\n", active, bias, status);
    }
    if (kernelBias != bias)
    {
        NtLog(TRUE, L"[ERROR] clock: kernel bias %ld after the move, expected %ld\n", kernelBias, bias);
        goto cleanup;
    }
    outcome = L"corrected";

cleanup:
    if (windowsKey)
        NtClose(windowsKey);
    if (zoneKey)
        NtClose(zoneKey);
    return outcome;
}

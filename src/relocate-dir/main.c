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

#include "io.h"

HANDLE g_Heap;

#ifdef __MINGW32__
#define TEXT(x) x
#endif

__declspec(dllimport)
int _vsnwprintf(
    wchar_t *buffer,
    size_t count,
    const wchar_t *format,
    va_list argptr
    );

__declspec(dllimport)
int _snwprintf(
    wchar_t *buffer,
    size_t count,
    const wchar_t *format,
    ...
    );

void Sleep(ULONG milliseconds)
{
    LARGE_INTEGER sleepInterval;

    sleepInterval.QuadPart = -1LL * NANOTICKS * milliseconds;
    NtDelayExecution(FALSE, &sleepInterval);
}

void DisplayString(IN const PWCHAR msg)
{
    UNICODE_STRING us;

#pragma warning(push)
#pragma warning(disable: 4090) // different const qualifiers
    // The call below doesn't change the string.
    us.Buffer = msg;
#pragma warning(pop)
    us.Length = (USHORT)wcslen(msg) * sizeof(WCHAR);
    us.MaximumLength = us.Length + sizeof(WCHAR);

    NtDisplayString(&us);
}

void NtPrintf(IN const PWCHAR format, ...)
{
    va_list args;
    WCHAR buffer[1024];

    va_start(args, format);
    _vsnwprintf(buffer, RTL_NUMBER_OF(buffer), format, args);
    va_end(args);

    DisplayString(buffer);
}

// Log path is kept so the result record (see WriteResultRecord) can point at it.
static WCHAR g_LogFileName[256] = { 0 };

// Fixed-name, machine-readable outcome of the last run. The per-run log has a timestamped name
// and free-text content, so nothing could tell "the move failed" from "it never ran"; a checker
// (installer/health-check) can read this file instead.
#define RESULT_RECORD_PATH L"c:\\relocate-dir-result.txt"

void NtLog(IN BOOLEAN print, IN const PWCHAR format, ...)
{
    va_list args;
    TIME_FIELDS tf;
    LARGE_INTEGER systemTime, localTime;
    WCHAR buffer[1024];
    BYTE utf16Bom[2] = { 0xFF, 0xFE };
    static HANDLE logFile = NULL;
    NTSTATUS status;

    if (!logFile)
    {
        NtQuerySystemTime(&systemTime);
        RtlSystemTimeToLocalTime(&systemTime, &localTime);
        RtlTimeToTimeFields(&localTime, &tf);
        _snwprintf(g_LogFileName, RTL_NUMBER_OF(g_LogFileName),
                   L"c:\\relocate-dir-%04d%02d%02d-%02d%02d%02d.log", // TODO: read from registry
                   tf.Year, tf.Month, tf.Day, tf.Hour, tf.Minute, tf.Second);
        g_LogFileName[RTL_NUMBER_OF(g_LogFileName) - 1] = L'\0';
        status = FileOpen(&logFile, g_LogFileName, TRUE, TRUE, FALSE);
        if (!NT_SUCCESS(status))
            goto print;
        FileWrite(logFile, utf16Bom, RTL_NUMBER_OF(utf16Bom), NULL);
    }

    va_start(args, format);
    _vsnwprintf(buffer, RTL_NUMBER_OF(buffer), format, args);
    va_end(args);

print:
    if (print)
        DisplayString(buffer);
    if (logFile)
        FileWrite(logFile, buffer, (ULONG)(sizeof(WCHAR)*wcslen(buffer)), NULL);
}

HANDLE InitHeap(void)
{
    RTL_HEAP_PARAMETERS heapParams;

    RtlZeroMemory(&heapParams, sizeof(heapParams));
    heapParams.Length = sizeof(heapParams);
    return RtlCreateHeap(HEAP_GROWABLE, NULL, 0x100000, 0x1000, NULL, &heapParams);
}

BOOLEAN FreeHeap(HANDLE heap)
{
    return NULL == RtlDestroyHeap(heap);
}

NTSTATUS EnablePrivileges(void)
{
    HANDLE processToken = NULL;
    TOKEN_PRIVILEGES *tp = NULL;
    ULONG size;
    NTSTATUS status;
    const int privilegeCount = 3;

    // This is a variable-size struct, but definition contains 1 element by default.
    size = sizeof(TOKEN_PRIVILEGES) + (privilegeCount - 1) * sizeof(LUID_AND_ATTRIBUTES);
    tp = RtlAllocateHeap(g_Heap, 0, size);

    status = NtOpenProcessToken(NtCurrentProcess(), TOKEN_ALL_ACCESS, &processToken);
    if (!NT_SUCCESS(status))
        goto cleanup;

    tp->PrivilegeCount = privilegeCount;
    tp->Privileges[0].Luid.HighPart = 0;
    tp->Privileges[0].Luid.LowPart = SE_SECURITY_PRIVILEGE; // needed for file security manipulation
    tp->Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    tp->Privileges[1].Luid.HighPart = 0;
    tp->Privileges[1].Luid.LowPart = SE_BACKUP_PRIVILEGE; // needed for reading files with ACLs that don't grant access to SYSTEM
    tp->Privileges[1].Attributes = SE_PRIVILEGE_ENABLED;
    tp->Privileges[2].Luid.HighPart = 0;
    tp->Privileges[2].Luid.LowPart = SE_RESTORE_PRIVILEGE; // needed for setting file ownership
    tp->Privileges[2].Attributes = SE_PRIVILEGE_ENABLED;

    status = NtAdjustPrivilegesToken(processToken, FALSE, tp, size, NULL, NULL);

cleanup:
    if (processToken)
        NtClose(processToken);
    if (tp)
        RtlFreeHeap(g_Heap, 0, tp);
    return status;
}

// Writes RESULT_RECORD_PATH (overwritten each run). Fields are one `key=value` per line so a
// script can grep them; the log path is included because the log name is timestamped.
void WriteResultRecord(IN const WCHAR *result, IN const WCHAR *reason, IN NTSTATUS status,
                       IN const WCHAR *source, IN const WCHAR *target)
{
    HANDLE file = NULL;
    WCHAR buffer[1024];
    BYTE utf16Bom[2] = { 0xFF, 0xFE };
    NTSTATUS openStatus;

    openStatus = FileOpen(&file, RESULT_RECORD_PATH, TRUE, TRUE, FALSE);
    if (!NT_SUCCESS(openStatus))
    {
        NtLog(TRUE, L"[!] WriteResultRecord: FileOpen(%s) failed: %x\n", RESULT_RECORD_PATH, openStatus);
        return;
    }

    _snwprintf(buffer, RTL_NUMBER_OF(buffer),
               L"result=%s\r\nreason=%s\r\nstatus=0x%08x\r\nsource=%s\r\ntarget=%s\r\nlog=%s\r\n",
               result, reason, status,
               source ? source : L"", target ? target : L"", g_LogFileName);
    buffer[RTL_NUMBER_OF(buffer) - 1] = L'\0';

    FileWrite(file, utf16Bom, RTL_NUMBER_OF(utf16Bom), NULL);
    FileWrite(file, buffer, (ULONG)(sizeof(WCHAR) * wcslen(buffer)), NULL);
    NtClose(file);

    NtLog(TRUE, L"[*] Result record written to %s: result=%s reason=%s\n", RESULT_RECORD_PATH, result, reason);
}

// Not declared by ntifs.h (only the Zw variant is); exported by ntdll, same signature.
NTSTATUS
NTAPI
NtQueryValueKey(
    IN  HANDLE KeyHandle,
    IN  PUNICODE_STRING ValueName,
    IN  KEY_VALUE_INFORMATION_CLASS KeyValueInformationClass,
    OUT PVOID KeyValueInformation OPTIONAL,
    IN  ULONG Length,
    OUT PULONG ResultLength
    );

// TRUE if a BootExecute entry is ours. The installer registers the literal
// "relocate-dir.exe <src> <dst>" (CoreComponents.wxs); match on the image name, case-insensitive
// (ASCII fold is enough for that literal), so no other vendor's entry can be mistaken for it.
static BOOLEAN IsOwnBootExecuteEntry(IN const WCHAR *entry)
{
    static const WCHAR needle[] = L"relocate-dir";
    const size_t needleLen = RTL_NUMBER_OF(needle) - 1;
    size_t entryLen = wcslen(entry);
    size_t i, j;

    for (i = 0; i + needleLen <= entryLen; i++)
    {
        for (j = 0; j < needleLen; j++)
        {
            WCHAR c = entry[i + j];
            if (c >= L'A' && c <= L'Z')
                c = c - L'A' + L'a';
            if (c != needle[j])
                break;
        }
        if (j == needleLen)
            return TRUE;
    }
    return FALSE;
}

// Removes ONLY this program's entry from BootExecute. The previous version wrote the default
// "autocheck autochk *" wholesale, so every other entry registered there (other vendors, or
// anything Windows itself added) was dropped on EVERY exit path, success or failure. The
// wholesale default write is now only the fallback when the current value cannot be read or is
// not a multi-string, and it is logged as an anomaly when it happens.
NTSTATUS RemoveBootExecuteEntry(void)
{
    WCHAR keyName[] = L"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\Session Manager";
    WCHAR valueName[] = L"BootExecute";
    WCHAR defaultValue[] = L"autocheck autochk *\0"; // multi-string, so double null-terminated
    UNICODE_STRING keyNameU, valueNameU;
    OBJECT_ATTRIBUTES oa;
    HANDLE key = NULL;
    KEY_VALUE_PARTIAL_INFORMATION *info = NULL;
    WCHAR *newValue = NULL;
    WCHAR *src, *end, *dst;
    ULONG resultLength = 0;
    ULONG kept = 0, removed = 0;
    NTSTATUS status;

    keyNameU.Buffer = keyName;
    keyNameU.Length = (USHORT)wcslen(keyName) * sizeof(WCHAR);
    keyNameU.MaximumLength = keyNameU.Length + sizeof(WCHAR);

    InitializeObjectAttributes(
        &oa,
        &keyNameU,
        OBJ_CASE_INSENSITIVE,
        NULL,
        NULL);

    status = NtOpenKey(&key, KEY_READ | KEY_WRITE, &oa);
    if (!NT_SUCCESS(status))
    {
        NtLog(TRUE, L"[!] RemoveBootExecuteEntry: NtOpenKey(%s) failed: %x\n", keyName, status);
        goto cleanup;
    }

    valueNameU.Buffer = valueName;
    valueNameU.Length = (USHORT)wcslen(valueName) * sizeof(WCHAR);
    valueNameU.MaximumLength = valueNameU.Length + sizeof(WCHAR);

    // Read the current multi-string: size query, then the data.
    status = NtQueryValueKey(key, &valueNameU, KeyValuePartialInformation, NULL, 0, &resultLength);
    if ((status == STATUS_BUFFER_TOO_SMALL || status == STATUS_BUFFER_OVERFLOW) && resultLength > 0)
    {
        // Two extra WCHARs (zeroed) guarantee termination even if the stored data lacks it.
        info = RtlAllocateHeap(g_Heap, HEAP_ZERO_MEMORY, resultLength + 2 * sizeof(WCHAR));
        if (!info)
        {
            status = STATUS_NO_MEMORY;
        }
        else
        {
            status = NtQueryValueKey(key, &valueNameU, KeyValuePartialInformation, info, resultLength, &resultLength);
        }
    }

    if (!NT_SUCCESS(status) || !info || info->Type != REG_MULTI_SZ)
    {
        // Cannot see what else is registered: fall back to the old wholesale default so this
        // program does not re-run (and re-copy) on every boot. Loud, because it drops entries.
        NtLog(TRUE, L"[!] RemoveBootExecuteEntry: cannot read current %s (status %x, type %u); "
                    L"writing default value, other entries (if any) are LOST\n",
              valueName, status, info ? info->Type : 0);
        status = NtSetValueKey(key, &valueNameU, 0, REG_MULTI_SZ, defaultValue, sizeof(defaultValue));
        if (!NT_SUCCESS(status))
            NtLog(TRUE, L"[!] RemoveBootExecuteEntry: NtSetValueKey(%s, %s) failed: %x\n", keyName, valueName, status);
        goto cleanup;
    }

    // Rebuild the multi-string without our entry. Output is never longer than the input.
    newValue = RtlAllocateHeap(g_Heap, HEAP_ZERO_MEMORY, info->DataLength + 2 * sizeof(WCHAR));
    if (!newValue)
    {
        NtLog(TRUE, L"[!] RemoveBootExecuteEntry: out of memory\n");
        status = STATUS_NO_MEMORY;
        goto cleanup;
    }

    src = (WCHAR *)info->Data;
    end = src + info->DataLength / sizeof(WCHAR);
    dst = newValue;
    while (src < end && *src)
    {
        size_t len = 0;
        while (src + len < end && src[len])
            len++;

        if (IsOwnBootExecuteEntry(src))
        {
            NtLog(TRUE, L"[*] RemoveBootExecuteEntry: removing own entry '%s'\n", src);
            removed++;
        }
        else
        {
            RtlCopyMemory(dst, src, len * sizeof(WCHAR));
            dst[len] = L'\0';
            dst += len + 1;
            kept++;
        }
        src += len + 1;
    }
    *dst++ = L'\0'; // multi-string terminator

    if (removed == 0)
        NtLog(TRUE, L"[!] RemoveBootExecuteEntry: own entry not found in %s (nothing removed)\n", valueName);

    if (kept == 0)
    {
        // Nothing but our entry was there, i.e. autochk had already been dropped; restore it.
        NtLog(TRUE, L"[!] RemoveBootExecuteEntry: no other entries present, writing default value\n");
        status = NtSetValueKey(key, &valueNameU, 0, REG_MULTI_SZ, defaultValue, sizeof(defaultValue));
    }
    else
    {
        status = NtSetValueKey(key, &valueNameU, 0, REG_MULTI_SZ, newValue, (ULONG)((dst - newValue) * sizeof(WCHAR)));
    }

    if (!NT_SUCCESS(status))
    {
        NtLog(TRUE, L"[!] RemoveBootExecuteEntry: NtSetValueKey(%s, %s) failed: %x\n", keyName, valueName, status);
        goto cleanup;
    }

    NtLog(TRUE, L"[*] RemoveBootExecuteEntry: %u entr%s kept, %u removed\n", kept, kept == 1 ? L"y" : L"ies", removed);
    status = STATUS_SUCCESS;

cleanup:
    if (newValue)
        RtlFreeHeap(g_Heap, 0, newValue);
    if (info)
        RtlFreeHeap(g_Heap, 0, info);
    if (key)
        NtClose(key);
    return status;
}

NTSTATUS wmain(INT argc, WCHAR *argv[], WCHAR *envp[], ULONG DebugFlag OPTIONAL)
{
    NTSTATUS status;
    ULONG attrs;
    TIME_FIELDS tf;
    LARGE_INTEGER systemTime, localTime;
    // Outcome for the result record. Every abort path used to look identical from outside
    // (Windows boots normally, C:\Users still on the root volume, nothing reports it); the
    // record makes "failed", "already done" and "never ran" distinguishable. Default = failed.
    const WCHAR *result = L"failed";
    const WCHAR *reason = L"unknown";

    UNREFERENCED_PARAMETER(envp);
    UNREFERENCED_PARAMETER(DebugFlag);

    NtLog(TRUE, L"move-profiles (" TEXT(__DATE__) L" " TEXT(__TIME__) L")\n");

    status = EnablePrivileges();
    if (!NT_SUCCESS(status))
    {
        NtLog(TRUE, L"[!] EnablePrivileges failed: %x\n", status);
        reason = L"enable-privileges";
        goto cleanup;
    }

    NtQuerySystemTime(&systemTime);
    RtlSystemTimeToLocalTime(&systemTime, &localTime);
    RtlTimeToTimeFields(&localTime, &tf);

    NtLog(TRUE, L"[*] Start time: %04d-%02d-%02d %02d:%02d:%02d.%03d\n",
        tf.Year, tf.Month, tf.Day, tf.Hour, tf.Minute, tf.Second, tf.Milliseconds);

    if (argc < 3)
    {
        NtLog(TRUE, L"[!] Usage: move-profiles <source dir> <target dir>\n");
        status = STATUS_INVALID_PARAMETER;
        reason = L"usage";
        goto cleanup;
    }

    // Check if source directory is already a symlink.
    status = FileGetAttributes(argv[1], &attrs);
    if (!NT_SUCCESS(status))
    {
        NtLog(TRUE, L"[!] FileGetAttributes(%s) failed: %x\n", argv[1], status);
        reason = L"source-attributes";
        goto cleanup;
    }

    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT)
    {
        NtLog(TRUE, L"[*] Source directory (%s) is already a reparse point, aborting\n", argv[1]);
        result = L"skipped";
        reason = L"source-already-reparse-point";
        goto cleanup;
    }

    // Check if destination directory exists.
    status = FileGetAttributes(argv[2], &attrs);
    if (NT_SUCCESS(status))
    {
        // Source is NOT relocated (checked above) yet the target exists: typically the partial
        // copy left by an earlier failed run. The move did not happen, so this is a failure.
        NtLog(TRUE, L"[?] Destination directory (%s) already exists, aborting\n", argv[2]);
        reason = L"destination-exists";
        goto cleanup;
    }

    // TODO: parsing quotes so directories can have embedded spaces
    // Might happen in some non-english languages?
    NtLog(TRUE, L"[*] Copying: '%s' -> '%s', stand by...\n", argv[1], argv[2]);
    status = FileCopyDirectory(argv[1], argv[2], FALSE);
    if (!NT_SUCCESS(status))
    {
        NtLog(TRUE, L"[!] FileCopyDirectory(%s, %s) failed: %x\n", argv[1], argv[2], status);
        reason = L"copy";
        goto cleanup;
    }

    NtLog(TRUE, L"[*] Deleting: '%s'\n", argv[1]);
    status = FileDeleteDirectory(argv[1], FALSE);
    if (!NT_SUCCESS(status))
    {
        NtLog(TRUE, L"[!] FileDeleteDirectory failed: %x\n", status);

        // Attempt to restore previous state.
        FileCopyDirectory(argv[2], argv[1], TRUE);
        reason = L"delete";
        goto cleanup;
    }

    NtLog(TRUE, L"[*] Creating symlink: '%s' -> '%s'\n", argv[1], argv[2]);
    status = FileSetSymlink(argv[1], argv[2]);
    if (!NT_SUCCESS(status))
    {
        NtLog(TRUE, L"[!] FileSetReparsePoint failed: %x\n", status);
        reason = L"symlink";
        goto cleanup;
    }

    status = STATUS_SUCCESS;
    result = L"ok";
    reason = L"relocated";

cleanup:
    NtQuerySystemTime(&systemTime);
    RtlSystemTimeToLocalTime(&systemTime, &localTime);
    RtlTimeToTimeFields(&localTime, &tf);

    NtLog(TRUE, L"[*] End time: %04d-%02d-%02d %02d:%02d:%02d.%03d\n",
        tf.Year, tf.Month, tf.Day, tf.Hour, tf.Minute, tf.Second, tf.Milliseconds);

    WriteResultRecord(result, reason, status,
                      argc > 1 ? argv[1] : NULL, argc > 2 ? argv[2] : NULL);

    // Remove itself from BootExecute.
    RemoveBootExecuteEntry();

    return status;
}

void EnvironmentStringToUnicodeString(IN WCHAR *wsIn, OUT UNICODE_STRING *usOut)
{
    if (wsIn)
    {
        WCHAR *currentChar = wsIn;

        while (*currentChar)
        {
            while (*currentChar++);
        }

        currentChar++;

        usOut->Buffer = wsIn;
        usOut->MaximumLength = usOut->Length = (USHORT)(currentChar - wsIn) * sizeof(WCHAR);
    }
    else
    {
        usOut->Buffer = NULL;
        usOut->Length = usOut->MaximumLength = 0;
    }
}

// from ReactOS lib/nt/entry_point.c
void NtProcessStartup(PPEB2 Peb)
{
    NTSTATUS Status;
    PRTL_USER_PROCESS_PARAMETERS ProcessParameters;
    UNICODE_STRING *CmdLineString;
    //UNICODE_STRING UnicodeEnvironment;
    PWCHAR NullPointer = NULL;
    INT argc = 0;
    PWCHAR *argv;
    PWCHAR *envp;
    PWCHAR *ArgumentList;
    PWCHAR Source, Destination;
    ULONG Length;

    /* Normalize and get the Process Parameters */
    ProcessParameters = RtlNormalizeProcessParams(Peb->ProcessParameters);

    Status = STATUS_NO_MEMORY;
    g_Heap = InitHeap();
    if (!g_Heap)
        goto fail;

    /* Allocate memory for the argument list, enough for 512 tokens */
    //FIXME: what if 512 is not enough????
    ArgumentList = RtlAllocateHeap(g_Heap, 0, 512 * sizeof(PWCHAR));
    if (!ArgumentList)
        goto fail;

    /* Use a null pointer as default */
    argv = &NullPointer;
    envp = &NullPointer;

    /* Set the first pointer to NULL, and set the argument array to the buffer */
    *ArgumentList = NULL;
    argv = ArgumentList;

    /* Get the pointer to the Command Line */
    CmdLineString = &ProcessParameters->CommandLine;

    /* If we don't have a command line, use the image path instead */
    if (!CmdLineString->Buffer || !CmdLineString->Length)
    {
        CmdLineString = &ProcessParameters->ImagePathName;
    }

    /* Save parameters for parsing */
    Source = CmdLineString->Buffer;
    Length = CmdLineString->Length;

    /* Ensure it's valid */
    if (Source)
    {
        /* Allocate a buffer for the destination */
        Destination = RtlAllocateHeap(g_Heap, 0, (Length + 1) * sizeof(WCHAR));
        if (!Destination)
            goto fail;

        /* Start parsing */
        while (*Source)
        {
            /* Skip the white space. */
            while (*Source && *Source <= L' ') Source++;

            /* Copy until the next white space is reached */
            if (*Source)
            {
                /* Save one token pointer */
                *ArgumentList++ = Destination;

                /* Increase one token count */
                argc++;

                /* Copy token until white space */
                while (*Source > L' ')
                    *Destination++ = *Source++;

                /* Null terminate it */
                *Destination++ = L'\0';
            }
        }
    }

    /* Null terminate the token pointer list */
    *ArgumentList++ = NULL;

    /* Now handle the enviornment, point the envp at our current list location. */
    envp = ArgumentList;

    /* Call the Main Function */
    Status = wmain(argc, argv, envp, 0);

fail:
    /* We're done here */
    NtTerminateProcess(NtCurrentProcess(), Status);
}

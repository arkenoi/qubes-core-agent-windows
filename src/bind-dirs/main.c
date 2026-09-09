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

/*
 * bind-dirs.exe - NATIVE application run from Session Manager's BootExecute, every boot,
 * after relocate-dir.exe (MoveUsers) and before the Win32 subsystem or any service starts.
 * That is the only point at which a directory a service opens at start-up can still be
 * replaced by a junction - the same reason relocate-dir lives there. Cost of the choice:
 * ntdll only (no kernel32, no CRT beyond what ntdll exports), text output goes to the boot
 * screen via NtDisplayString, and a hang here is a hang of the boot, so nothing in this
 * program waits or retries.
 *
 * Unlike relocate-dir it does NOT remove itself from BootExecute: an AppVM's C: is reset at
 * every boot, so the junctions must be re-created at every boot (Linux: the bind mounts are
 * redone by qubes-bind-dirs.service at every boot too).
 *
 * The decision logic is in bind-dirs.c (portable, tested offline); this file is the ntdll
 * implementation of its BD_FS interface, logging, the result record and process start-up.
 * File primitives come from relocate-dir's io.c where they are usable as-is.
 */

#include "../relocate-dir/io.h"
#include "bind-dirs.h"

HANDLE g_Heap;   // io.c allocates from it

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

// Defined in io.c but not declared in io.h.
NTSTATUS FileCopyBasicInformation(IN HANDLE source, IN HANDLE target);

// --- where things live on the guest --------------------------------------------------
// Log and result record go to the PRIVATE volume: an AppVM's C: is discarded at reboot, so a
// log there would not survive the very boot that failed. Only if Q: cannot be written does
// the log fall back to C: (and says so on the boot screen).
#define LOG_DIR            L"Q:\\Qubes Logs"
#define LOG_PATH           L"Q:\\Qubes Logs\\bind-dirs.log"
#define LOG_PREV_PATH      L"Q:\\Qubes Logs\\bind-dirs.prev.log"
#define LOG_FALLBACK_PATH  L"C:\\bind-dirs.log"
// Fixed-name, machine-readable outcome of the last run (same idea as relocate-dir's
// c:\relocate-dir-result.txt): "failed", "ok" and "never ran" must be distinguishable.
#define RESULT_RECORD_PATH L"Q:\\Qubes Logs\\bind-dirs-result.txt"

// Config sources, applied in this order (Linux: /usr/lib/qubes-bind-dirs.d,
// /etc/qubes-bind-dirs.d, /rw/config/qubes-bind-dirs.d).
static const wchar_t *const g_ConfigSources[] = {
    L"C:\\Program Files\\Qubes Tools\\qubes-bind-dirs.d",   // shipped with the tools
    L"C:\\ProgramData\\Qubes\\qubes-bind-dirs.d",          // template administrator
    L"Q:\\config\\qubes-bind-dirs.d",                       // the user, on the private volume
};

// ------------------------------------------------------------------ logging

static HANDLE g_LogFile = NULL;
static WCHAR g_LogFileName[256] = { 0 };

static void DisplayString(IN const PWCHAR msg)
{
    UNICODE_STRING us;

#pragma warning(push)
#pragma warning(disable: 4090) // different const qualifiers
    us.Buffer = msg;
#pragma warning(pop)
    us.Length = (USHORT)wcslen(msg) * sizeof(WCHAR);
    us.MaximumLength = us.Length + sizeof(WCHAR);

    NtDisplayString(&us);
}

static void NtLogV(IN BOOLEAN print, IN const WCHAR *format, IN va_list args)
{
    WCHAR buffer[2048];

    _vsnwprintf(buffer, RTL_NUMBER_OF(buffer), format, args);
    buffer[RTL_NUMBER_OF(buffer) - 1] = L'\0';

    if (print)
        DisplayString(buffer);
    if (g_LogFile)
        FileWrite(g_LogFile, buffer, (ULONG)(sizeof(WCHAR) * wcslen(buffer)), NULL);
}

// io.c logs through this.
void NtLog(IN BOOLEAN print, IN const PWCHAR format, ...)
{
    va_list args;

    va_start(args, format);
    NtLogV(print, format, args);
    va_end(args);
}

static NTSTATUS LogOpenAt(IN const WCHAR *path)
{
    BYTE utf16Bom[2] = { 0xFF, 0xFE };
    NTSTATUS status;

    status = FileOpen(&g_LogFile, (PWCHAR)path, TRUE, TRUE, FALSE);
    if (!NT_SUCCESS(status))
    {
        g_LogFile = NULL;
        return status;
    }
    FileWrite(g_LogFile, utf16Bom, RTL_NUMBER_OF(utf16Bom), NULL);
    _snwprintf(g_LogFileName, RTL_NUMBER_OF(g_LogFileName), L"%s", path);
    g_LogFileName[RTL_NUMBER_OF(g_LogFileName) - 1] = L'\0';
    return STATUS_SUCCESS;
}

static NTSTATUS NtRenameEntry(IN const WCHAR *oldPath, IN const WCHAR *newPath, IN BOOLEAN replaceIfExists);

// Fixed-name log, previous boot's kept as .prev: two boots of history, no unbounded growth
// on the private volume (this runs at EVERY boot, unlike relocate-dir's one timestamped file).
static void LogOpen(void)
{
    ULONG attrs;
    NTSTATUS status;

    status = FileGetAttributes(LOG_DIR, &attrs);
    if (!NT_SUCCESS(status))
        FileCreateDirectory(LOG_DIR);   // may fail (no Q:) - the open below decides

    NtRenameEntry(LOG_PATH, LOG_PREV_PATH, TRUE);   // best effort

    status = LogOpenAt(LOG_PATH);
    if (NT_SUCCESS(status))
        return;

    if (NT_SUCCESS(LogOpenAt(LOG_FALLBACK_PATH)))
    {
        NtLog(TRUE, L"[!] bind-dirs: cannot open %s (%x); logging to %s, which does NOT survive an AppVM reboot\n",
              LOG_PATH, status, LOG_FALLBACK_PATH);
        return;
    }
    NtLog(TRUE, L"[!] bind-dirs: cannot open any log file (%x), boot-screen output only\n", status);
}

// ------------------------------------------------------------------ privileges (as relocate-dir)

static NTSTATUS EnablePrivileges(void)
{
    HANDLE processToken = NULL;
    TOKEN_PRIVILEGES *tp = NULL;
    ULONG size;
    NTSTATUS status;
    const int privilegeCount = 3;

    size = sizeof(TOKEN_PRIVILEGES) + (privilegeCount - 1) * sizeof(LUID_AND_ATTRIBUTES);
    tp = RtlAllocateHeap(g_Heap, 0, size);
    if (!tp)
        return STATUS_NO_MEMORY;

    status = NtOpenProcessToken(NtCurrentProcess(), TOKEN_ALL_ACCESS, &processToken);
    if (!NT_SUCCESS(status))
        goto cleanup;

    tp->PrivilegeCount = privilegeCount;
    tp->Privileges[0].Luid.HighPart = 0;
    tp->Privileges[0].Luid.LowPart = SE_SECURITY_PRIVILEGE; // SACL copy
    tp->Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    tp->Privileges[1].Luid.HighPart = 0;
    tp->Privileges[1].Luid.LowPart = SE_BACKUP_PRIVILEGE;   // read files whose ACL excludes SYSTEM
    tp->Privileges[1].Attributes = SE_PRIVILEGE_ENABLED;
    tp->Privileges[2].Luid.HighPart = 0;
    tp->Privileges[2].Luid.LowPart = SE_RESTORE_PRIVILEGE;  // set ownership on the seed copy
    tp->Privileges[2].Attributes = SE_PRIVILEGE_ENABLED;

    status = NtAdjustPrivilegesToken(processToken, FALSE, tp, size, NULL, NULL);

cleanup:
    if (processToken)
        NtClose(processToken);
    if (tp)
        RtlFreeHeap(g_Heap, 0, tp);
    return status;
}

// ------------------------------------------------------------------ NT primitives

// Opens the object at `path` ITSELF (FILE_OPEN_REPARSE_POINT: a junction is opened as the
// junction, never followed) with the given access. Share-all, backup intent.
static NTSTATUS OpenEntry(OUT HANDLE *handle, IN const WCHAR *path, IN ACCESS_MASK access, IN ULONG extraOptions)
{
    UNICODE_STRING pathU = { 0 };
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    NTSTATUS status;

    *handle = NULL;
    if (!RtlDosPathNameToNtPathName_U(path, &pathU, NULL, NULL))
        return STATUS_OBJECT_NAME_INVALID;

    InitializeObjectAttributes(&oa, &pathU, OBJ_CASE_INSENSITIVE, NULL, NULL);
    status = NtCreateFile(
        handle,
        access | SYNCHRONIZE,
        &oa,
        &iosb,
        NULL,
        FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        FILE_OPEN,
        FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT | FILE_OPEN_FOR_BACKUP_INTENT | extraOptions,
        NULL,
        0);

    RtlFreeUnicodeString(&pathU);
    if (!NT_SUCCESS(status))
        *handle = NULL;
    return status;
}

// Correct same-volume rename (io.c's FileRename doubles the name length - UNICODE_STRING.Length
// is already in bytes - so it is not used).
static NTSTATUS NtRenameEntry(IN const WCHAR *oldPath, IN const WCHAR *newPath, IN BOOLEAN replaceIfExists)
{
    UNICODE_STRING newU = { 0 };
    FILE_RENAME_INFORMATION *fri = NULL;
    IO_STATUS_BLOCK iosb;
    HANDLE file = NULL;
    ULONG size;
    NTSTATUS status;

    status = OpenEntry(&file, oldPath, DELETE | FILE_READ_ATTRIBUTES, 0);
    if (!NT_SUCCESS(status))
        return status;

    if (!RtlDosPathNameToNtPathName_U(newPath, &newU, NULL, NULL))
    {
        status = STATUS_OBJECT_NAME_INVALID;
        goto cleanup;
    }

    size = sizeof(FILE_RENAME_INFORMATION) + newU.Length;
    fri = RtlAllocateHeap(g_Heap, HEAP_ZERO_MEMORY, size);
    if (!fri)
    {
        status = STATUS_NO_MEMORY;
        goto cleanup;
    }
    fri->ReplaceIfExists = replaceIfExists;
    fri->RootDirectory = NULL;
    fri->FileNameLength = newU.Length;
    RtlCopyMemory(fri->FileName, newU.Buffer, newU.Length);

    status = NtSetInformationFile(file, &iosb, fri, size, FileRenameInformation);

cleanup:
    if (fri)
        RtlFreeHeap(g_Heap, 0, fri);
    if (newU.Buffer)
        RtlFreeUnicodeString(&newU);
    if (file)
        NtClose(file);
    return status;
}

// Reads a reparse point's tag and (for junctions/symlinks) its substitute name.
static NTSTATUS ReadReparse(IN HANDLE file, OUT ULONG *tag, OUT WCHAR *target, IN size_t capacity)
{
    REPARSE_DATA_BUFFER *rdb;
    IO_STATUS_BLOCK iosb;
    NTSTATUS status;
    const WCHAR *name = NULL;
    USHORT nameBytes = 0;

    *tag = 0;
    target[0] = L'\0';

    rdb = RtlAllocateHeap(g_Heap, HEAP_ZERO_MEMORY, MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
    if (!rdb)
        return STATUS_NO_MEMORY;

    status = NtFsControlFile(file, NULL, NULL, NULL, &iosb, FSCTL_GET_REPARSE_POINT, NULL, 0, rdb, MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
    if (!NT_SUCCESS(status))
        goto cleanup;

    *tag = rdb->ReparseTag;
    if (rdb->ReparseTag == IO_REPARSE_TAG_MOUNT_POINT)
    {
        name = rdb->MountPointReparseBuffer.PathBuffer + rdb->MountPointReparseBuffer.SubstituteNameOffset / sizeof(WCHAR);
        nameBytes = rdb->MountPointReparseBuffer.SubstituteNameLength;
    }
    else if (rdb->ReparseTag == IO_REPARSE_TAG_SYMLINK)
    {
        name = rdb->SymbolicLinkReparseBuffer.PathBuffer + rdb->SymbolicLinkReparseBuffer.SubstituteNameOffset / sizeof(WCHAR);
        nameBytes = rdb->SymbolicLinkReparseBuffer.SubstituteNameLength;
    }
    if (name && nameBytes / sizeof(WCHAR) + 1 <= capacity)
    {
        RtlCopyMemory(target, name, nameBytes);
        target[nameBytes / sizeof(WCHAR)] = L'\0';
    }

cleanup:
    RtlFreeHeap(g_Heap, 0, rdb);
    return status;
}

// Strict file copy: data, basic info, security. Kept separate from io.c's FileCopy (whose
// >64 KiB size-check bug was fixed in 0cbb07f) because the seed must be STRICT end to end and
// FileCopyDirectory still discards its children's status; this file's copy tree is the only
// one whose success means "every byte is there".
static NTSTATUS CopyFileStrict(IN const WCHAR *sourcePath, IN const WCHAR *targetPath)
{
    HANDLE source = NULL, target = NULL;
    BYTE *buffer = NULL;
    INT64 fileSize = 0, total = 0;
    ULONG readSize, writtenSize;
    NTSTATUS status;

    status = FileOpen(&source, (PWCHAR)sourcePath, FALSE, FALSE, FALSE);
    if (!NT_SUCCESS(status))
        goto cleanup;
    status = FileOpen(&target, (PWCHAR)targetPath, TRUE, TRUE, FALSE);
    if (!NT_SUCCESS(status))
        goto cleanup;

    status = FileGetSize(source, &fileSize);
    if (!NT_SUCCESS(status))
        goto cleanup;

    buffer = RtlAllocateHeap(g_Heap, 0, 65536);
    if (!buffer)
    {
        status = STATUS_NO_MEMORY;
        goto cleanup;
    }

    while (total < fileSize)
    {
        readSize = 0;
        status = FileRead(source, buffer, 65536, &readSize);
        if (status == STATUS_END_OF_FILE)
        {
            status = STATUS_SUCCESS;
            break;
        }
        if (!NT_SUCCESS(status))
            goto cleanup;
        if (readSize == 0)
            break;
        writtenSize = 0;
        status = FileWrite(target, buffer, readSize, &writtenSize);
        if (!NT_SUCCESS(status))
            goto cleanup;
        if (writtenSize != readSize)
        {
            status = STATUS_UNSUCCESSFUL;
            goto cleanup;
        }
        total += writtenSize;
    }

    if (total != fileSize)
    {
        // Missing data FAILS: a short copy must never pass as a seed.
        NtLog(TRUE, L"[!] CopyFileStrict: %s: copied %I64d of %I64d bytes\n", sourcePath, total, fileSize);
        status = STATUS_UNSUCCESSFUL;
        goto cleanup;
    }

    status = FileCopyBasicInformation(source, target);
    if (!NT_SUCCESS(status))
        goto cleanup;
    status = FileCopySecurity(source, target);

cleanup:
    if (buffer)
        RtlFreeHeap(g_Heap, 0, buffer);
    if (source)
        NtClose(source);
    if (target)
        NtClose(target);
    return status;
}

typedef NTSTATUS (*ENUM_CALLBACK)(void *cookie, const WCHAR *dirPath, const WCHAR *name, ULONG attributes);

// Enumerates a directory's children (not "." / ".."), calling back for each; the first
// failing callback ends the enumeration with its status.
static NTSTATUS EnumerateDirectory(IN const WCHAR *dirPath, IN ENUM_CALLBACK callback, IN void *cookie)
{
    HANDLE dir = NULL, event = NULL;
    FILE_FULL_DIR_INFORMATION *dirInfo = NULL, *entry;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    BOOLEAN firstQuery = TRUE;
    WCHAR *name = NULL;
    NTSTATUS status;

    status = OpenEntry(&dir, dirPath, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_DIRECTORY_FILE);
    if (!NT_SUCCESS(status))
        return status;

    dirInfo = RtlAllocateHeap(g_Heap, 0, 65536);
    name = RtlAllocateHeap(g_Heap, 0, MAX_PATH_LONG * sizeof(WCHAR));
    if (!dirInfo || !name)
    {
        status = STATUS_NO_MEMORY;
        goto cleanup;
    }

    InitializeObjectAttributes(&oa, NULL, 0, NULL, NULL);
    status = ZwCreateEvent(&event, EVENT_ALL_ACCESS, &oa, SynchronizationEvent, FALSE);
    if (!NT_SUCCESS(status))
        goto cleanup;

    while (TRUE)
    {
        status = NtQueryDirectoryFile(dir, event, NULL, 0, &iosb, dirInfo, 65536,
                                      FileFullDirectoryInformation, FALSE, NULL, firstQuery);
        if (status == STATUS_PENDING)
        {
            ZwWaitForSingleObject(event, FALSE, NULL);
            status = iosb.Status;
        }
        if (status == STATUS_NO_MORE_FILES)
        {
            status = STATUS_SUCCESS;
            break;
        }
        if (!NT_SUCCESS(status))
            goto cleanup;

        entry = dirInfo;
        while (entry)
        {
            ULONG chars = entry->FileNameLength / sizeof(WCHAR);
            if (chars < MAX_PATH_LONG &&
                !(chars == 1 && entry->FileName[0] == L'.') &&
                !(chars == 2 && entry->FileName[0] == L'.' && entry->FileName[1] == L'.'))
            {
                RtlCopyMemory(name, entry->FileName, entry->FileNameLength);
                name[chars] = L'\0';
                status = callback(cookie, dirPath, name, entry->FileAttributes);
                if (!NT_SUCCESS(status))
                    goto cleanup;
            }
            if (!entry->NextEntryOffset)
                break;
            entry = (FILE_FULL_DIR_INFORMATION *)((ULONG_PTR)entry + entry->NextEntryOffset);
        }
        firstQuery = FALSE;
    }

cleanup:
    if (name)
        RtlFreeHeap(g_Heap, 0, name);
    if (dirInfo)
        RtlFreeHeap(g_Heap, 0, dirInfo);
    if (event)
        NtClose(event);
    if (dir)
        NtClose(dir);
    return status;
}

static NTSTATUS JoinPath(OUT WCHAR *out, IN size_t capacity, IN const WCHAR *dir, IN const WCHAR *name)
{
    size_t d = wcslen(dir), n = wcslen(name);

    if (d + 1 + n + 1 > capacity)
        return STATUS_NAME_TOO_LONG;
    RtlCopyMemory(out, dir, d * sizeof(WCHAR));
    out[d] = L'\\';
    RtlCopyMemory(out + d + 1, name, (n + 1) * sizeof(WCHAR));
    return STATUS_SUCCESS;
}

static NTSTATUS CopyDirectoryStrict(IN const WCHAR *sourcePath, IN const WCHAR *targetPath);

typedef struct _COPY_CONTEXT
{
    const WCHAR *targetDir;
} COPY_CONTEXT;

static NTSTATUS CopyChild(void *cookie, const WCHAR *dirPath, const WCHAR *name, ULONG attributes)
{
    COPY_CONTEXT *cc = cookie;
    WCHAR *src = NULL, *dst = NULL;
    NTSTATUS status;

    src = RtlAllocateHeap(g_Heap, 0, MAX_PATH_LONG * sizeof(WCHAR));
    dst = RtlAllocateHeap(g_Heap, 0, MAX_PATH_LONG * sizeof(WCHAR));
    if (!src || !dst)
    {
        status = STATUS_NO_MEMORY;
        goto cleanup;
    }
    status = JoinPath(src, MAX_PATH_LONG, dirPath, name);
    if (!NT_SUCCESS(status))
        goto cleanup;
    status = JoinPath(dst, MAX_PATH_LONG, cc->targetDir, name);
    if (!NT_SUCCESS(status))
        goto cleanup;

    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
        status = FileCopyReparsePoint(src, dst);       // copied as a link, never followed
    else if (attributes & FILE_ATTRIBUTE_DIRECTORY)
        status = CopyDirectoryStrict(src, dst);
    else
        status = CopyFileStrict(src, dst);

    if (!NT_SUCCESS(status))
        NtLog(TRUE, L"[!] copy %s -> %s failed: %x\n", src, dst, status);

cleanup:
    if (src)
        RtlFreeHeap(g_Heap, 0, src);
    if (dst)
        RtlFreeHeap(g_Heap, 0, dst);
    return status;
}

// Strict recursive copy (io.c's FileCopyDirectory discards every child's status, so a
// partial copy returns success there - unusable as a seed).
static NTSTATUS CopyDirectoryStrict(IN const WCHAR *sourcePath, IN const WCHAR *targetPath)
{
    HANDLE source = NULL, target = NULL;
    COPY_CONTEXT cc;
    NTSTATUS status;

    status = FileCreateDirectory((PWCHAR)targetPath);
    if (!NT_SUCCESS(status))
        return status;

    cc.targetDir = targetPath;
    status = EnumerateDirectory(sourcePath, CopyChild, &cc);
    if (!NT_SUCCESS(status))
        return status;

    // Attributes and ACLs after the children, so a restrictive ACL cannot block the copy.
    status = FileOpen(&source, (PWCHAR)sourcePath, FALSE, FALSE, FALSE);
    if (!NT_SUCCESS(status))
        goto cleanup;
    status = FileOpen(&target, (PWCHAR)targetPath, TRUE, FALSE, FALSE);
    if (!NT_SUCCESS(status))
        goto cleanup;
    status = FileCopyBasicInformation(source, target);
    if (!NT_SUCCESS(status))
        goto cleanup;
    status = FileCopySecurity(source, target);

cleanup:
    if (source)
        NtClose(source);
    if (target)
        NtClose(target);
    return status;
}

// Sets a junction (IO_REPARSE_TAG_MOUNT_POINT) on an existing empty directory. No privilege
// is needed for a junction (unlike a symbolic link), which is one reason it is the mechanism.
static NTSTATUS SetJunction(IN const WCHAR *path, IN const WCHAR *targetPath)
{
    REPARSE_DATA_BUFFER *rdb = NULL;
    IO_STATUS_BLOCK iosb;
    HANDLE dir = NULL;
    USHORT subBytes, printBytes;
    WCHAR *pb;
    ULONG total;
    NTSTATUS status;

    // Substitute name: \??\Q:\bind-dirs\..., print name: Q:\bind-dirs\...
    subBytes = (USHORT)((4 + wcslen(targetPath)) * sizeof(WCHAR));
    printBytes = (USHORT)(wcslen(targetPath) * sizeof(WCHAR));
    total = REPARSE_DATA_BUFFER_HEADER_SIZE + 8 + subBytes + sizeof(WCHAR) + printBytes + sizeof(WCHAR);
    if (total > MAXIMUM_REPARSE_DATA_BUFFER_SIZE)
        return STATUS_NAME_TOO_LONG;

    rdb = RtlAllocateHeap(g_Heap, HEAP_ZERO_MEMORY, MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
    if (!rdb)
        return STATUS_NO_MEMORY;

    rdb->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    rdb->ReparseDataLength = (USHORT)(total - REPARSE_DATA_BUFFER_HEADER_SIZE);
    rdb->Reserved = 0;
    rdb->MountPointReparseBuffer.SubstituteNameOffset = 0;
    rdb->MountPointReparseBuffer.SubstituteNameLength = subBytes;
    rdb->MountPointReparseBuffer.PrintNameOffset = subBytes + sizeof(WCHAR);
    rdb->MountPointReparseBuffer.PrintNameLength = printBytes;
    pb = rdb->MountPointReparseBuffer.PathBuffer;
    RtlCopyMemory(pb, L"\\??\\", 4 * sizeof(WCHAR));
    RtlCopyMemory(pb + 4, targetPath, printBytes);
    pb[subBytes / sizeof(WCHAR)] = L'\0';
    RtlCopyMemory(pb + subBytes / sizeof(WCHAR) + 1, targetPath, printBytes);
    pb[subBytes / sizeof(WCHAR) + 1 + printBytes / sizeof(WCHAR)] = L'\0';

    status = OpenEntry(&dir, path, FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES | FILE_READ_ATTRIBUTES, FILE_DIRECTORY_FILE);
    if (!NT_SUCCESS(status))
        goto cleanup;

    status = NtFsControlFile(dir, NULL, NULL, NULL, &iosb, FSCTL_SET_REPARSE_POINT, rdb, total, NULL, 0);

cleanup:
    if (dir)
        NtClose(dir);
    if (rdb)
        RtlFreeHeap(g_Heap, 0, rdb);
    return status;
}

// ------------------------------------------------------------------ BD_FS implementation

static BD_STATUS FsStat(void *context, const wchar_t *path, BD_STAT *info)
{
    HANDLE file = NULL;
    FILE_BASIC_INFORMATION fbi;
    IO_STATUS_BLOCK iosb;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(context);
    RtlZeroMemory(info, sizeof(*info));

    status = OpenEntry(&file, path, FILE_READ_ATTRIBUTES, 0);
    if (status == STATUS_OBJECT_NAME_NOT_FOUND || status == STATUS_OBJECT_PATH_NOT_FOUND || status == STATUS_NO_SUCH_FILE)
        return BD_OK;   // exists = 0
    if (!NT_SUCCESS(status))
        return status;

    status = NtQueryInformationFile(file, &iosb, &fbi, sizeof(fbi), FileBasicInformation);
    if (!NT_SUCCESS(status))
        goto cleanup;

    info->exists = 1;
    info->isDirectory = (fbi.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
    if (fbi.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
    {
        ULONG tag = 0;
        info->isReparsePoint = 1;
        // A reparse point we cannot read is still a reparse point (foreign, by construction).
        if (NT_SUCCESS(ReadReparse(file, &tag, info->reparseTarget, RTL_NUMBER_OF(info->reparseTarget))))
            info->reparseTag = tag;
        else
            info->reparseTag = 0xFFFFFFFFUL;
    }
    status = STATUS_SUCCESS;

cleanup:
    NtClose(file);
    return status;
}

static BD_STATUS FsCreateDirectory(void *context, const wchar_t *path)
{
    UNREFERENCED_PARAMETER(context);
    return FileCreateDirectory((PWCHAR)path);
}

static BD_STATUS FsRename(void *context, const wchar_t *oldPath, const wchar_t *newPath)
{
    UNREFERENCED_PARAMETER(context);
    return NtRenameEntry(oldPath, newPath, FALSE);
}

static BD_STATUS FsCopyDirectory(void *context, const wchar_t *sourcePath, const wchar_t *targetPath)
{
    UNREFERENCED_PARAMETER(context);
    return CopyDirectoryStrict(sourcePath, targetPath);
}

static BD_STATUS FsDeleteDirectory(void *context, const wchar_t *path)
{
    BD_STAT st;
    HANDLE file = NULL;
    NTSTATUS status;

    // A junction is deleted as the junction: open it as itself and drop the reparse point +
    // the (empty) directory. Its target is never entered.
    status = FsStat(context, path, &st);
    if (!NT_SUCCESS(status))
        return status;
    if (!st.exists)
        return STATUS_OBJECT_NAME_NOT_FOUND;
    if (st.isReparsePoint)
    {
        status = OpenEntry(&file, path, DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES, 0);
        if (!NT_SUCCESS(status))
            return status;
        status = FileDelete(file);
        NtClose(file);
        return status;
    }
    return FileDeleteDirectory((PWCHAR)path, TRUE);
}

static BD_STATUS FsSetJunction(void *context, const wchar_t *path, const wchar_t *targetPath)
{
    UNREFERENCED_PARAMETER(context);
    return SetJunction(path, targetPath);
}

typedef struct _LIST_CONTEXT
{
    BD_LIST_CALLBACK callback;
    void *cookie;
} LIST_CONTEXT;

static NTSTATUS ListChild(void *cookie, const WCHAR *dirPath, const WCHAR *name, ULONG attributes)
{
    LIST_CONTEXT *lc = cookie;

    UNREFERENCED_PARAMETER(dirPath);
    lc->callback(lc->cookie, name, (attributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0);
    return STATUS_SUCCESS;
}

static BD_STATUS FsListDirectory(void *context, const wchar_t *path, BD_LIST_CALLBACK callback, void *cookie)
{
    LIST_CONTEXT lc;

    UNREFERENCED_PARAMETER(context);
    lc.callback = callback;
    lc.cookie = cookie;
    return EnumerateDirectory(path, ListChild, &lc);
}

static BD_STATUS FsReadFile(void *context, const wchar_t *path, unsigned char **data, unsigned long *size, unsigned long maxBytes)
{
    HANDLE file = NULL;
    INT64 fileSize = 0;
    BYTE *buffer = NULL;
    ULONG total = 0, readSize;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(context);
    *data = NULL;
    *size = 0;

    status = FileOpen(&file, (PWCHAR)path, FALSE, FALSE, FALSE);
    if (!NT_SUCCESS(status))
        return status;
    status = FileGetSize(file, &fileSize);
    if (!NT_SUCCESS(status))
        goto cleanup;
    if (fileSize > (INT64)maxBytes)
    {
        status = STATUS_FILE_TOO_LARGE;
        goto cleanup;
    }
    buffer = RtlAllocateHeap(g_Heap, HEAP_ZERO_MEMORY, (SIZE_T)fileSize + 1);
    if (!buffer)
    {
        status = STATUS_NO_MEMORY;
        goto cleanup;
    }
    while (total < (ULONG)fileSize)
    {
        readSize = 0;
        status = FileRead(file, buffer + total, (ULONG)fileSize - total, &readSize);
        if (status == STATUS_END_OF_FILE)
        {
            status = STATUS_SUCCESS;
            break;
        }
        if (!NT_SUCCESS(status))
            goto cleanup;
        if (readSize == 0)
            break;
        total += readSize;
    }
    if (total != (ULONG)fileSize)
    {
        status = STATUS_UNSUCCESSFUL;   // short read is a failure, not a shorter config
        goto cleanup;
    }
    *data = buffer;
    *size = total;
    buffer = NULL;
    status = STATUS_SUCCESS;

cleanup:
    if (buffer)
        RtlFreeHeap(g_Heap, 0, buffer);
    if (file)
        NtClose(file);
    return status;
}

static void *FsAlloc(void *context, size_t size)
{
    UNREFERENCED_PARAMETER(context);
    return RtlAllocateHeap(g_Heap, HEAP_ZERO_MEMORY, size);
}

static void FsFree(void *context, void *ptr)
{
    UNREFERENCED_PARAMETER(context);
    if (ptr)
        RtlFreeHeap(g_Heap, 0, ptr);
}

static wchar_t FsUpcase(void *context, wchar_t c)
{
    UNREFERENCED_PARAMETER(context);
    return RtlUpcaseUnicodeChar(c);
}

// Boot-screen output only for problems ("[!" prefix); everything is logged. This runs at
// every boot, and a normal boot should not paint a screenful of "already bound".
static void FsLog(void *context, const wchar_t *format, ...)
{
    va_list args;
    BOOLEAN print;

    UNREFERENCED_PARAMETER(context);
    print = (format[0] == L'[' && format[1] == L'!') ? TRUE : FALSE;
    va_start(args, format);
    NtLogV(print, format, args);
    va_end(args);
}

static BD_FS g_Fs = {
    NULL,
    FsStat,
    FsCreateDirectory,
    FsRename,
    FsCopyDirectory,
    FsDeleteDirectory,
    FsSetJunction,
    FsListDirectory,
    FsReadFile,
    FsAlloc,
    FsFree,
    FsUpcase,
    FsLog,
};

// ------------------------------------------------------------------ result record

static void RecordLine(IN HANDLE file, IN const WCHAR *format, ...)
{
    // Three BD_MAX_PATH paths plus tokens fit; not on the stack (BootExecute stack is small).
    static WCHAR buffer[8192];
    va_list args;

    va_start(args, format);
    _vsnwprintf(buffer, RTL_NUMBER_OF(buffer), format, args);
    va_end(args);
    buffer[RTL_NUMBER_OF(buffer) - 1] = L'\0';
    FileWrite(file, buffer, (ULONG)(sizeof(WCHAR) * wcslen(buffer)), NULL);
}

// Overwritten every run. One `key=value` line per field plus one `entry=` line per
// configured path, so a health check can grep it.
static void WriteResultRecord(IN const WCHAR *result, IN const WCHAR *reason, IN NTSTATUS status,
                              IN const BD_LIST *list, IN const BD_ENTRY_RESULT *results, IN const BD_REPORT *report)
{
    HANDLE file = NULL;
    BYTE utf16Bom[2] = { 0xFF, 0xFE };
    NTSTATUS openStatus;
    unsigned long i;

    openStatus = FileOpen(&file, RESULT_RECORD_PATH, TRUE, TRUE, FALSE);
    if (!NT_SUCCESS(openStatus))
    {
        NtLog(TRUE, L"[!] WriteResultRecord: FileOpen(%s) failed: %x\n", RESULT_RECORD_PATH, openStatus);
        return;
    }
    FileWrite(file, utf16Bom, RTL_NUMBER_OF(utf16Bom), NULL);
    RecordLine(file, L"result=%s\r\nreason=%s\r\nstatus=0x%08x\r\n", result, reason, status);
    if (report)
    {
        RecordLine(file, L"entries=%lu\r\nok=%lu\r\nfailed=%lu\r\nseeded=%lu\r\nwarnings=%lu\r\n",
                   report->total, report->ok, report->failed, report->seeded, report->warnings);
    }
    RecordLine(file, L"log=%s\r\n", g_LogFileName);
    if (list && results)
    {
        for (i = 0; i < list->count; i++)
        {
            RecordLine(file, L"entry=%s result=%s reason=%s status=0x%08x seeded=%d warning=%d rollback_failed=%d rw=%s source=%s\r\n",
                       list->entries[i].path, results[i].result, results[i].reason, (ULONG)results[i].status,
                       results[i].seeded, results[i].warning, results[i].rollbackFailed, results[i].rwPath,
                       list->entries[i].source);
        }
    }
    NtClose(file);
    NtLog(FALSE, L"[*] Result record written to %s: result=%s reason=%s\n", RESULT_RECORD_PATH, result, reason);
}

// ------------------------------------------------------------------ main

static void LogTime(IN const WCHAR *label)
{
    TIME_FIELDS tf;
    LARGE_INTEGER systemTime, localTime;

    NtQuerySystemTime(&systemTime);
    RtlSystemTimeToLocalTime(&systemTime, &localTime);
    RtlTimeToTimeFields(&localTime, &tf);
    NtLog(FALSE, L"[*] %s: %04d-%02d-%02d %02d:%02d:%02d.%03d\n", label,
          tf.Year, tf.Month, tf.Day, tf.Hour, tf.Minute, tf.Second, tf.Milliseconds);
}

NTSTATUS wmain(INT argc, WCHAR *argv[], WCHAR *envp[], ULONG DebugFlag OPTIONAL)
{
    NTSTATUS status;
    BD_LIST *list = NULL;
    BD_ENTRY_RESULT *results = NULL;
    BD_REPORT report;
    const WCHAR *result = L"failed";
    const WCHAR *reason = L"unknown";

    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);
    UNREFERENCED_PARAMETER(envp);
    UNREFERENCED_PARAMETER(DebugFlag);

    RtlZeroMemory(&report, sizeof(report));

    // Before the first FileOpen: io.c requests ACCESS_SYSTEM_SECURITY on every open, which
    // needs SeSecurityPrivilege enabled in the token.
    status = EnablePrivileges();
    if (!NT_SUCCESS(status))
    {
        NtLog(TRUE, L"[!] bind-dirs: EnablePrivileges failed: %x\n", status);
        reason = L"enable-privileges";
    }

    LogOpen();
    NtLog(FALSE, L"bind-dirs (" TEXT(__DATE__) L" " TEXT(__TIME__) L")\n");
    LogTime(L"Start time");
    if (!NT_SUCCESS(status))
    {
        NtLog(FALSE, L"[!] EnablePrivileges failed: %x\n", status);
        goto cleanup;
    }

    // Linux: prerequisite() exits on a fully persistent VM. DIFFERS FROM LINUX: not done
    // here - qubesdb is not reachable from BootExecute (no xeniface, no Win32). On a
    // template/standalone the junction is simply created once and found already-bound on
    // every later boot. See docs/BIND-DIRS.md.

    status = BdInitRwRoot(&g_Fs);
    if (BD_FAILED(status))
    {
        reason = L"private-volume";
        goto cleanup;
    }

    list = RtlAllocateHeap(g_Heap, HEAP_ZERO_MEMORY, sizeof(BD_LIST));
    if (!list)
    {
        status = STATUS_NO_MEMORY;
        reason = L"out-of-memory";
        goto cleanup;
    }

    status = BdLoadConfig(&g_Fs, g_ConfigSources, RTL_NUMBER_OF(g_ConfigSources), list);
    if (BD_FAILED(status))
    {
        NtLog(TRUE, L"[!] bind-dirs: configuration error, NOTHING bound (see %s)\n", g_LogFileName);
        reason = L"config";
        goto cleanup;
    }

    if (list->count == 0)
    {
        NtLog(FALSE, L"[*] no bind-dirs configured\n");
        status = STATUS_SUCCESS;
        result = L"ok";
        reason = L"no-config";
        goto cleanup;
    }

    results = RtlAllocateHeap(g_Heap, HEAP_ZERO_MEMORY, list->count * sizeof(BD_ENTRY_RESULT));
    if (!results)
    {
        status = STATUS_NO_MEMORY;
        reason = L"out-of-memory";
        goto cleanup;
    }

    status = BdRun(&g_Fs, list, results, &report);
    if (BD_FAILED(status))
    {
        NtLog(TRUE, L"[!] bind-dirs: %lu of %lu entries FAILED (see %s)\n", report.failed, report.total, g_LogFileName);
        reason = L"entries-failed";
        goto cleanup;
    }
    result = L"ok";
    reason = L"bound";

cleanup:
    LogTime(L"End time");
    WriteResultRecord(result, reason, status, list, results, list ? &report : NULL);
    if (results)
        RtlFreeHeap(g_Heap, 0, results);
    if (list)
        RtlFreeHeap(g_Heap, 0, list);
    if (g_LogFile)
    {
        NtClose(g_LogFile);
        g_LogFile = NULL;
    }
    return status;
}

// ------------------------------------------------------------------ process start-up
// Same as relocate-dir (from ReactOS lib/nt/entry_point.c): a native image has no CRT
// start-up, so argv/envp are carved out of the PEB by hand.

HANDLE InitHeap(void)
{
    RTL_HEAP_PARAMETERS heapParams;

    RtlZeroMemory(&heapParams, sizeof(heapParams));
    heapParams.Length = sizeof(heapParams);
    return RtlCreateHeap(HEAP_GROWABLE, NULL, 0x100000, 0x1000, NULL, &heapParams);
}

void NtProcessStartup(PPEB2 Peb)
{
    NTSTATUS Status;
    PRTL_USER_PROCESS_PARAMETERS ProcessParameters;
    UNICODE_STRING *CmdLineString;
    PWCHAR NullPointer = NULL;
    INT argc = 0;
    PWCHAR *argv;
    PWCHAR *envp;
    PWCHAR *ArgumentList;
    PWCHAR Source, Destination;
    ULONG Length;

    ProcessParameters = RtlNormalizeProcessParams(Peb->ProcessParameters);

    Status = STATUS_NO_MEMORY;
    g_Heap = InitHeap();
    if (!g_Heap)
        goto fail;

    ArgumentList = RtlAllocateHeap(g_Heap, 0, 512 * sizeof(PWCHAR));
    if (!ArgumentList)
        goto fail;

    argv = &NullPointer;
    envp = &NullPointer;

    *ArgumentList = NULL;
    argv = ArgumentList;

    CmdLineString = &ProcessParameters->CommandLine;
    if (!CmdLineString->Buffer || !CmdLineString->Length)
        CmdLineString = &ProcessParameters->ImagePathName;

    Source = CmdLineString->Buffer;
    Length = CmdLineString->Length;

    if (Source)
    {
        Destination = RtlAllocateHeap(g_Heap, 0, (Length + 1) * sizeof(WCHAR));
        if (!Destination)
            goto fail;

        while (*Source)
        {
            while (*Source && *Source <= L' ') Source++;

            if (*Source)
            {
                *ArgumentList++ = Destination;
                argc++;
                while (*Source > L' ')
                    *Destination++ = *Source++;
                *Destination++ = L'\0';
            }
        }
    }

    *ArgumentList++ = NULL;
    envp = ArgumentList;

    Status = wmain(argc, argv, envp, 0);

fail:
    NtTerminateProcess(NtCurrentProcess(), Status);
}

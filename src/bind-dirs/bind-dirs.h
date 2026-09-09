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
 * bind-dirs - the Windows counterpart of qubes-bind-dirs (qubes-core-agent-linux,
 * /usr/lib/qubes/init/bind-dirs.sh).
 *
 * Linux                                       Windows
 * /rw (persistent private volume)             Q: (the Qubes private volume)
 * template root, reset every boot             C: (an AppVM's system volume)
 * mount --bind <rw> <ro>                      NTFS directory junction at <ro> -> <rw>
 * /rw/config/qubes-bind-dirs.d/NAME.conf      Q:\config\qubes-bind-dirs.d\NAME.conf
 * /rw/bind-dirs/<path>                        Q:\bind-dirs\<path without "C:">
 * qubes-bind-dirs.service (early boot)        Session Manager BootExecute (before any service)
 *
 * This header is the PORTABLE part: config parsing, path validation, the rw<-ro mapping and
 * the per-entry decision/execution logic, written against a small file-system interface
 * (BD_FS) so it compiles with gcc on Linux for the offline tests (tools/tests/bind-dirs) and
 * with the WDK toolset on Windows, where main.c implements BD_FS over relocate-dir's ntdll
 * I/O layer. It uses only what ntdll exports (wcslen, wcsncmp, memcpy, memset) - no CRT.
 */

#pragma once

#ifdef _WIN32
// The km headers (via relocate-dir's nt.h) declare wchar_t, size_t and the string.h set.
#include "../relocate-dir/nt.h"
#else
#include <stddef.h>
#include <string.h>
#include <wchar.h>
#endif

// Status convention: NTSTATUS on Windows (negative = failure), a negative int in the fake FS.
typedef long BD_STATUS;
#define BD_OK 0
#define BD_FAILED(s) ((s) < 0)

// Fixed limits. A hard-coded cap is deliberate in a BootExecute program: nothing here may
// grow without bound, and a config that needs more than this is a config error, not a
// resource to accommodate.
#define BD_MAX_PATH      1024   // characters, incl. terminator; generous for C:\... paths
#define BD_MAX_ENTRIES   256
#define BD_MAX_CONF_FILES 256
#define BD_MAX_CONF_BYTES (1024 * 1024)

#define BD_RW_ROOT      L"Q:\\bind-dirs"      // Linux: /rw/bind-dirs   (DEFAULT_RW_BIND_DIR)
#define BD_SEEDING_SUFFIX L".qbd-seeding"     // staging dir for a first-use seed in progress
#define BD_ORIG_SUFFIX    L".qbd-orig"        // the original C: directory, moved aside

// What Stat() reports about a path.
typedef struct _BD_STAT
{
    int exists;
    int isDirectory;
    int isReparsePoint;
    unsigned long reparseTag;               // IO_REPARSE_TAG_* when isReparsePoint
    wchar_t reparseTarget[BD_MAX_PATH];     // substitute name as stored (e.g. \??\Q:\bind-dirs\x); empty if unknown
} BD_STAT;

struct _BD_FS;
typedef struct _BD_FS BD_FS;

typedef void (*BD_LIST_CALLBACK)(void *cookie, const wchar_t *name, int isDirectory);

// The file-system interface. Every operation is a single, non-recursive primitive except
// CopyDirectory/DeleteDirectory (recursive by nature). All paths are DOS paths (C:\...).
// Contract for every function returning BD_STATUS: BD_FAILED(status) means NOTHING was
// changed by that call unless documented otherwise; the core relies on this for rollback.
struct _BD_FS
{
    void *context;

    BD_STATUS (*Stat)(void *context, const wchar_t *path, BD_STAT *info);
    // Creates exactly one directory level. Must fail if it already exists.
    BD_STATUS (*CreateDirectory)(void *context, const wchar_t *path);
    // Same-volume rename of a directory. Must fail if newPath exists.
    BD_STATUS (*Rename)(void *context, const wchar_t *oldPath, const wchar_t *newPath);
    // Recursive copy preserving attributes, ACLs and reparse points. STRICT: any child
    // failure fails the call (a partially copied target may be left behind; the core
    // deletes it).
    BD_STATUS (*CopyDirectory)(void *context, const wchar_t *sourcePath, const wchar_t *targetPath);
    // Recursive delete of a directory (contents and the directory itself). A junction is
    // removed as a junction: its target is never entered.
    BD_STATUS (*DeleteDirectory)(void *context, const wchar_t *path);
    // Turns an existing EMPTY directory into a junction to targetPath (a DOS path).
    BD_STATUS (*SetJunction)(void *context, const wchar_t *path, const wchar_t *targetPath);
    // Lists the immediate children of a directory, unordered.
    BD_STATUS (*ListDirectory)(void *context, const wchar_t *path, BD_LIST_CALLBACK callback, void *cookie);
    // Reads a whole file. Buffer is allocated with Alloc; caller frees. Fails above maxBytes.
    BD_STATUS (*ReadFile)(void *context, const wchar_t *path, unsigned char **data, unsigned long *size, unsigned long maxBytes);

    void *(*Alloc)(void *context, size_t size);
    void (*Free)(void *context, void *ptr);
    wchar_t (*Upcase)(void *context, wchar_t c);   // for case-insensitive path compares
    void (*Log)(void *context, const wchar_t *format, ...);
};

// One configured bind, with where it came from (for error messages).
typedef struct _BD_ENTRY
{
    wchar_t path[BD_MAX_PATH];        // normalized: C:\Dir\Sub (no trailing backslash)
    wchar_t source[BD_MAX_PATH];      // "<file>:<line>" of the statement that added it
} BD_ENTRY;

typedef struct _BD_LIST
{
    BD_ENTRY entries[BD_MAX_ENTRIES];
    unsigned long count;
} BD_LIST;

// Outcome of one entry. `result` is one of "ok", "failed"; `reason` is a short stable token
// (see bind-dirs.c) suitable for the machine-readable result record.
typedef struct _BD_ENTRY_RESULT
{
    const wchar_t *result;
    const wchar_t *reason;
    BD_STATUS status;
    int seeded;                       // 1 if this run performed the first-use seed
    int warning;                      // 1 if the bind is complete but a cleanup step failed
    int rollbackFailed;               // 1 if a failed bind could NOT restore the original (see log)
    wchar_t rwPath[BD_MAX_PATH];
} BD_ENTRY_RESULT;

typedef struct _BD_REPORT
{
    unsigned long total;
    unsigned long ok;
    unsigned long failed;
    unsigned long seeded;
    unsigned long warnings;
} BD_REPORT;

// --- config ---------------------------------------------------------------------------
// Parses one .conf file's text (already decoded to wchar_t) and applies its statements to
// `list` in order. Grammar: see bind-dirs.c ("CONFIG GRAMMAR"). Any malformed line FAILS
// the whole parse: the list is left as it was before this file, and `errorLine` /
// `errorText` name the offender. Mirrors `bash -n` + `set -e` on Linux, where a syntax
// error in any .conf aborts the whole bind-dirs run.
BD_STATUS BdParseConfigText(BD_FS *fs, const wchar_t *fileName, const wchar_t *text, BD_LIST *list,
                            unsigned long *errorLine, const wchar_t **errorText);

// Decodes a raw file (UTF-8 with or without BOM, or UTF-16LE with BOM) into a NUL-terminated
// wchar_t string allocated with fs->Alloc. Fails on invalid UTF-8 or a NUL byte.
BD_STATUS BdDecodeText(BD_FS *fs, const unsigned char *data, unsigned long size, wchar_t **text);

// Loads every *.conf in each source directory (lexical order, case-insensitive), in the
// order the directories are given. A missing directory is skipped (Linux: `continue` on
// `[ ! -d "$source_folder" ]`). Any unreadable or malformed file fails the load.
BD_STATUS BdLoadConfig(BD_FS *fs, const wchar_t *const *sourceDirs, unsigned long sourceDirCount, BD_LIST *list);

// --- paths ----------------------------------------------------------------------------
// Validates and normalizes one configured path. Pure (no FS access). Returns BD_OK and the
// normalized path, or a failure with `reason` set to a stable token:
//   not-absolute-c, cross-volume, unc-or-device-path, dot-component, empty-component,
//   bad-character, root, protected, too-long
BD_STATUS BdValidatePath(BD_FS *fs, const wchar_t *input, wchar_t *normalized, size_t capacity, const wchar_t **reason);

// C:\Dir\Sub -> Q:\bind-dirs\Dir\Sub (Linux: rw_from_ro). `input` must be normalized.
BD_STATUS BdRwFromRo(const wchar_t *ro, wchar_t *rw, size_t capacity);

// Case-insensitive path equality using fs->Upcase.
int BdPathEquals(BD_FS *fs, const wchar_t *a, const wchar_t *b);

// --- execution ------------------------------------------------------------------------
// Binds one entry. Never leaves the path half-converted: on any failure the original
// directory is back where it was (or was never touched) and `out` says why.
void BdApplyEntry(BD_FS *fs, const wchar_t *ro, BD_ENTRY_RESULT *out);

// Ensures Q:\bind-dirs exists (Linux: init() -> mkdir --parents "$rw_dest_dir").
BD_STATUS BdInitRwRoot(BD_FS *fs);

// Full run over a list: validate, dedupe, apply each, aggregate. `results` must hold
// list->count elements. Returns BD_OK only if every entry ended "ok".
BD_STATUS BdRun(BD_FS *fs, const BD_LIST *list, BD_ENTRY_RESULT *results, BD_REPORT *report);

#define BD_IO_REPARSE_TAG_MOUNT_POINT 0xA0000003UL
#define BD_IO_REPARSE_TAG_SYMLINK     0xA000000CUL

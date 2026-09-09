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
 * bind-dirs core: portable logic (see bind-dirs.h for the Linux <-> Windows mapping).
 *
 * Every place this deliberately differs from bind-dirs.sh is marked "DIFFERS FROM LINUX"
 * with the reason. The full list is in docs/BIND-DIRS.md.
 */

#include "bind-dirs.h"

#define BD_STATUS_FAIL   (-1L)   // generic failure raised by the core itself (not by the FS)

// ------------------------------------------------------------------ string helpers
// Only wcslen/wcsncmp/memcpy/memset are used: those are what relocate-dir already links from ntdll.

static size_t BdLen(const wchar_t *s)
{
    return wcslen(s);
}

// Copies src into dst[capacity]; fails (dst untouched) if it does not fit.
static int BdCopy(wchar_t *dst, size_t capacity, const wchar_t *src)
{
    size_t len = BdLen(src);

    if (len + 1 > capacity)
        return -1;
    memcpy(dst, src, (len + 1) * sizeof(wchar_t));
    return 0;
}

static int BdAppend(wchar_t *dst, size_t capacity, const wchar_t *src)
{
    size_t have = BdLen(dst);
    size_t len = BdLen(src);

    if (have + len + 1 > capacity)
        return -1;
    memcpy(dst + have, src, (len + 1) * sizeof(wchar_t));
    return 0;
}

static wchar_t BdUp(BD_FS *fs, wchar_t c)
{
    if (c >= L'a' && c <= L'z')
        return (wchar_t)(c - L'a' + L'A');
    if (c < 0x80)
        return c;
    return fs->Upcase(fs->context, c);
}

int BdPathEquals(BD_FS *fs, const wchar_t *a, const wchar_t *b)
{
    while (*a && *b)
    {
        if (BdUp(fs, *a) != BdUp(fs, *b))
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

// TRUE if `path` is strictly below `prefix` (prefix = C:\a, path = C:\a\b...). Both normalized.
static int BdPathIsBelow(BD_FS *fs, const wchar_t *path, const wchar_t *prefix)
{
    size_t plen = BdLen(prefix);
    size_t i;

    if (BdLen(path) <= plen)
        return 0;
    for (i = 0; i < plen; i++)
    {
        if (BdUp(fs, path[i]) != BdUp(fs, prefix[i]))
            return 0;
    }
    return path[plen] == L'\\';
}

static int BdEqualsAscii(BD_FS *fs, const wchar_t *s, size_t len, const wchar_t *literal)
{
    size_t i;

    for (i = 0; i < len; i++)
    {
        if (!literal[i] || BdUp(fs, s[i]) != BdUp(fs, literal[i]))
            return 0;
    }
    return literal[len] == L'\0';
}

static int BdEndsWithNoCase(BD_FS *fs, const wchar_t *s, const wchar_t *suffix)
{
    size_t slen = BdLen(s), xlen = BdLen(suffix);

    if (xlen > slen)
        return 0;
    return BdEqualsAscii(fs, s + slen - xlen, xlen, suffix);
}

static int BdIsSpace(wchar_t c)
{
    return c == L' ' || c == L'\t' || c == L'\r';
}

// Exact (case-sensitive) equality, for result/reason tokens. Never compare literal
// pointers: identical literals are not guaranteed to share an address.
static int BdStrEq(const wchar_t *a, const wchar_t *b)
{
    while (*a && *a == *b)
    {
        a++;
        b++;
    }
    return *a == *b;
}

// ------------------------------------------------------------------ path validation

// Components that may never be bound. `Windows` and `Program Files\Qubes Tools` are refused
// as whole subtrees (the OS and the tools this program is part of); the other roots are
// refused exactly (their contents may be bound, e.g. C:\ProgramData\SomeApp).
//
// C:\Users is MoveUsers' (relocate-dir.exe replaces it with a link onto Q:\Users). Anything
// under it is then already persistent, and is refused at run time by the reparse-ancestor
// check rather than here, so that a guest installed with -NoMoveUsers can still bind a
// profile subdirectory. DIFFERS FROM LINUX: bind-dirs.sh special-cases /home and /usr/local
// into /rw/home and /rw/usrlocal; the Windows profile tree needs no such mapping because
// MoveUsers owns it wholesale.
static const wchar_t *const g_ProtectedSubtrees[] = {
    L"Windows",
    L"Program Files\\Qubes Tools",
};

static const wchar_t *const g_ProtectedRoots[] = {
    L"Users",
    L"Program Files",
    L"Program Files (x86)",
    L"ProgramData",
    L"Recovery",
    L"System Volume Information",
    L"$Recycle.Bin",
};

BD_STATUS BdValidatePath(BD_FS *fs, const wchar_t *input, wchar_t *normalized, size_t capacity, const wchar_t **reason)
{
    wchar_t buffer[BD_MAX_PATH];
    size_t len, i, start, out;
    unsigned long components = 0;
    unsigned long k;

    *reason = L"";
    len = BdLen(input);
    if (len == 0)
    {
        *reason = L"not-absolute-c";
        return BD_STATUS_FAIL;
    }
    if (len + 1 > sizeof(buffer) / sizeof(buffer[0]) || len + 1 > capacity)
    {
        *reason = L"too-long";
        return BD_STATUS_FAIL;
    }

    // Forward slashes are accepted and normalized (NT accepts them in DOS paths too).
    for (i = 0; i < len; i++)
        buffer[i] = (input[i] == L'/') ? L'\\' : input[i];
    buffer[len] = L'\0';

    if (buffer[0] == L'\\')
    {
        // \\server\share, \\?\..., \\.\..., or a rootless \path
        *reason = (len > 1 && buffer[1] == L'\\') ? L"unc-or-device-path" : L"not-absolute-c";
        return BD_STATUS_FAIL;
    }

    if (len < 2 || buffer[1] != L':')
    {
        *reason = L"not-absolute-c";
        return BD_STATUS_FAIL;
    }

    if (BdUp(fs, buffer[0]) != L'C')
    {
        // Q: itself (binding the private volume into itself), the tools CD, anything else.
        *reason = L"cross-volume";
        return BD_STATUS_FAIL;
    }

    if (len == 2 || (len == 3 && buffer[2] == L'\\'))
    {
        *reason = L"root";
        return BD_STATUS_FAIL;
    }

    if (buffer[2] != L'\\')
    {
        // C:relative
        *reason = L"not-absolute-c";
        return BD_STATUS_FAIL;
    }

    // Strip exactly one trailing backslash (C:\Foo\ is C:\Foo). A second one is an empty
    // component and is refused below.
    if (buffer[len - 1] == L'\\')
    {
        buffer[len - 1] = L'\0';
        len--;
        if (len == 3)
        {
            // C:\\ - the root spelled with a doubled separator.
            *reason = L"root";
            return BD_STATUS_FAIL;
        }
    }

    // Walk components after "C:\".
    normalized[0] = L'C';
    normalized[1] = L':';
    out = 2;
    start = 3;
    for (i = 3; i <= len; i++)
    {
        if (i == len || buffer[i] == L'\\')
        {
            size_t clen = i - start;
            size_t j;

            if (clen == 0)
            {
                *reason = L"empty-component";
                return BD_STATUS_FAIL;
            }
            if ((clen == 1 && buffer[start] == L'.') || (clen == 2 && buffer[start] == L'.' && buffer[start + 1] == L'.'))
            {
                *reason = L"dot-component";
                return BD_STATUS_FAIL;
            }
            for (j = start; j < i; j++)
            {
                wchar_t c = buffer[j];
                if (c < 0x20 || c == L'<' || c == L'>' || c == L':' || c == L'"' || c == L'|' || c == L'?' || c == L'*')
                {
                    *reason = L"bad-character";
                    return BD_STATUS_FAIL;
                }
            }
            // Win32 silently strips trailing spaces/dots, the NT API does not: such a name
            // would be bound under a different name than Explorer shows. Refuse.
            if (buffer[i - 1] == L' ' || buffer[i - 1] == L'.')
            {
                *reason = L"bad-character";
                return BD_STATUS_FAIL;
            }
            {
                // Would collide with our own staging/backup names. Check the component
                // alone (temporarily terminate it).
                wchar_t saved = buffer[i];
                int reserved;
                buffer[i] = L'\0';
                reserved = BdEndsWithNoCase(fs, buffer + start, BD_SEEDING_SUFFIX) ||
                           BdEndsWithNoCase(fs, buffer + start, BD_ORIG_SUFFIX);
                buffer[i] = saved;
                if (reserved)
                {
                    *reason = L"reserved-suffix";
                    return BD_STATUS_FAIL;
                }
            }

            normalized[out++] = L'\\';
            memcpy(normalized + out, buffer + start, clen * sizeof(wchar_t));
            out += clen;
            components++;
            start = i + 1;
        }
    }
    normalized[out] = L'\0';

    if (components == 0)
    {
        *reason = L"root";
        return BD_STATUS_FAIL;
    }

    // Protected locations.
    for (k = 0; k < sizeof(g_ProtectedSubtrees) / sizeof(g_ProtectedSubtrees[0]); k++)
    {
        wchar_t full[BD_MAX_PATH];
        BdCopy(full, BD_MAX_PATH, L"C:\\");
        BdAppend(full, BD_MAX_PATH, g_ProtectedSubtrees[k]);
        if (BdPathEquals(fs, normalized, full) || BdPathIsBelow(fs, normalized, full))
        {
            *reason = L"protected";
            return BD_STATUS_FAIL;
        }
    }
    for (k = 0; k < sizeof(g_ProtectedRoots) / sizeof(g_ProtectedRoots[0]); k++)
    {
        wchar_t full[BD_MAX_PATH];
        BdCopy(full, BD_MAX_PATH, L"C:\\");
        BdAppend(full, BD_MAX_PATH, g_ProtectedRoots[k]);
        if (BdPathEquals(fs, normalized, full))
        {
            *reason = L"protected";
            return BD_STATUS_FAIL;
        }
    }

    return BD_OK;
}

BD_STATUS BdRwFromRo(const wchar_t *ro, wchar_t *rw, size_t capacity)
{
    // Linux: rw="${rw_dest_dir}${ro}"  ->  Q:\bind-dirs + <ro without the "C:" prefix>
    if (BdCopy(rw, capacity, BD_RW_ROOT) != 0)
        return BD_STATUS_FAIL;
    if (BdAppend(rw, capacity, ro + 2) != 0)
        return BD_STATUS_FAIL;
    return BD_OK;
}

// ------------------------------------------------------------------ text decoding

BD_STATUS BdDecodeText(BD_FS *fs, const unsigned char *data, unsigned long size, wchar_t **text)
{
    wchar_t *out;
    unsigned long i, n = 0;

    *text = NULL;

    // Worst case: one wchar per byte (+ surrogate pairs never exceed that on 16-bit wchar_t
    // because a 4-byte sequence yields 2 units).
    out = fs->Alloc(fs->context, ((size_t)size + 1) * sizeof(wchar_t));
    if (!out)
        return BD_STATUS_FAIL;

    if (size >= 2 && data[0] == 0xFF && data[1] == 0xFE)
    {
        // UTF-16LE with BOM.
        if (size % 2 != 0)
            goto fail;
        for (i = 2; i + 1 < size; i += 2)
        {
            unsigned long u = (unsigned long)data[i] | ((unsigned long)data[i + 1] << 8);
            if (u == 0)
                goto fail;
            if (sizeof(wchar_t) == 2)
            {
                out[n++] = (wchar_t)u;
            }
            else
            {
                // Combine surrogates for a 32-bit wchar_t host (the Linux test build).
                if (u >= 0xD800 && u <= 0xDBFF && i + 3 < size)
                {
                    unsigned long lo = (unsigned long)data[i + 2] | ((unsigned long)data[i + 3] << 8);
                    if (lo >= 0xDC00 && lo <= 0xDFFF)
                    {
                        u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                        i += 2;
                    }
                }
                out[n++] = (wchar_t)u;
            }
        }
        out[n] = L'\0';
        *text = out;
        return BD_OK;
    }

    i = 0;
    if (size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF)
        i = 3;

    while (i < size)
    {
        unsigned long cp;
        unsigned char b = data[i];
        unsigned long need;

        if (b == 0)
            goto fail;
        if (b < 0x80)
        {
            cp = b;
            need = 0;
        }
        else if ((b & 0xE0) == 0xC0)
        {
            cp = b & 0x1F;
            need = 1;
            if (cp < 2)
                goto fail;   // overlong
        }
        else if ((b & 0xF0) == 0xE0)
        {
            cp = b & 0x0F;
            need = 2;
        }
        else if ((b & 0xF8) == 0xF0)
        {
            cp = b & 0x07;
            need = 3;
        }
        else
        {
            goto fail;
        }

        {
            unsigned long k;
            // Continuation bytes data[i+1 .. i+need] must all exist.
            if (need > 0 && i + need >= size)
                goto fail;
            for (k = 1; k <= need; k++)
            {
                unsigned char c = data[i + k];
                if ((c & 0xC0) != 0x80)
                    goto fail;
                cp = (cp << 6) | (c & 0x3F);
            }
        }
        if ((need == 2 && cp < 0x800) || (need == 3 && cp < 0x10000) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            goto fail;
        i += need + 1;

        if (sizeof(wchar_t) == 2 && cp >= 0x10000)
        {
            cp -= 0x10000;
            out[n++] = (wchar_t)(0xD800 + (cp >> 10));
            out[n++] = (wchar_t)(0xDC00 + (cp & 0x3FF));
        }
        else
        {
            out[n++] = (wchar_t)cp;
        }
    }
    out[n] = L'\0';
    *text = out;
    return BD_OK;

fail:
    fs->Free(fs->context, out);
    return BD_STATUS_FAIL;
}

// ------------------------------------------------------------------ config parsing
/*
 * CONFIG GRAMMAR - the subset of bash that bind-dirs.sh configs actually use, so a file
 * written for the Linux feature reads the same here (with Windows paths):
 *
 *   # comment                                  blank lines and comments ignored
 *   binds+=( 'C:\ProgramData\Foo' "C:\x" )     append (the documented Linux form)
 *   binds=( 'C:\a' 'C:\b' )                    replace the list (bash semantics)
 *   binds=( "${binds[@]/'C:\ProgramData\Foo'}" )   remove an earlier entry - the removal
 *                                              idiom from https://www.qubes-os.org/doc/bind-dirs/
 *   binds=()                                   clear
 *
 * One statement per line; a trailing `# comment` is allowed. Elements are single-quoted
 * (literal), double-quoted (bash escapes \\ \" \$ \` honoured; any other $ or ` rejected
 * because no expansion is supported) or bare. A bare element may not contain a backslash:
 * in bash a bare backslash is an escape, so `binds+=( C:\Foo )` on Linux would have read
 * "C:Foo" - quote the path instead. Anything else is a syntax error, and like `bash -n`
 * + `set -e` on Linux one syntax error aborts the whole run.
 *
 * DIFFERS FROM LINUX: the removal idiom is bash substring substitution (it blanks the
 * pattern inside every element); here it removes elements EQUAL to the path. That is the
 * only use the documentation gives it and the only one that makes sense for a path list.
 */

typedef struct _BD_PARSER
{
    BD_FS *fs;
    const wchar_t *fileName;
    unsigned long line;
    const wchar_t *error;
} BD_PARSER;

static void BdListRemove(BD_FS *fs, BD_LIST *list, const wchar_t *path)
{
    unsigned long i = 0;

    while (i < list->count)
    {
        if (BdPathEquals(fs, list->entries[i].path, path))
        {
            // Shift down by struct assignment: memmove is the one string.h function the
            // native build has not been seen to link from ntdll, so it is not used.
            unsigned long k;
            for (k = i; k + 1 < list->count; k++)
                list->entries[k] = list->entries[k + 1];
            list->count--;
        }
        else
        {
            i++;
        }
    }
}

static int BdListAdd(BD_PARSER *p, BD_LIST *list, const wchar_t *path)
{
    BD_ENTRY *e;
    wchar_t lineText[32];
    unsigned long v = p->line;
    int n = 0, i;
    wchar_t tmp[16];

    if (list->count >= BD_MAX_ENTRIES)
    {
        p->error = L"too-many-entries";
        return -1;
    }
    e = &list->entries[list->count];
    if (BdCopy(e->path, BD_MAX_PATH, path) != 0)
    {
        p->error = L"too-long";
        return -1;
    }
    // source = "<file>:<line>" without printf.
    do
    {
        tmp[n++] = (wchar_t)(L'0' + v % 10);
        v /= 10;
    } while (v && n < 15);
    for (i = 0; i < n; i++)
        lineText[i] = tmp[n - 1 - i];
    lineText[n] = L'\0';
    BdCopy(e->source, BD_MAX_PATH, L"");
    if (BdCopy(e->source, BD_MAX_PATH, p->fileName) != 0 || BdAppend(e->source, BD_MAX_PATH, L":") != 0 ||
        BdAppend(e->source, BD_MAX_PATH, lineText) != 0)
    {
        BdCopy(e->source, BD_MAX_PATH, L"?");
    }
    list->count++;
    return 0;
}

// Stores the element's text in `path` (normalized when valid; raw otherwise, so the run
// can fail it with the right reason). Returns 0 or -1 with p->error set.
static int BdStoreElement(BD_PARSER *p, const wchar_t *raw, wchar_t *path)
{
    const wchar_t *reason;

    if (BdLen(raw) == 0)
    {
        p->error = L"empty-element";
        return -1;
    }
    if (BD_FAILED(BdValidatePath(p->fs, raw, path, BD_MAX_PATH, &reason)))
    {
        if (BdCopy(path, BD_MAX_PATH, raw) != 0)
        {
            p->error = L"too-long";
            return -1;
        }
    }
    return 0;
}

// Parses one element starting at *cursor (not whitespace, not ')'). Writes the element
// text to `out`, sets *isRemoval for the idiom. Advances *cursor past the element.
static int BdParseElement(BD_PARSER *p, const wchar_t **cursor, wchar_t *out, size_t capacity, int *isRemoval)
{
    const wchar_t *s = *cursor;
    size_t n = 0;

    *isRemoval = 0;
    out[0] = L'\0';

    if (*s == L'\'')
    {
        s++;
        while (*s && *s != L'\'')
        {
            if (n + 1 >= capacity) { p->error = L"too-long"; return -1; }
            out[n++] = *s++;
        }
        if (*s != L'\'') { p->error = L"unterminated-quote"; return -1; }
        s++;
    }
    else if (*s == L'"')
    {
        static const wchar_t idiom[] = L"${binds[@]/";
        s++;
        if (wcsncmp(s, idiom, BdLen(idiom)) == 0)
        {
            // "${binds[@]/'PATH'}"  or  "${binds[@]/PATH}"
            wchar_t quote = 0;
            s += BdLen(idiom);
            if (*s == L'\'') { quote = L'\''; s++; }
            while (*s && *s != (quote ? quote : L'}'))
            {
                if (n + 1 >= capacity) { p->error = L"too-long"; return -1; }
                out[n++] = *s++;
            }
            if (quote)
            {
                if (*s != quote) { p->error = L"unterminated-quote"; return -1; }
                s++;
            }
            if (*s != L'}') { p->error = L"malformed-removal"; return -1; }
            s++;
            if (*s != L'"') { p->error = L"malformed-removal"; return -1; }
            s++;
            *isRemoval = 1;
        }
        else
        {
            while (*s && *s != L'"')
            {
                wchar_t c = *s;
                if (c == L'\\' && (s[1] == L'\\' || s[1] == L'"' || s[1] == L'$' || s[1] == L'`'))
                {
                    c = s[1];
                    s++;
                }
                else if (c == L'$' || c == L'`')
                {
                    p->error = L"expansion-unsupported";
                    return -1;
                }
                if (n + 1 >= capacity) { p->error = L"too-long"; return -1; }
                out[n++] = c;
                s++;
            }
            if (*s != L'"') { p->error = L"unterminated-quote"; return -1; }
            s++;
        }
    }
    else
    {
        // bare
        while (*s && !BdIsSpace(*s) && *s != L')' && *s != L'\n')
        {
            wchar_t c = *s;
            if (c == L'\'' || c == L'"' || c == L'(' || c == L'#')
            {
                p->error = L"bad-token";
                return -1;
            }
            if (c == L'\\')
            {
                p->error = L"bare-backslash-quote-the-path";
                return -1;
            }
            if (c == L'$' || c == L'`')
            {
                p->error = L"expansion-unsupported";
                return -1;
            }
            if (n + 1 >= capacity) { p->error = L"too-long"; return -1; }
            out[n++] = c;
            s++;
        }
    }
    out[n] = L'\0';

    // An element must be followed by whitespace or the closing paren.
    if (*s && !BdIsSpace(*s) && *s != L')' && *s != L'\n')
    {
        p->error = L"bad-token";
        return -1;
    }
    *cursor = s;
    return 0;
}

// Parses one logical line. Returns 0 or -1 with p->error set. Mutates `list` only on success
// of the whole statement (elements are staged first).
static int BdParseLine(BD_PARSER *p, const wchar_t *line, BD_LIST *list)
{
    static const wchar_t keyword[] = L"binds";
    const wchar_t *s = line;
    int append;
    BD_LIST *staged;
    int rc = -1;

    while (BdIsSpace(*s))
        s++;
    if (*s == L'\0' || *s == L'\n' || *s == L'#')
        return 0;

    if (wcsncmp(s, keyword, BdLen(keyword)) != 0)
    {
        p->error = L"unknown-statement";
        return -1;
    }
    s += BdLen(keyword);
    if (s[0] == L'+' && s[1] == L'=')
    {
        append = 1;
        s += 2;
    }
    else if (s[0] == L'=')
    {
        append = 0;
        s += 1;
    }
    else
    {
        p->error = L"unknown-statement";
        return -1;
    }
    if (*s != L'(')
    {
        p->error = L"expected-open-paren";
        return -1;
    }
    s++;

    // Stage into a copy so a syntax error later on the line leaves `list` untouched.
    staged = p->fs->Alloc(p->fs->context, sizeof(BD_LIST));
    if (!staged)
    {
        p->error = L"out-of-memory";
        return -1;
    }
    if (append)
        memcpy(staged, list, sizeof(BD_LIST));
    else
        staged->count = 0;

    for (;;)
    {
        wchar_t element[BD_MAX_PATH];
        wchar_t path[BD_MAX_PATH];
        int isRemoval;

        while (BdIsSpace(*s))
            s++;
        if (*s == L')')
            break;
        if (*s == L'\0' || *s == L'\n')
        {
            p->error = L"statement-incomplete";
            goto cleanup;
        }
        if (BdParseElement(p, &s, element, BD_MAX_PATH, &isRemoval) != 0)
            goto cleanup;
        if (BdStoreElement(p, element, path) != 0)
            goto cleanup;

        if (isRemoval)
        {
            if (append)
            {
                // binds+=( "${binds[@]/x}" ) would DUPLICATE the list in bash. Nobody means that.
                p->error = L"removal-needs-assignment";
                goto cleanup;
            }
            // binds=( "${binds[@]/'x'}" ): the current list minus x. Bash evaluates the
            // expansion against the list as it was before this statement.
            memcpy(staged, list, sizeof(BD_LIST));
            BdListRemove(p->fs, staged, path);
        }
        else
        {
            if (BdListAdd(p, staged, path) != 0)
                goto cleanup;
        }
    }
    s++; // ')'

    while (BdIsSpace(*s))
        s++;
    if (*s != L'\0' && *s != L'\n' && *s != L'#')
    {
        p->error = L"trailing-garbage";
        goto cleanup;
    }

    memcpy(list, staged, sizeof(BD_LIST));
    rc = 0;

cleanup:
    p->fs->Free(p->fs->context, staged);
    return rc;
}

BD_STATUS BdParseConfigText(BD_FS *fs, const wchar_t *fileName, const wchar_t *text, BD_LIST *list,
                            unsigned long *errorLine, const wchar_t **errorText)
{
    BD_PARSER p;
    BD_LIST *work;
    const wchar_t *s = text;
    BD_STATUS status = BD_STATUS_FAIL;

    p.fs = fs;
    p.fileName = fileName;
    p.line = 0;
    p.error = L"";
    *errorLine = 0;
    *errorText = L"";

    // Whole-file staging: a syntax error anywhere leaves `list` as it was before this file.
    work = fs->Alloc(fs->context, sizeof(BD_LIST));
    if (!work)
    {
        *errorText = L"out-of-memory";
        return BD_STATUS_FAIL;
    }
    memcpy(work, list, sizeof(BD_LIST));

    while (*s)
    {
        wchar_t lineBuffer[BD_MAX_PATH * 2];
        size_t n = 0;

        p.line++;
        while (*s && *s != L'\n')
        {
            if (n + 1 >= sizeof(lineBuffer) / sizeof(lineBuffer[0]))
            {
                *errorLine = p.line;
                *errorText = L"line-too-long";
                goto cleanup;
            }
            lineBuffer[n++] = *s++;
        }
        lineBuffer[n] = L'\0';
        if (*s == L'\n')
            s++;

        if (BdParseLine(&p, lineBuffer, work) != 0)
        {
            *errorLine = p.line;
            *errorText = p.error;
            goto cleanup;
        }
    }

    memcpy(list, work, sizeof(BD_LIST));
    status = BD_OK;

cleanup:
    fs->Free(fs->context, work);
    return status;
}

// ------------------------------------------------------------------ config loading

typedef struct _BD_NAME_LIST
{
    BD_FS *fs;
    wchar_t (*names)[BD_MAX_PATH];
    unsigned long count;
    int overflow;
} BD_NAME_LIST;

static void BdCollectConf(void *cookie, const wchar_t *name, int isDirectory)
{
    BD_NAME_LIST *nl = cookie;

    if (isDirectory)
        return;
    if (!BdEndsWithNoCase(nl->fs, name, L".conf"))
        return;
    if (nl->count >= BD_MAX_CONF_FILES)
    {
        nl->overflow = 1;
        return;
    }
    if (BdCopy(nl->names[nl->count], BD_MAX_PATH, name) != 0)
    {
        nl->overflow = 1;
        return;
    }
    nl->count++;
}

static int BdCompareNoCase(BD_FS *fs, const wchar_t *a, const wchar_t *b)
{
    while (*a && *b)
    {
        wchar_t x = BdUp(fs, *a), y = BdUp(fs, *b);
        if (x != y)
            return x < y ? -1 : 1;
        a++;
        b++;
    }
    if (*a == *b)
        return 0;
    return *a ? 1 : -1;
}

// Lexical order, case-insensitive. Linux: the shell glob "$source_folder/"*".conf" sorted by
// the C/locale collation; NTFS names are case-insensitive so this is the closest equivalent.
static void BdSortNames(BD_NAME_LIST *nl)
{
    unsigned long i, j;
    wchar_t tmp[BD_MAX_PATH];

    for (i = 1; i < nl->count; i++)
    {
        memcpy(tmp, nl->names[i], sizeof(tmp));
        j = i;
        while (j > 0 && BdCompareNoCase(nl->fs, nl->names[j - 1], tmp) > 0)
        {
            memcpy(nl->names[j], nl->names[j - 1], sizeof(tmp));
            j--;
        }
        memcpy(nl->names[j], tmp, sizeof(tmp));
    }
}

BD_STATUS BdLoadConfig(BD_FS *fs, const wchar_t *const *sourceDirs, unsigned long sourceDirCount, BD_LIST *list)
{
    unsigned long d, i;
    BD_STATUS status = BD_STATUS_FAIL;
    BD_NAME_LIST nl;

    nl.fs = fs;
    nl.names = fs->Alloc(fs->context, sizeof(wchar_t) * BD_MAX_PATH * BD_MAX_CONF_FILES);
    if (!nl.names)
        return BD_STATUS_FAIL;

    for (d = 0; d < sourceDirCount; d++)
    {
        BD_STAT st;
        const wchar_t *dir = sourceDirs[d];

        if (BD_FAILED(fs->Stat(fs->context, dir, &st)) || !st.exists || !st.isDirectory)
        {
            fs->Log(fs->context, L"[*] config: %ls: not present, skipped\n", dir);
            continue;
        }

        nl.count = 0;
        nl.overflow = 0;
        status = fs->ListDirectory(fs->context, dir, BdCollectConf, &nl);
        if (BD_FAILED(status))
        {
            fs->Log(fs->context, L"[!] config: cannot list %ls: 0x%08lx\n", dir, (unsigned long)status);
            goto cleanup;
        }
        if (nl.overflow)
        {
            fs->Log(fs->context, L"[!] config: %ls: more than %lu .conf files, refusing\n", dir, (unsigned long)BD_MAX_CONF_FILES);
            status = BD_STATUS_FAIL;
            goto cleanup;
        }
        BdSortNames(&nl);

        for (i = 0; i < nl.count; i++)
        {
            wchar_t path[BD_MAX_PATH];
            unsigned char *data = NULL;
            unsigned long size = 0;
            wchar_t *text = NULL;
            unsigned long errorLine;
            const wchar_t *errorText;

            if (BdCopy(path, BD_MAX_PATH, dir) != 0 || BdAppend(path, BD_MAX_PATH, L"\\") != 0 ||
                BdAppend(path, BD_MAX_PATH, nl.names[i]) != 0)
            {
                fs->Log(fs->context, L"[!] config: path too long under %ls\n", dir);
                status = BD_STATUS_FAIL;
                goto cleanup;
            }

            status = fs->ReadFile(fs->context, path, &data, &size, BD_MAX_CONF_BYTES);
            if (BD_FAILED(status))
            {
                fs->Log(fs->context, L"[!] config: cannot read %ls: 0x%08lx\n", path, (unsigned long)status);
                goto cleanup;
            }
            status = BdDecodeText(fs, data, size, &text);
            fs->Free(fs->context, data);
            if (BD_FAILED(status))
            {
                fs->Log(fs->context, L"[!] config: %ls is not valid UTF-8/UTF-16 text\n", path);
                goto cleanup;
            }
            status = BdParseConfigText(fs, path, text, list, &errorLine, &errorText);
            fs->Free(fs->context, text);
            if (BD_FAILED(status))
            {
                fs->Log(fs->context, L"[!] config: %ls:%lu: %ls\n", path, errorLine, errorText);
                goto cleanup;
            }
            fs->Log(fs->context, L"[*] config: %ls: %lu entr%ls after this file\n", path, list->count,
                    list->count == 1 ? L"y" : L"ies");
        }
    }
    status = BD_OK;

cleanup:
    fs->Free(fs->context, nl.names);
    return status;
}

// ------------------------------------------------------------------ execution

static int BdTargetMatches(BD_FS *fs, const BD_STAT *st, const wchar_t *rw)
{
    const wchar_t *t = st->reparseTarget;

    if (st->reparseTag != BD_IO_REPARSE_TAG_MOUNT_POINT)
        return 0;
    if (wcsncmp(t, L"\\??\\", 4) == 0)
        t += 4;
    else if (wcsncmp(t, L"\\\\?\\", 4) == 0)
        t += 4;
    return BdPathEquals(fs, t, rw);
}

// Parent of a normalized path into `parent`; returns 0, or -1 for a first-level path (C:\x).
static int BdParent(const wchar_t *path, wchar_t *parent, size_t capacity)
{
    size_t len = BdLen(path);
    size_t i;

    for (i = len; i > 3; i--)
    {
        if (path[i - 1] == L'\\')
        {
            if (i - 1 + 1 > capacity)
                return -1;
            memcpy(parent, path, (i - 1) * sizeof(wchar_t));
            parent[i - 1] = L'\0';
            return 0;
        }
    }
    return -1;
}

// TRUE if any ancestor of `path` (excluding itself, down to but excluding C:\) is a
// reparse point. Reparse points in intermediate components are ALWAYS followed by the NT
// API, so a junction created "at" C:\Users\x after MoveUsers would land on Q:\Users\x -
// a junction on the private volume pointing into the private volume.
static BD_STATUS BdAncestorIsReparse(BD_FS *fs, const wchar_t *path, int *isReparse, wchar_t *which)
{
    wchar_t current[BD_MAX_PATH];
    wchar_t parent[BD_MAX_PATH];
    BD_STAT st;
    BD_STATUS status;

    *isReparse = 0;
    which[0] = L'\0';
    BdCopy(current, BD_MAX_PATH, path);
    while (BdParent(current, parent, BD_MAX_PATH) == 0)
    {
        status = fs->Stat(fs->context, parent, &st);
        if (BD_FAILED(status))
            return status;
        if (st.exists && st.isReparsePoint)
        {
            *isReparse = 1;
            BdCopy(which, BD_MAX_PATH, parent);
            return BD_OK;
        }
        BdCopy(current, BD_MAX_PATH, parent);
    }
    return BD_OK;
}

// Creates the missing ancestors of `path` (not `path` itself). Linux: mk_parent_dirs.
static BD_STATUS BdEnsureParents(BD_FS *fs, const wchar_t *path)
{
    wchar_t parent[BD_MAX_PATH];
    size_t len, i;
    BD_STAT st;
    BD_STATUS status;

    if (BdParent(path, parent, BD_MAX_PATH) != 0)
        return BD_OK;   // first-level: the volume root is the parent

    len = BdLen(parent);
    // Walk forward from the first component so each level is created in order.
    for (i = 3; i <= len; i++)
    {
        if (i == len || parent[i] == L'\\')
        {
            wchar_t prefix[BD_MAX_PATH];
            memcpy(prefix, parent, i * sizeof(wchar_t));
            prefix[i] = L'\0';
            status = fs->Stat(fs->context, prefix, &st);
            if (BD_FAILED(status))
                return status;
            if (!st.exists)
            {
                status = fs->CreateDirectory(fs->context, prefix);
                if (BD_FAILED(status))
                    return status;
                fs->Log(fs->context, L"[*] created %ls\n", prefix);
            }
            else if (!st.isDirectory || st.isReparsePoint)
            {
                return BD_STATUS_FAIL;
            }
        }
    }
    return BD_OK;
}

static void BdFail(BD_ENTRY_RESULT *out, const wchar_t *reason, BD_STATUS status)
{
    out->result = L"failed";
    out->reason = reason;
    out->status = status;
}

void BdApplyEntry(BD_FS *fs, const wchar_t *ro, BD_ENTRY_RESULT *out)
{
    wchar_t rw[BD_MAX_PATH];
    wchar_t staging[BD_MAX_PATH];
    wchar_t orig[BD_MAX_PATH];
    wchar_t which[BD_MAX_PATH];
    BD_STAT sRo, sRw, st;
    BD_STATUS status;
    int ancestorReparse = 0;
    int movedAside = 0;

    memset(out, 0, sizeof(*out));
    out->result = L"failed";
    out->reason = L"unknown";

    if (BD_FAILED(BdRwFromRo(ro, rw, BD_MAX_PATH)))
    {
        BdFail(out, L"too-long", BD_STATUS_FAIL);
        return;
    }
    BdCopy(out->rwPath, BD_MAX_PATH, rw);

    fs->Log(fs->context, L"[*] %ls -> %ls\n", ro, rw);

    status = fs->Stat(fs->context, ro, &sRo);
    if (BD_FAILED(status))
    {
        BdFail(out, L"stat-source", status);
        return;
    }

    // --- already a reparse point? (idempotence, and the foreign-link refusal) ---
    if (sRo.exists && sRo.isReparsePoint)
    {
        if (!BdTargetMatches(fs, &sRo, rw))
        {
            // A symlink/junction that is not ours: following it would bind whatever it
            // points at (possibly another volume) under this name. DIFFERS FROM LINUX:
            // bind-dirs.sh resolves symlinks (up to 10 levels) and binds the target.
            fs->Log(fs->context, L"[!] %ls is a reparse point (tag 0x%08lx -> '%ls') that is not ours, refusing\n",
                    ro, sRo.reparseTag, sRo.reparseTarget);
            BdFail(out, L"foreign-reparse-point", BD_STATUS_FAIL);
            return;
        }
        status = fs->Stat(fs->context, rw, &sRw);
        if (BD_FAILED(status))
        {
            BdFail(out, L"stat-target", status);
            return;
        }
        if (!sRw.exists || !sRw.isDirectory || sRw.isReparsePoint)
        {
            // Our junction, but it dangles: someone removed Q:\bind-dirs\... Do NOT
            // "repair" by re-seeding (that would resurrect template content over what the
            // user deleted on purpose, or hide a broken private volume).
            fs->Log(fs->context, L"[!] %ls is already our junction but %ls is missing or not a directory\n", ro, rw);
            BdFail(out, L"target-missing", BD_STATUS_FAIL);
            return;
        }
        fs->Log(fs->context, L"[*] %ls already bound, nothing to do\n", ro);
        out->result = L"ok";
        out->reason = L"already-bound";
        out->status = BD_OK;
        return;
    }

    status = BdAncestorIsReparse(fs, ro, &ancestorReparse, which);
    if (BD_FAILED(status))
    {
        BdFail(out, L"stat-ancestor", status);
        return;
    }
    if (ancestorReparse)
    {
        fs->Log(fs->context, L"[!] %ls: ancestor %ls is a reparse point (MoveUsers' C:\\Users, or another link) - already persistent or cross-volume, refusing\n", ro, which);
        BdFail(out, L"reparse-ancestor", BD_STATUS_FAIL);
        return;
    }

    if (sRo.exists && !sRo.isDirectory)
    {
        // DIFFERS FROM LINUX: bind-dirs.sh bind-mounts files too. An NTFS junction is a
        // directory-only reparse point; a file would need a file symbolic link, whose
        // evaluation is policy-controlled (fsutil behavior SymlinkEvaluation) and which
        // applications commonly refuse to follow. Not supported - loudly.
        fs->Log(fs->context, L"[!] %ls is a file; only directories can be bound on Windows\n", ro);
        BdFail(out, L"file-unsupported", BD_STATUS_FAIL);
        return;
    }

    status = fs->Stat(fs->context, rw, &sRw);
    if (BD_FAILED(status))
    {
        BdFail(out, L"stat-target", status);
        return;
    }

    // --- first-use seeding ---
    if (!sRw.exists)
    {
        if (!sRo.exists)
        {
            // Linux: "is neither a directory nor a file and the path does not exist below
            // /rw, skipping." DIFFERS FROM LINUX: this FAILS. A path that exists nowhere is
            // a typo in the config, and a silently skipped typo is how "I configured it and
            // nothing persisted" happens.
            fs->Log(fs->context, L"[!] %ls does not exist and nothing is seeded at %ls\n", ro, rw);
            BdFail(out, L"source-missing", BD_STATUS_FAIL);
            return;
        }

        if (BdCopy(staging, BD_MAX_PATH, rw) != 0 || BdAppend(staging, BD_MAX_PATH, BD_SEEDING_SUFFIX) != 0)
        {
            BdFail(out, L"too-long", BD_STATUS_FAIL);
            return;
        }

        // A staging directory can only be the remains of an interrupted seed: the copy is
        // committed by the rename below, so an existing staging dir is by definition
        // incomplete and the source is still intact. Start over.
        status = fs->Stat(fs->context, staging, &st);
        if (BD_FAILED(status))
        {
            BdFail(out, L"stat-staging", status);
            return;
        }
        if (st.exists)
        {
            fs->Log(fs->context, L"[!] %ls left by an interrupted seed, removing\n", staging);
            status = fs->DeleteDirectory(fs->context, staging);
            if (BD_FAILED(status))
            {
                BdFail(out, L"seed-stale-staging", status);
                return;
            }
        }

        status = BdEnsureParents(fs, rw);
        if (BD_FAILED(status))
        {
            BdFail(out, L"seed-parent", status);
            return;
        }

        fs->Log(fs->context, L"[*] Initializing %ls with files from %ls\n", rw, ro);
        status = fs->CopyDirectory(fs->context, ro, staging);
        if (BD_FAILED(status))
        {
            fs->Log(fs->context, L"[!] seed copy %ls -> %ls failed: 0x%08lx, removing the partial copy\n", ro, staging, (unsigned long)status);
            fs->DeleteDirectory(fs->context, staging);
            BdFail(out, L"seed-copy", status);
            return;
        }
        // Commit: the atomic rename is what makes a seed "exist". Nothing can mistake a
        // half-copied staging dir for a completed seed, so a re-run can never re-seed over
        // real data.
        status = fs->Rename(fs->context, staging, rw);
        if (BD_FAILED(status))
        {
            fs->Log(fs->context, L"[!] seed commit %ls -> %ls failed: 0x%08lx\n", staging, rw, (unsigned long)status);
            fs->DeleteDirectory(fs->context, staging);
            BdFail(out, L"seed-commit", status);
            return;
        }
        out->seeded = 1;
    }
    else if (!sRw.isDirectory || sRw.isReparsePoint)
    {
        fs->Log(fs->context, L"[!] %ls exists but is not a plain directory\n", rw);
        BdFail(out, L"target-not-directory", BD_STATUS_FAIL);
        return;
    }

    // --- bind: move the original aside, put a junction in its place ---
    if (BdCopy(orig, BD_MAX_PATH, ro) != 0 || BdAppend(orig, BD_MAX_PATH, BD_ORIG_SUFFIX) != 0)
    {
        BdFail(out, L"too-long", BD_STATUS_FAIL);
        return;
    }

    status = fs->Stat(fs->context, orig, &st);
    if (BD_FAILED(status))
    {
        BdFail(out, L"stat-orig", status);
        return;
    }
    if (st.exists)
    {
        // Leftover of an earlier run that died between move-aside and cleanup. The seed
        // at rw is complete (rename-committed), so this is a duplicate of it.
        fs->Log(fs->context, L"[!] %ls left by an interrupted run, removing\n", orig);
        status = fs->DeleteDirectory(fs->context, orig);
        if (BD_FAILED(status))
        {
            BdFail(out, L"orig-stale", status);
            return;
        }
    }

    if (sRo.exists)
    {
        status = fs->Rename(fs->context, ro, orig);
        if (BD_FAILED(status))
        {
            // Nothing changed on C:. Typically STATUS_ACCESS_DENIED / SHARING_VIOLATION:
            // something has the directory open - i.e. this path is not manageable even at
            // BootExecute time.
            fs->Log(fs->context, L"[!] cannot move %ls aside: 0x%08lx\n", ro, (unsigned long)status);
            BdFail(out, L"move-aside", status);
            return;
        }
        movedAside = 1;
    }
    else
    {
        // Linux: mk_parent_dirs when the rw side exists but ro does not.
        status = BdEnsureParents(fs, ro);
        if (BD_FAILED(status))
        {
            BdFail(out, L"parent", status);
            return;
        }
    }

    status = fs->CreateDirectory(fs->context, ro);
    if (BD_FAILED(status))
    {
        fs->Log(fs->context, L"[!] cannot create %ls: 0x%08lx\n", ro, (unsigned long)status);
        BdFail(out, L"mkdir", status);
        goto rollback;
    }

    fs->Log(fs->context, L"[*] Bind mounting %ls onto %ls (junction)\n", rw, ro);
    status = fs->SetJunction(fs->context, ro, rw);
    if (BD_FAILED(status))
    {
        fs->Log(fs->context, L"[!] cannot set junction at %ls: 0x%08lx\n", ro, (unsigned long)status);
        BdFail(out, L"junction", status);
        fs->DeleteDirectory(fs->context, ro);
        goto rollback;
    }

    // Judge the outcome, not the call: re-read the reparse point.
    status = fs->Stat(fs->context, ro, &st);
    if (BD_FAILED(status) || !st.exists || !st.isReparsePoint || !BdTargetMatches(fs, &st, rw))
    {
        fs->Log(fs->context, L"[!] %ls does not read back as our junction after setting it\n", ro);
        BdFail(out, L"verify", BD_FAILED(status) ? status : BD_STATUS_FAIL);
        fs->DeleteDirectory(fs->context, ro);
        goto rollback;
    }

    if (movedAside)
    {
        // The bind is complete and the data is on Q:. On an AppVM this directory vanishes
        // with the volatile C: anyway; on a template/standalone it would be a stale copy.
        status = fs->DeleteDirectory(fs->context, orig);
        if (BD_FAILED(status))
        {
            fs->Log(fs->context, L"[!] WARNING: bound, but could not remove the moved-aside original %ls: 0x%08lx\n", orig, (unsigned long)status);
            out->warning = 1;
        }
    }

    out->result = L"ok";
    out->reason = L"bound";
    out->status = BD_OK;
    return;

rollback:
    if (movedAside)
    {
        BD_STATUS rb = fs->Rename(fs->context, orig, ro);
        if (BD_FAILED(rb))
        {
            fs->Log(fs->context, L"[!!] ROLLBACK FAILED: %ls is at %ls and could not be moved back: 0x%08lx\n", ro, orig, (unsigned long)rb);
            out->rollbackFailed = 1;
        }
        else
        {
            fs->Log(fs->context, L"[*] rolled back: %ls restored\n", ro);
        }
    }
}

BD_STATUS BdInitRwRoot(BD_FS *fs)
{
    BD_STAT st;
    BD_STATUS status;

    status = fs->Stat(fs->context, L"Q:\\", &st);
    if (BD_FAILED(status) || !st.exists)
    {
        fs->Log(fs->context, L"[!] Q:\\ (the private volume) is not available: 0x%08lx\n", (unsigned long)status);
        return BD_FAILED(status) ? status : BD_STATUS_FAIL;
    }
    status = fs->Stat(fs->context, BD_RW_ROOT, &st);
    if (BD_FAILED(status))
        return status;
    if (!st.exists)
    {
        status = fs->CreateDirectory(fs->context, BD_RW_ROOT);
        if (BD_FAILED(status))
        {
            fs->Log(fs->context, L"[!] cannot create %ls: 0x%08lx\n", BD_RW_ROOT, (unsigned long)status);
            return status;
        }
        fs->Log(fs->context, L"[*] created %ls\n", BD_RW_ROOT);
    }
    else if (!st.isDirectory || st.isReparsePoint)
    {
        fs->Log(fs->context, L"[!] %ls exists but is not a plain directory\n", BD_RW_ROOT);
        return BD_STATUS_FAIL;
    }
    return BD_OK;
}

BD_STATUS BdRun(BD_FS *fs, const BD_LIST *list, BD_ENTRY_RESULT *results, BD_REPORT *report)
{
    unsigned long i, j;
    BD_STATUS status;

    memset(report, 0, sizeof(*report));
    report->total = list->count;

    // Pass 1: validate everything and refuse duplicates/nesting BEFORE touching anything,
    // so a bad list fails as a whole picture rather than half-applied.
    for (i = 0; i < list->count; i++)
    {
        wchar_t normalized[BD_MAX_PATH];
        const wchar_t *reason;
        BD_ENTRY_RESULT *r = &results[i];

        memset(r, 0, sizeof(*r));
        r->result = L"pending";
        r->reason = L"";
        if (BD_FAILED(BdValidatePath(fs, list->entries[i].path, normalized, BD_MAX_PATH, &reason)))
        {
            fs->Log(fs->context, L"[!] %ls (%ls): refused: %ls\n", list->entries[i].path, list->entries[i].source, reason);
            BdFail(r, reason, BD_STATUS_FAIL);
            continue;
        }
        for (j = 0; j < i; j++)
        {
            // Compare against entries that validated (pending/ok/nested), never against
            // an entry that was refused for its own reasons.
            if (BdStrEq(results[j].result, L"failed") && !BdStrEq(results[j].reason, L"nested"))
                continue;
            if (BdPathEquals(fs, list->entries[j].path, normalized))
            {
                // Linux would bind it twice (stacked mounts). Harmless there, pointless
                // here: the second run would see its own junction. Note it, do nothing.
                fs->Log(fs->context, L"[*] %ls (%ls): duplicate of %ls, ignored\n", normalized, list->entries[i].source, list->entries[j].source);
                r->result = L"ok";
                r->reason = L"duplicate";
                break;
            }
            if (BdPathIsBelow(fs, normalized, list->entries[j].path) || BdPathIsBelow(fs, list->entries[j].path, normalized))
            {
                // DIFFERS FROM LINUX: nested binds are refused (both of them). Binding the
                // inner first and then seeding the outer would copy the inner junction onto
                // Q: as a junction pointing back into Q:\bind-dirs - a loop; binding the
                // outer first makes the inner an under-reparse path. Neither is what anyone
                // wants; the outer bind already persists the inner path.
                fs->Log(fs->context, L"[!] %ls (%ls) and %ls (%ls) are nested, refusing both\n",
                        normalized, list->entries[i].source, list->entries[j].path, list->entries[j].source);
                BdFail(r, L"nested", BD_STATUS_FAIL);
                BdFail(&results[j], L"nested", BD_STATUS_FAIL);
                break;
            }
        }
    }

    // Pass 2: apply, in config order.
    for (i = 0; i < list->count; i++)
    {
        BD_ENTRY_RESULT *r = &results[i];
        wchar_t saved[BD_MAX_PATH];

        if (!BdStrEq(r->result, L"pending"))
            continue;
        BdCopy(saved, BD_MAX_PATH, list->entries[i].path);
        BdApplyEntry(fs, saved, r);
    }

    for (i = 0; i < list->count; i++)
    {
        if (BdStrEq(results[i].result, L"ok"))
            report->ok++;
        else
            report->failed++;
        if (results[i].seeded)
            report->seeded++;
        if (results[i].warning || results[i].rollbackFailed)
            report->warnings++;
    }

    status = (report->failed == 0) ? BD_OK : BD_STATUS_FAIL;
    fs->Log(fs->context, L"[*] %lu entr%ls: %lu ok, %lu failed, %lu seeded, %lu warning%ls\n",
            report->total, report->total == 1 ? L"y" : L"ies", report->ok, report->failed, report->seeded,
            report->warnings, report->warnings == 1 ? L"" : L"s");
    return status;
}

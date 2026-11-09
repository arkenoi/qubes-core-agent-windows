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

#pragma once
#include <windows.h>
#include <libvchan.h>
#include <qrexec.h>

// TODO: make configurable
// The compiled-in default password is GONE. It was L"userpass", it never matched the account
// our installer creates, and even with the right password the only consumer returned
// ERROR_NOT_SUPPORTED because granting a fresh logon session access to the interactive desktop
// was never implemented. A user-context service now reuses the console session or fails saying so.

#define VCHAN_BUFFER_SIZE 65536
#define PIPE_BUFFER_SIZE 65536
#define PIPE_DEFAULT_TIMEOUT 100

// DATA-VCHAN PROTOCOL VERSION (2026-09-29). The control vchan (qrexec-agent <-> dom0 daemon) stays at
// QREXEC_PROTOCOL_VERSION 2: announcing 3 there obliges the agent to send MSG_TRIGGER_SERVICE3, and the
// daemon exits on a trigger that does not match the version an agent announced. Each DATA vchan negotiates
// its own version (min of the two hellos, independent of the control vchan), and at v3 the only change is
// the data chunk: 64 KiB instead of 4 KiB. Under v2 a 256 MiB copy is 65,536 messages each way through the
// wrapper; measured on this rig, a copy into a Linux qube (v3+) ran ~106 MB/s against ~47 MB/s into Windows.
#define DATA_PROTOCOL_VERSION   4   // v4 on the data vchan is v3 there (64 KiB chunks); 4 for consistency with upstream
#define MAX_DATA_CHUNK_V3       65536
// What this side SENDS stays at MAX_DATA_CHUNK (4 KiB) under either version - v3 only raises the MAXIMUM a
// chunk may be. Receiving up to 64 KiB while sending 4 KiB is the amended shape (44368a2), under which copy-out
// cannot regress by construction, because each send is all-or-nothing and waits for a ring gap the size of the
// WHOLE chunk.
// NO THROUGHPUT NUMBER BELONGS IN THIS FILE. The 2026-09-29 A/B's figures stood here as measured fact and are
// RETRACTED - findings/qrexec.md owns that record and the reason: the CONTROL's own copy-in ranged 40-106 MB/s
// and rose round over round, every round ran control first, and a counterbalanced re-run separated nothing.
// Quoting them from here has already produced one wrong argument. What stands is the SOURCE fact: Windows
// announced v2 (4 KiB chunks) where dom0 uses 64 KiB, and the data-vchan v3 change removes that.
// OWNER, 2026-11-09, on a proposal to SHRINK this geometry to save grant pages: «yet we still want up to date
// qrexec protocol and large buffers like linux counterpart does. measure later.» So the current protocol and
// the large buffers STAY, and nothing here is traded for grants. The one remaining gap to that parity is this
// side's 4 KiB SEND against dom0's 64 KiB; it is not closed here because the measurement is deferred and the
// instrument does not exist - tools/bench-stock-vs-ours.sh measures guest CPU, not copy throughput, and a valid
// one needs source on tmpfs, counterbalanced order and control stability shown first.

typedef enum _PIPE_TYPE
{
    PTYPE_INVALID = 0,
    PTYPE_STDOUT,
    PTYPE_STDERR,
    PTYPE_STDIN
} PIPE_TYPE;

// child i/o pipe
typedef struct _PIPE_DATA
{
    HANDLE      ReadEndpoint;
    HANDLE      WriteEndpoint;
} PIPE_DATA, *PPIPE_DATA;

// state of the child process
typedef struct _CHILD_STATE
{
    HANDLE       Process;
    HANDLE       StdoutThread;
    HANDLE       StderrThread;

    PIPE_DATA    Stdout;
    PIPE_DATA    Stderr;
    PIPE_DATA    Stdin;

    PSECURITY_DESCRIPTOR PipeSd;
    PACL         PipeAcl;

    libvchan_t   *Vchan;

    BOOL         IsVchanServer;

    // Negotiated DATA-vchan protocol version: 2 until the peer's hello is processed, then min(ours, peer's).
    // Read by the stdout/stderr threads while the main thread may set it, hence volatile LONG + Interlocked.
    volatile LONG DataVersion;

} CHILD_STATE, *PCHILD_STATE;

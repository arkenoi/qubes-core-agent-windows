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
#define DEFAULT_USER_PASSWORD_UNICODE   L"userpass"

#define VCHAN_BUFFER_SIZE 65536
#define PIPE_BUFFER_SIZE 65536
#define PIPE_DEFAULT_TIMEOUT 100

// DATA-VCHAN PROTOCOL VERSION (2026-09-29). The control vchan (qrexec-agent <-> dom0 daemon) stays at
// QREXEC_PROTOCOL_VERSION 2: announcing 3 there obliges the agent to send MSG_TRIGGER_SERVICE3, and the
// daemon exits on a trigger that does not match the version an agent announced. Each DATA vchan negotiates
// its own version (min of the two hellos, independent of the control vchan), and at v3 the only change is
// the data chunk: 64 KiB instead of 4 KiB. Under v2 a 256 MiB copy is 65,536 messages each way through the
// wrapper; measured on this rig, a copy into a Linux qube (v3+) ran ~106 MB/s against ~47 MB/s into Windows.
#define DATA_PROTOCOL_VERSION   3
#define MAX_DATA_CHUNK_V3       65536
// What this side SENDS stays at MAX_DATA_CHUNK (4 KiB) under either version - v3 only raises the MAXIMUM a
// chunk may be. Measured 2026-09-29 (interleaved, 3 rounds, Windows 11): with 32 KiB sends, copy OUT of the guest
// fell (median 51 -> 28 MB/s), because each send is all-or-nothing and waits for a ring gap the size of the WHOLE
// chunk by polling with Sleep(1). Receiving up to 64 KiB is where the gain is (copy IN median 55 -> 94 MB/s).

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

    // Our MSG_HELLO has gone out on the data vchan (the server sends it in InitVchan, the client replies to the peer's).
    // The final cleanup sends one only if not: a second hello on a vchan that already carried data is a protocol error.
    BOOL         HelloSent;
} CHILD_STATE, *PCHILD_STATE;

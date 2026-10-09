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

// This program is launched by qrexec agent as a wrapper for arbitrary executable
// (qrexec services or just anything).
// It's role is communication with the data vchan peer and handling child program's I/O.

#include "qrexec-wrapper.h"
#include <stdlib.h>
#include <shlwapi.h>
#include <assert.h>
#include <strsafe.h>

#include <libvchan.h>
#include <qrexec.h>

#include <log.h>
#include <vchan-common.h>
#include <exec.h>
#include <qubes-io.h>

// How long to wait for the stdout/stderr pumps to DRAIN after the child exits, before the exit
// code goes out and the peer closes the vchan. This is a hang backstop only - it is NOT a normal
// completion path, and reaching it means the transfer was truncated (see EventLoop). It replaces a
// 1000 ms wait that was routinely hit by ordinary transfers and silently shortened them.
#define DRAIN_TIMEOUT_MS (120 * 1000)

static CRITICAL_SECTION g_VchanCs;

// CONSOLE-CONTROL SHUTDOWN (2026-09-29). Measured during a Windows 11 reinstall: two live wrappers were ended with
// STATUS_CONTROL_C_EXIT by the MSI's Restart Manager, five seconds after the installer had stopped QrexecAgent -
// wrappers outlive the agent (one was held open by a detached child that inherited its pipes). Without a handler,
// the default one calls ExitProcess at once and the data vchan's grant mapping is left to the exit-time kernel
// cleanup, the path the always-close in wmain's cleanup exists to avoid. WrapperCtrlHandler signals g_StopEvent;
// the main thread leaves its wait, runs that same cleanup on its own thread, and signals g_ClosedEvent.
#define CTRL_CLOSE_WAIT_MS 3000
static HANDLE g_StopEvent = NULL;
static HANDLE g_ClosedEvent = NULL;

// Set by wmain's cleanup just before it closes the data vchan, when nothing more is to be sent: the exit code (the
// protocol's last message) has gone out, or the peer is gone, or the call is failing or being stopped. A pump still
// waiting for room then gives up at once (VchanWaitForSendSpace), so the close never waits on a peer that has stopped
// reading.
static volatile LONG g_VchanClosing = 0;

// IS THERE A DATA VCHAN TO CLOSE AT ALL? The console-control handler waits for the cleanup to
// confirm the close, and reports an ERROR when it does not - which is right when a mapping could be
// left for the kernel to reclaim at process exit, and meaningless when there was never a vchan. The
// commonest case of the latter is the one this log was full of: a client that went away before
// libvchan_client_init could connect, where nothing was ever mapped and nothing will ever signal
// the close. Set after InitVchan succeeds, cleared once the cleanup has closed it.
static volatile LONG g_VchanOpen = 0;

static BOOL WINAPI WrapperCtrlHandler(DWORD ctrlType)
{
    switch (ctrlType)
    {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        LogWarning("console control event %lu: closing the data vchan before exiting", ctrlType);
        if (g_StopEvent)
            SetEvent(g_StopEvent);
        if (g_ClosedEvent && WaitForSingleObject(g_ClosedEvent, CTRL_CLOSE_WAIT_MS) == WAIT_OBJECT_0)
            LogInfo("data vchan closed by our own cleanup after console control event %lu", ctrlType);
        else if (!g_VchanOpen)
            // NOTHING TO CONFIRM. No data vchan is open - it was never created (the commonest case:
            // a client that went away before libvchan_client_init could connect) or the cleanup has
            // already closed it. Nothing is mapped, so nothing can be left for the kernel to
            // reclaim at process exit, and no cleanup will ever signal the close.
            LogInfo("no data vchan to close at console control event %lu - exiting", ctrlType);
        else
            // A MAPPING MAY BE LEFT BEHIND. This is the case the always-close in the cleanup exists
            // for: the ring's grant mapping and the event channel are reclaimed during process exit
            // instead, from a system worker attached to the dying process - the path the 2026-08-20
            // NMI dump caught spinning on a TLB shootdown.
            LogError("data vchan NOT confirmed closed within %lu ms of console control event %lu (cleanup %s) - "
                     L"exiting anyway, so its mapping is reclaimed at process exit",
                     CTRL_CLOSE_WAIT_MS, ctrlType, g_VchanClosing ? L"had begun closing it" : L"never reached the close");
        // The status the default handler would have used, so nothing downstream sees a different exit.
        ExitProcess(STATUS_CONTROL_C_EXIT);
    default:
        return FALSE;
    }
}
static BOOL g_exitCodeReceived = FALSE;

// WHY THE EVENT LOOP ENDED. The cleanup has to say whether an output pump still running at exit is
// expected or a loss, and it could not: it printed one warning asserting three possible causes
// ("the peer hung up first, a data error or a stop") without knowing which. On a guest-initiated
// call the ordinary end is the peer closing the vchan, which never reaches the post-exit drain, so
// the pumps ARE still running and every successful call logged that warning - 229 of them in one
// boot, 226 inside two minutes.
typedef enum {
    EXIT_REASON_UNSET = 0,
    EXIT_REASON_PEER_CLOSED,     // the peer finished and closed the vchan: the ordinary end
    EXIT_REASON_CHILD_DRAINED,   // our child exited and both pumps were waited for
    EXIT_REASON_DATA_ERROR,      // HandleDataMessage failed
    EXIT_REASON_DRAIN_EXPIRED,   // the 120 s backstop: already reported as a TRUNCATED transfer
    EXIT_REASON_CTRL_STOP,       // a console control event asked us to stop
} EXIT_REASON;
static EXIT_REASON g_exitReason = EXIT_REASON_UNSET;

static const WCHAR *ExitReasonName(EXIT_REASON r)
{
    switch (r)
    {
    case EXIT_REASON_PEER_CLOSED:   return L"the peer closed the vchan";
    case EXIT_REASON_CHILD_DRAINED: return L"the child exited and its output was drained";
    case EXIT_REASON_DATA_ERROR:    return L"a data error";
    case EXIT_REASON_DRAIN_EXPIRED: return L"the drain backstop expired";
    case EXIT_REASON_CTRL_STOP:     return L"a console control stop";
    default:                        return L"not recorded";
    }
}

/**
 * @brief Create an anonymous pipe that will be used as one of the std handles for a child process.
 * @param pipeData Pipe data to initialize.
 * @param pipeType Pipe type.
 * @param securityDescriptor Security descriptor for the pipe.
 * @return Error code.
 */
DWORD InitPipe(
    _Out_ PPIPE_DATA pipeData,
    _In_ PIPE_TYPE pipeType,
    _In_ PSECURITY_DESCRIPTOR securityDescriptor
    )
{
    SECURITY_ATTRIBUTES sa = { 0 };

    assert(pipeData);

    LogVerbose("pipe type %d", pipeType);

    ZeroMemory(pipeData, sizeof(*pipeData));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = securityDescriptor;

    if (!CreatePipe(&pipeData->ReadEndpoint, &pipeData->WriteEndpoint, &sa, PIPE_BUFFER_SIZE))
        return win_perror("CreatePipe");

    return ERROR_SUCCESS;
}

/**
 * @brief Close given pipes and set them to NULL.
 * @param pipeData Pipe data to close.
 */
void ClosePipe(
    _Inout_ PPIPE_DATA pipeData
    )
{
    assert(pipeData);

    if (pipeData->ReadEndpoint)
    {
        LogDebug("closed read pipe %p", pipeData->ReadEndpoint);
        CloseHandle(pipeData->ReadEndpoint);
        pipeData->ReadEndpoint = NULL;
    }

    if (pipeData->WriteEndpoint)
    {
        LogDebug("closed write pipe %p", pipeData->WriteEndpoint);
        CloseHandle(pipeData->WriteEndpoint);
        pipeData->WriteEndpoint = NULL;
    }
}

/**
* @brief Create pipes that will be used as std* handles for the child process.
* @param child Child state.
* @return Error code.
*/
DWORD CreateChildPipes(
    _Inout_ PCHILD_STATE child
    )
{
    DWORD status;

    assert(child);

    LogVerbose("start");

    // create pipes and make sure endpoints we use are not inherited
    status = InitPipe(&child->Stdout, PTYPE_STDOUT, child->PipeSd);
    if (ERROR_SUCCESS != status)
        return win_perror2(status, "InitPipe(STDOUT)");

    SetHandleInformation(child->Stdout.ReadEndpoint, HANDLE_FLAG_INHERIT, 0);

    status = InitPipe(&child->Stderr, PTYPE_STDERR, child->PipeSd);
    if (ERROR_SUCCESS != status)
    {
        ClosePipe(&child->Stdout);
        return win_perror2(status, "InitPipe(STDERR)");
    }

    SetHandleInformation(child->Stderr.ReadEndpoint, HANDLE_FLAG_INHERIT, 0);

    status = InitPipe(&child->Stdin, PTYPE_STDIN, child->PipeSd);
    if (ERROR_SUCCESS != status)
    {
        ClosePipe(&child->Stdout);
        ClosePipe(&child->Stderr);
        return win_perror2(status, "InitPipe(STDIN)");
    }

    SetHandleInformation(child->Stdin.WriteEndpoint, HANDLE_FLAG_INHERIT, 0);

    return ERROR_SUCCESS;
}

/**
* @brief Create the child process that's optionally piped with data peer's vchan for i/o exchange.
* @param child Child state.
* @param userName User name to run the child as. If NULL, this process' user token will be used (normally SYSTEM).
* @param commandLine Command line of the child process.
* @param interactive Run the child in the interactive session (a user must be logged in).
* @param piped Connect the child's standard I/O handles to pipes.
* @return Error code.
*/
DWORD StartChild(
    _Inout_ PCHILD_STATE child,
    _In_opt_ const PWSTR userName,
    _Inout_ PWSTR commandLine, // CreateProcess* can modify this
    _In_ BOOL interactive,
    _In_ BOOL piped
    )
{
    DWORD status;

    assert(child);
    assert(commandLine);

    LogDebug("user '%s', cmd '%s', interactive %d, piped %d", userName, commandLine, interactive, piped);

    // if userName is NULL we run the process on behalf of the current user.
    if (userName)
        LogInfo("Running '%s' as user '%s'", commandLine, userName);
    else
        LogInfo("Running '%s' as SYSTEM", commandLine);

    if (piped)
    {
        status = CreateChildPipes(child);
        if (ERROR_SUCCESS != status)
            return win_perror2(status, "CreateChildPipes");
    }

    if (userName)
    {
        if (piped)
        {
            status = CreatePipedProcessAsUser(
                userName,
                DEFAULT_USER_PASSWORD_UNICODE, // password will only be required if the requested user is not logged on
                commandLine,
                interactive,
                child->Stdin.ReadEndpoint, // child will use stdin for reading and stdout/stderr for writing
                child->Stdout.WriteEndpoint,
                child->Stderr.WriteEndpoint,
                &child->Process);
        }
        else // piped
        {
            status = CreateNormalProcessAsUser(
                userName,
                DEFAULT_USER_PASSWORD_UNICODE,
                commandLine,
                interactive,
                &child->Process);

            if (ERROR_SUCCESS != status)
            {
                win_perror2(status, "CreateNormalProcessAsUser");
                // Run-as the requested user failed, so we degrade to SYSTEM. This is an ANOMALY on
                // a logged-on guest, not a normal fallback: the usual cause is the GWeck field bug
                // (2026-09-10) where dom0's default target user "user" does not match the guest's
                // real local account (qvm-prefs default_user), so exec.c's LogonUser("user",...)
                // failed. The correct fix for interactive services is in CreatePipedProcessAsUser
                // (reuse the logged-on console token regardless of the requested name); log loudly
                // here so a residual run-as failure is visible instead of silently becoming SYSTEM.
                LogWarning("run-as user '%s' failed (0x%x) - degrading this service to SYSTEM "
                    "(anomaly on a logged-on guest; the guest's real account may differ from the "
                    "requested name)", userName, status);
                status = CreateNormalProcessAsCurrentUser(
                    commandLine,
                    &child->Process);
            }
        }
    }
    else // userName
    {
        if (piped)
        {
            status = CreatePipedProcessAsCurrentUser(
                commandLine,
                interactive,
                child->Stdin.ReadEndpoint,
                child->Stdout.WriteEndpoint,
                child->Stderr.WriteEndpoint,
                &child->Process);
        }
        else
        {
            status = CreateNormalProcessAsCurrentUser(
                commandLine,
                &child->Process);
        }
    }

    if (piped)
    {
        // we won't be using these pipe endpoints, only the child will
        CloseHandle(child->Stdin.ReadEndpoint);
        child->Stdin.ReadEndpoint = NULL;
        CloseHandle(child->Stdout.WriteEndpoint);
        child->Stdout.WriteEndpoint = NULL;
        CloseHandle(child->Stderr.WriteEndpoint);
        child->Stderr.WriteEndpoint = NULL;
    }

    if (ERROR_SUCCESS != status)
    {
        if (piped)
        {
            // close *our* endpoints
            CloseHandle(child->Stdin.WriteEndpoint);
            CloseHandle(child->Stdout.ReadEndpoint);
            CloseHandle(child->Stderr.ReadEndpoint);
            return win_perror2(status, "CreatePipedProcessAsCurrentUser");
        }

        return win_perror2(status, "CreateNormalProcessAsCurrentUser");
    }

    // Give the service payload the whole machine back. This process inherited qrexec-agent's
    // one-CPU mask (QREXECPIN, qrexec-agent.c) so that its vchan and event-channel work stays on
    // one CPU; the payload does none of that work and must not be confined to that CPU.
    {
        DWORD_PTR procMask = 0, sysMask = 0;
        if (child->Process && GetProcessAffinityMask(GetCurrentProcess(), &procMask, &sysMask) &&
            sysMask && procMask != sysMask && !SetProcessAffinityMask(child->Process, sysMask))
        {
            LogWarning("QREXECPIN could not give the payload the full CPU mask 0x%Ix (error 0x%x)",
                sysMask, GetLastError());
        }
    }

    return ERROR_SUCCESS;
}

/**
 * @brief Wait until the data vchan has room for @a size bytes; FALSE if it closed first.
 *
 * VchanSendBuffer (windows-utils) waits for room itself, but never checks that the peer is still there: a peer that
 * goes away while the ring is full (killed mid-transfer) leaves that wait spinning forever, with g_VchanCs held.
 * VchanSendMessage waits here instead, under the same lock, for the WHOLE message (header and data), so both
 * VchanSendBuffer calls find their room and never spin, and a message goes out whole or not at all; only the peer
 * frees room, so nothing can take it in between. It logs the warning VchanSendBuffer logged when it had to wait.
 * @param vchan Data vchan.
 * @param size Bytes the next VchanSendBuffer calls will write.
 * @param what Description of the buffer (for logging).
 * @return TRUE when there is room, FALSE when the vchan closed.
 */
static BOOL VchanWaitForSendSpace(
    _Inout_ libvchan_t *vchan,
    _In_ size_t size,
    _In_ const PWSTR what
    )
{
    if (VchanGetWriteBufferSize(vchan) < (int)size)
        LogWarning("(%p, %s): vchan buffer full, blocking write", vchan, what);
    while (VchanGetWriteBufferSize(vchan) < (int)size)
    {
        if (!libvchan_is_open(vchan))
        {
            LogWarning("the peer closed the vchan while %Iu bytes waited for room - not sent", size);
            return FALSE;
        }
        if (g_VchanClosing)
        {
            LogWarning("the vchan is being closed while %Iu bytes waited for room - not sent", size);
            return FALSE;
        }
        Sleep(1);
    }
    return TRUE;
}

/**
 * @brief Send message to the vchan peer.
 * @param vchan Data vchan.
 * @param messageType Data message type (MSG_DATA_*).
 * @param data Buffer to send.
 * @param cbData Size of the @a data buffer, in bytes.
 * @param what Description of the buffer (for logging).
 * @return TRUE on success.
 */
BOOL VchanSendMessage(
    _Inout_ libvchan_t *vchan,
    _In_ ULONG messageType,
    _In_reads_bytes_opt_(cbData) const void *data,
    _In_ ULONG cbData,
    _In_ const PWSTR what
    )
{
    struct msg_header header;
    BOOL status = FALSE;

    assert(vchan);

    LogDebug("msg 0x%x, data %p, size %u (%s)", messageType, data, cbData, what);

    header.type = messageType;
    header.len = cbData;
    EnterCriticalSection(&g_VchanCs);

    if (!libvchan_is_open(vchan))
    {
        // THE PEER IS GONE, and whether that COSTS anything depends on what we were about to send.
        // This line used to be "vchan is closed" and nothing else - no message type, no size, no
        // description - so the ordinary end (the client hung up after taking the exit code) and a
        // real loss of output were indistinguishable, and it was one of the eight undeclared error
        // lines a clean startup/shutdown cycle produced on win11r-logvol, 2026-10-08. cbData == 0 is
        // the trailing EOF marker (see the `cbData == 0 // EOF` branch below): there is nothing in
        // it to lose. Anything with bytes pending IS a loss and still says so, now with the amount.
        // MEASURED 2026-10-09 on a current build: four of the five error lines a clean capture
        // carried were this one for `4 byte(s) of hello (msg 0x300)`. A handshake is not OUTPUT -
        // when the peer departs before it, nothing a user had is lost; the request was already dead
        // and QGAVCHANFAIL's condition has simply arrived one step later. The discriminator is the
        // message TYPE, a positive fact, not an absence. Pending CHILD OUTPUT is still a real loss
        // and still an error, with the amount.
        if (cbData == 0)
            LogDebug("vchan already closed, nothing left to send: msg 0x%x (%s)", messageType, what);
        else if (messageType == MSG_HELLO)
            LogWarning("peer gone before the handshake (msg 0x%x); request never served", messageType);
        else
            LogError("vchan already closed with %u byte(s) of %s (msg 0x%x) still to send - that "
                L"output does not reach the peer", cbData, what, messageType);
        goto cleanup;
    }

    if (!VchanWaitForSendSpace(vchan, sizeof(header) + cbData, what))
        goto cleanup;

    if (!VchanSendBuffer(vchan, &header, sizeof(header), L"header"))
    {
        LogError("VchanSendBuffer(header for %s) failed", what);
        goto cleanup;
    }

    status = TRUE;
    if (cbData == 0) // EOF
        goto cleanup;

    status = FALSE;
    if (!VchanSendBuffer(vchan, data, cbData, what))
    {
        LogError("VchanSendBuffer(%s) failed", what);
        goto cleanup;
    }
    status = TRUE;

cleanup:
    LeaveCriticalSection(&g_VchanCs);
    return status;
}

/**
 * @brief Send child output to the vchan data peer.
 * @param child Child state.
 * @param data Stdout data to send.
 * @param cbData Size of the @a data buffer, in bytes.
 * @param pipeType Pipe type (stdout/stderr).
 * @return TRUE on success.
 */
BOOL VchanSendData(
    _In_ const PCHILD_STATE child,
    _In_reads_bytes_opt_(cbData) const BYTE *data,
    _In_ DWORD cbData,
    _In_ PIPE_TYPE pipeType
    )
{
    ULONG messageType;
    BOOL ok;

    assert(child);
    if (!child)
        return FALSE;

    LogVerbose("data %p, size %lu, type %d", data, cbData, pipeType);

    switch (pipeType)
    {
    case PTYPE_STDOUT:
        messageType = MSG_DATA_STDOUT;
        break;
    case PTYPE_STDERR:
        messageType = MSG_DATA_STDERR;
        break;
    default:
        LogError("invalid pipe type %d", pipeType);
        return FALSE;
    }

    // child->Vchan is read under g_VchanCs because wmain's cleanup closes the vchan and clears it under that lock
    // while an output pump may still be running (see there): the pump either finishes this send before the close or
    // finds NULL after it, never a vchan being or already closed.
    EnterCriticalSection(&g_VchanCs);
    ok = child->Vchan && VchanSendMessage(child->Vchan, messageType, data, cbData, L"output data");
    LeaveCriticalSection(&g_VchanCs);
    return ok;
}

/**
 * @brief Send MSG_DATA_EXIT_CODE to the vchan peer if we're not the vchan server.
 * @param child Child state.
 * @param exitCode Exit code.
 * @return TRUE on success.
 */
BOOL VchanSendExitCode(
    _In_ const PCHILD_STATE child,
    _In_ int exitCode
    )
{
    assert(child && child->Vchan);

    LogVerbose("code %d", exitCode);

    // don't send anything if we're the vchan server.
    if (child->IsVchanServer)
        return TRUE;

    // EOF should be sent before exit code because peer closes vchan after receiving exit code
    LogDebug("sending stderr EOF");
    VchanSendData(child, NULL, 0, PTYPE_STDERR);
    LogDebug("sending stdout EOF");
    VchanSendData(child, NULL, 0, PTYPE_STDOUT);

    if (!VchanSendMessage(child->Vchan, MSG_DATA_EXIT_CODE, &exitCode, sizeof(exitCode), L"exit code"))
        return FALSE;

    LogDebug("Sent exit code %d", exitCode);
    return TRUE;
}

/**
 * @brief Send MSG_HELLO to the vchan peer.
 * @param vchan Data vchan.
 * @return TRUE on success.
 */
BOOL VchanSendHello(
    _Inout_ libvchan_t *vchan
    )
{
    struct peer_info info;

    assert(vchan);

    // The DATA vchan announces v3 (64 KiB chunks); see DATA_PROTOCOL_VERSION. The control vchan is not ours.
    info.version = DATA_PROTOCOL_VERSION;

    return VchanSendMessage(vchan, MSG_HELLO, &info, sizeof(info), L"hello");
}

/**
 * @brief Read stdin/stdout/stderr from data vchan. Send to child's stdin or just log if stderr.
 * @param header Vchan message header that was already read.
 * @param child Child state.
 * @return Error code.
 */
DWORD HandleRemoteData(
    _In_ const struct msg_header *header,
    _Inout_ PCHILD_STATE child
    )
{
    static void *buffer = NULL;
    DWORD status = ERROR_UNIDENTIFIED_ERROR;

    assert(header);
    assert(child && child->Vchan);

    LogVerbose("msg 0x%x, len %d, vchan data ready %d",
               header->type, header->len, VchanGetReadBufferSize(child->Vchan));

    if (!buffer)
    {
        status = ERROR_NOT_ENOUGH_MEMORY;
        buffer = malloc(MAX_DATA_CHUNK_V3); // large enough for either negotiated version
        if (!buffer)
            goto cleanup;
    }

    status = ERROR_INVALID_FUNCTION;
    if (!VchanReceiveBuffer(child->Vchan, buffer, header->len, header->type == MSG_DATA_STDERR ? L"stderr data" : L"inbound data"))
        goto cleanup;

    if (header->type != MSG_DATA_STDERR)
    {
        assert(child->Stdin.WriteEndpoint);
        LogVerbose("writing %d bytes of inbound data to child", header->len);
        if (!QioWriteBuffer(child->Stdin.WriteEndpoint, buffer, header->len))
        {
            status = win_perror("writing stdin data");
            goto cleanup;
        }
    }
    else
    {
        // write to log file (also to our stderr)
        // FIXME: is this unicode or ascii or what? assuming ascii
        LogInfo("STDERR from vchan: %S", buffer);
    }

    status = ERROR_SUCCESS;

cleanup:
    return status;
}

/**
 * @brief Handle MSG_DATA_EXIT_CODE (we're the vchan server). Just log it.
 * @param child Child state.
 * @return Error code.
 */
DWORD HandleExitCode(
    _Inout_ PCHILD_STATE child
    )
{
    int code;

    assert(child && child->Vchan && child->IsVchanServer);

    if (!VchanReceiveBuffer(child->Vchan, &code, sizeof(code), L"peer exit code"))
        return ERROR_INVALID_FUNCTION;

    LogDebug("remote exit code: %d", code);

    g_exitCodeReceived = TRUE;
    return ERROR_SUCCESS;
}

/**
 * @brief Handle data vchan message. Read header and dispatch accordingly.
 * @param child Child state.
 * @return Error code.
 */
DWORD HandleDataMessage(
    _Inout_ PCHILD_STATE child
    )
{
    struct msg_header header;
    struct peer_info peerInfo;

    assert(child && child->Vchan);

    if (VchanGetReadBufferSize(child->Vchan) == 0)
    {
        LogVerbose("no data");
        return ERROR_SUCCESS;
    }

    LogDebug("is server %d", child->IsVchanServer);
    if (!VchanReceiveBuffer(child->Vchan, &header, sizeof(header), L"data header"))
    {
        LogError("VchanReceiveBuffer(header) failed");
        return ERROR_INVALID_FUNCTION;
    }

    // The limit follows the NEGOTIATED data version: 4 KiB until (and unless) both hellos said >= 3.
    DWORD maxChunk = (child->DataVersion >= 3) ? MAX_DATA_CHUNK_V3 : MAX_DATA_CHUNK;
    if (header.len > maxChunk)
    {
        LogError("msg 0x%x, size too big: %d (max %lu, data protocol %ld)", header.type, header.len, maxChunk, child->DataVersion);
        return ERROR_INVALID_FUNCTION;
    }

    // stdin and stdout messages are basically interchangeable depending on which role we're in (vchan server or client)
    if (header.len == 0) // EOF
    {
        if (header.type == MSG_DATA_STDIN || header.type == MSG_DATA_STDOUT)
        {
            LogDebug("EOF from vchan (msg 0x%x)", header.type);
            ClosePipe(&child->Stdin);
            return ERROR_SUCCESS;
        }
        if (header.type == MSG_DATA_STDERR)
        {
            LogDebug("stderr EOF from vchan (msg 0x%x)", header.type);
            return ERROR_SUCCESS;
        }
    }

    /*
    * qrexec-client is the vchan server
    * sends: MSG_HELLO, MSG_DATA_STDIN
    * expects: MSG_HELLO, MSG_DATA_STDOUT, MSG_DATA_STDERR, MSG_DATA_EXIT_CODE
    *
    * if CLIENT_INFO.IsVchanServer is set, we act as a qrexec-client (vchan server)
    * (service connection to another agent that is the usual vchan client)
    */

    switch (header.type)
    {
    case MSG_HELLO:
        LogVerbose("MSG_HELLO");
        if (!VchanReceiveBuffer(child->Vchan, &peerInfo, sizeof(peerInfo), L"peer info"))
            return ERROR_INVALID_FUNCTION;

        LogDebug("protocol version %d", peerInfo.version);

        if (peerInfo.version < QREXEC_PROTOCOL_VERSION)
        {
            LogWarning("incompatible protocol version (got %d, expected %d)", peerInfo.version, QREXEC_PROTOCOL_VERSION);
            return ERROR_INVALID_FUNCTION;
        }

        // Negotiated data version = min(ours, peer's). The peer computes the same from our hello, so from
        // here on both sides may use chunks up to max_data_chunk(version). Set BEFORE we reply (client
        // case): the peer only sends larger chunks after it has our hello.
        InterlockedExchange(&child->DataVersion,
            peerInfo.version < DATA_PROTOCOL_VERSION ? (LONG)peerInfo.version : DATA_PROTOCOL_VERSION);
        LogInfo("data protocol version %ld (peer %d, ours %d)", child->DataVersion, peerInfo.version, DATA_PROTOCOL_VERSION);

        if (!child->IsVchanServer) // we're vchan client, reply with HELLO
        {
            if (!VchanSendHello(child->Vchan))
                return ERROR_INVALID_FUNCTION;
            child->HelloSent = TRUE;
        }
        break;

    case MSG_DATA_STDIN:
        LogVerbose("MSG_DATA_STDIN");
        return HandleRemoteData(&header, child);

    case MSG_DATA_STDOUT:
        LogVerbose("MSG_DATA_STDOUT");
        return HandleRemoteData(&header, child);

    case MSG_DATA_STDERR:
        LogVerbose("MSG_DATA_STDERR");
        return HandleRemoteData(&header, child);

    case MSG_DATA_EXIT_CODE:
        LogVerbose("MSG_DATA_EXIT_CODE");
        return HandleExitCode(child);

    default:
        LogError("unknown message type: 0x%x", header.type);
        return ERROR_INVALID_PARAMETER;
    }

    return ERROR_SUCCESS;
}

/**
 * @brief Read data from child's pipe, send to vchan
 */
static DWORD handle_child_output(
    _Inout_ PCHILD_STATE child,
    _In_    PIPE_TYPE pipe_type
    )
{
    PPIPE_DATA pipe;
    PBYTE buffer;
    DWORD eof;

    assert(child);

    pipe = pipe_type == PTYPE_STDOUT ? &child->Stdout : &child->Stderr;

    buffer = malloc(MAX_DATA_CHUNK_V3); // large enough for either negotiated version
    if (!buffer)
    {
        LogError("no memory");
        goto cleanup;
    }

    LogVerbose("start (type %d)", pipe_type);

    eof = FALSE;
    while (!eof)
    {
        DWORD nread;

        LogVerbose("reading...");
        // Sends stay at 4 KiB under either version (see MAX_DATA_CHUNK_V3 in qrexec-wrapper.h: larger sends
        // made copy-out slower through the all-or-nothing, Sleep(1)-polling send path).
        BOOL ok = ReadFile(pipe->ReadEndpoint, buffer, MAX_DATA_CHUNK, &nread, NULL); // this can block
        //
        // EOF is signaled by either:
        // - ok and nread == 0
        // - !ok and GetLastError() == ERROR_BROKEN_PIPE.
        //
        // ReadFile of an anonymous pipe returns FALSE and GetLastError
        // returns ERROR_BROKEN_PIPE when the corresponding write handle
        // has been closed.
        if (!ok && GetLastError() != ERROR_BROKEN_PIPE)
        {
            // if !ok, then nread == 0
            // Signal error condition by sending EOF
            win_perror("ReadFile");
        }
        eof = nread == 0;
        LogVerbose("read %lu 0x%lx", nread, nread);
        //hex_dump("child data", buffer, nread);

        if (nread > 0)
        {
            if (!VchanSendData(child, buffer, nread, pipe_type))
            {
                // Every failure path inside VchanSendData logs what failed and what it cost (the
                // closed peer with the byte count, the header send, the wait for space), so this
                // line only ever restated the one above it - the same two-lines-for-one-condition
                // the collapse exists to remove.
                LogDebug("VchanSendData failed; output pump ends");
                goto cleanup;
            }
        }
    }

cleanup:
    ClosePipe(pipe);
    LogVerbose("exiting (type %d)", pipe_type);
    free(buffer);
    return 1;
}

DWORD WINAPI StdoutThread(
    PVOID param
    )
{
    PCHILD_STATE child = param;

    DWORD ret = handle_child_output(child, PTYPE_STDOUT);
    LogDebug("sending stdout EOF");
    VchanSendData(child, NULL, 0, PTYPE_STDOUT);
    return ret;
}

DWORD WINAPI StderrThread(
    PVOID param
    )
{
    PCHILD_STATE child = param;

    DWORD ret = handle_child_output(child, PTYPE_STDERR);
    LogDebug("sending stderr EOF");
    VchanSendData(child, NULL, 0, PTYPE_STDERR);
    return ret;
}

// A DEPARTED PEER IS AN ORDINARY EVENT AND USED TO COST FOUR ERROR LINES OF OURS, PER CALL.
//
// This wrapper installs itself as the log sink for the PV/vchan libraries and forwards every
// message at the library's own level, verbatim (libvchan_register_logger(XifLogger, ...) below).
// Those libraries log every failed STEP at XLL_ERROR, including steps whose failure is part of a
// normal connect, and three layers each restate the same fact. The level enums line up 1:1
// (XLL_ERROR=1..XLL_TRACE=5 vs LOG_LEVEL_ERROR=1..VERBOSE=5), so an XLL_ERROR became an E line in
// our log with no classification at all.
//
// The measured four lines are TWO xenstore reads of ONE question - "has the peer published its
// ring?" - restated by every layer that touched it:
//   1. the deliberate existence PROBE in libvchan_client_init, whose own comment is "test if the
//      store entry exists; if not - wait a second time" and whose status the caller IGNORES,
//      logged one layer down by xencontrol as XcStoreRead ... failed: 0x5;
//   2. the second 500 ms watch wait timing out (the 0x102 warning);
//   3. the real read of ring-ref in libxenvchan_client_init - the same xencontrol ERROR again on a
//      second handle - plus that caller's own ERROR;
//   4. libxenvchan_client_init returning NULL -> "libvchan_client_init(...) failed".
// The status cannot settle it either: xeniface answers 0x5 (ACCESS_DENIED) for an absent node as
// well as for a real refusal, so the code alone cannot tell "the client left" from "we are broken".
//
// So the sink becomes CONNECT-AWARE: while the client connect runs, library records are HELD
// instead of printed, and the outcome decides what to do with them. Two rules keep this from being
// a suppression:
//   * NOTHING IS DESTROYED - every held record is replayed in every branch, at DEBUG when the
//     outcome was ordinary and at its ORIGINAL level when it was not, so raising the wrapper's
//     LogLevel reproduces today's output verbatim;
//   * QUIET REQUIRES TWO AGREEING POSITIVE DETECTIONS. If the held buffer overflowed, if the
//     detectors disagree, or if the probe returns anything unexpected, the loud replay is the
//     default. The absence of evidence never buys silence.
// MEASURED ON A GUEST 2026-10-09, which is how this number was chosen rather than guessed: at
// LogLevel 4 a client connect emits 20 records on the failing path and 27 on the succeeding one, so
// a 16-slot hold overflowed on EVERY connect - and because the hold is first-come-first-served, what
// it dropped was the diagnostic TAIL: the ring-ref status and libvchan's own verdict. One line built
// from that is worse than four lines, so the hold now takes a whole sequence with headroom.
#define XIFHOLD_MAX 64
typedef struct _XIFHOLD_REC {
    int level;
    char function[64];
    WCHAR text[1024];
} XIFHOLD_REC;
static XIFHOLD_REC g_XifHold[XIFHOLD_MAX];
static ULONG g_XifHoldCount = 0;
static ULONG g_XifHoldLost = 0;   // records past XIFHOLD_MAX: reported, never silently dropped
static BOOL g_XifHoldOn = FALSE;

static void XifLogger(int level, const char *function, const WCHAR *format, va_list args)
{
    WCHAR buf[1024];

    StringCbVPrintfW(buf, sizeof(buf), format, args);

    // The connect runs on the main thread before any pump thread exists, so the hold needs no lock.
    if (g_XifHoldOn)
    {
        if (g_XifHoldCount < XIFHOLD_MAX)
        {
            XIFHOLD_REC *r = &g_XifHold[g_XifHoldCount++];
            r->level = level;
            r->function[0] = 0;
            if (function)
                StringCbCopyA(r->function, sizeof(r->function), function);
            StringCbCopyW(r->text, sizeof(r->text), buf);
        }
        else
        {
            g_XifHoldLost++;
        }
        return;
    }

    // PRE-EXISTING DEFECT, fixed here: buf is ALREADY formatted by the StringCbVPrintfW above, and
    // _LogFormat is printf-style, so passing buf as the format made any literal '%' in a library
    // message read garbage off the varargs. Found while adding the hold above.
    _LogFormat(level, FALSE, function, L"%s", buf);
}

// Replay every held record. quiet=TRUE puts them at DEBUG (the outcome was ordinary); quiet=FALSE
// restores each record's ORIGINAL level, which is exactly what the log looked like before.
static void XifHoldRelease(BOOL quiet)
{
    g_XifHoldOn = FALSE;
    for (ULONG i = 0; i < g_XifHoldCount; i++)
    {
        XIFHOLD_REC *r = &g_XifHold[i];
        // "%s", text - NEVER text as the format. _LogFormat is printf-style and the held text is
        // ALREADY formatted (XifLogger ran StringCbVPrintfW before storing it), so passing it as a
        // format would make any '%' in a library message read garbage off the varargs.
        _LogFormat(quiet ? LOG_LEVEL_DEBUG : r->level, FALSE,
                   r->function[0] ? r->function : NULL, L"%s", r->text);
    }
    if (g_XifHoldLost)
    {
        // The wording used to say the OUTCOME was not classified from a complete record, which is
        // false on the path that reaches here after a SUCCESSFUL connect: the outcome is known and
        // the records were going to DEBUG anyway, so an overflow costs replay detail and nothing
        // else. It was measured firing on every successful connect at LogLevel 4. The failing path
        // reports its own overflow INSIDE the one error line, where it changes what the line means,
        // and clears the counter before it gets here.
        if (quiet)
            LogDebug("%lu record(s) past %d not replayed; connect succeeded",
                     g_XifHoldLost, XIFHOLD_MAX);
        else
            LogWarning("%lu record(s) past %d dropped",
                       g_XifHoldLost, XIFHOLD_MAX);
    }
    g_XifHoldCount = 0;
    g_XifHoldLost = 0;
}

// A failed vchan connect is ONE error line, not four restatements of it, and that line says in
// words what happened. Owner, 2026-10-09: keep it in the guest log, send NO dom0 message ("and
// certainly not four of them, each of misleading nature"), and "it is better to have diagnostic
// line which is interpretable by human like 'vchan prematurely closed by dom0'". The measured
// four-line sequence, why it is one fact restated by three layers, and why this collapse needs no
// quiet-mode detector are in findings/issues.md (QGAVCHANFAIL). Nothing is destroyed: every
// DISTINCT held text reaches the line with its repeat count, and every record is replayed verbatim
// at DEBUG.

static BOOL XifIsHexW(WCHAR c)
{
    return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
}

// WHAT THE LINE MAY SAY, and what it may not. The first draft of this said "vchan prematurely
// closed by dom0", which is the owner's own phrasing - and Jev refused it: wording_overclaims 0.81,
// classifier_sound 0.23, because a departed peer is an INFERENCE. What is OBSERVED is only that the
// xenstore node could not be read (status 0x5, which xeniface returns both for "no such node" and
// for "not permitted") or that the event channel could not be bound. So the line states the
// observation, in words, and says what follows from it. The KNOWN cause - a peer that departs
// mid-connect - is NOT asserted here and is recorded in findings/issues.md under this line's own
// QGAVCHANFAIL tag, which is how a reader gets from the line to the RCA. Five Jev rounds settled
// that: asserting the cause scored causal-hint-remains 0.94 and wording_overclaims 0.71-0.81,
// while naming no cause at all cost interpretability (0.55). Citing the register's PATH in the
// line was measured too and was worse on both counts (overclaim 0.41, repo_path_in_product_log
// 0.40, blocker 0.61), so the tag carries it instead.
typedef enum { XIFSHAPE_NONE = 0, XIFSHAPE_STORE, XIFSHAPE_EVTCHN, XIFSHAPE_OTHER } XIFSHAPE;

static XIFSHAPE XifHoldShape(const WCHAR **rec)
{
    ULONG i;

    *rec = NULL;
    if (g_XifHoldCount == 0)
        return XIFSHAPE_NONE;
    for (i = 0; i < g_XifHoldCount; i++)
    {
        // "ring-ref" ALONE matches the DEBUG records that merely name the path being read
        // ("Path: '<...>/ring-ref'") and the one that reports success ("ring-ref 45679,
        // event-channel 29"). Measured on a guest: at LogLevel 4 the shape test picked one of those
        // and the line then said the node "could not be read" while the records showed it HAD been
        // read, with its value. The failure record is the one that also says so.
        if (wcsstr(g_XifHold[i].text, L"ring-ref") && wcsstr(g_XifHold[i].text, L"failed"))
        {
            *rec = g_XifHold[i].text;
            return XIFSHAPE_STORE;
        }
    }
    for (i = 0; i < g_XifHoldCount; i++)
    {
        if (wcsstr(g_XifHold[i].text, L"failed to bind event channel"))
        {
            *rec = g_XifHold[i].text;
            return XIFSHAPE_EVTCHN;
        }
    }
    return XIFSHAPE_OTHER;
}

// The one piece of interpretation a reader genuinely cannot do unaided, and it is NOT an inference:
// xeniface returns 0x5 for a node that is ABSENT just as it does for one we may not read, so the
// status alone does not say which. The clause is added only when the record's status really is 0x5,
// so it can never describe a different status. ": 0x5" must not match ": 0x50".
static BOOL XifRecSays0x5(const WCHAR *rec)
{
    const WCHAR *p = rec ? wcsstr(rec, L": 0x5") : NULL;

    return (p != NULL) && !XifIsHexW(p[5]);
}

// Copy a library record so it can live INSIDE one line: drop the parenthesised handle values
// ("(000002B3CFBFBA90) ", 57 of the 397 characters of a real four-record sequence and meaningless to
// a reader), and fold every run of whitespace - including the record's OWN TRAILING NEWLINE - into a
// single space. MEASURED ON A GUEST 2026-10-09: the library's format strings end in "\n", so
// concatenating the records produced a "single" line that the log wrote as FOUR physical lines,
// three of them with no timestamp and no level prefix - which is worse to read than the four lines
// this replaces, and made the line look truncated. The HELD text is never modified, so the DEBUG
// replay still reproduces the library's own output verbatim.
static void XifCopyForLine(WCHAR *dst, size_t cb, const WCHAR *src)
{
    size_t n = cb / sizeof(WCHAR);
    size_t o = 0;
    const WCHAR *p = src;
    BOOL lastWasSpace = FALSE;

    while (*p && o + 1 < n)
    {
        if (*p == L'(')
        {
            const WCHAR *q = p + 1;
            while (XifIsHexW(*q))
                q++;
            if (*q == L')' && (q - p) >= 9)   // at least 8 hex digits: a handle, not prose
            {
                p = q + 1;
                while (*p == L' ')
                    p++;
                continue;
            }
        }
        if (*p == L'\r' || *p == L'\n' || *p == L'\t' || *p == L' ')
        {
            if (!lastWasSpace && o > 0)
            {
                dst[o++] = L' ';
                lastWasSpace = TRUE;
            }
            p++;
            continue;
        }
        lastWasSpace = FALSE;
        dst[o++] = *p++;
    }
    while (o > 0 && dst[o - 1] == L' ')   // no trailing space running into the next " | "
        o--;
    dst[o] = 0;
}

static void XifHoldReleaseCollapsed(int domain, int port)
{
    WCHAR line[2048];
    WCHAR part[1200];
    WCHAR clean[1024];
    WCHAR peer[96];
    const WCHAR *rec;
    XIFSHAPE shape;
    ULONG i, j, reps;
    ULONG shown;
    ULONG omitted = 0;
    BOOL seen;

    // Name the peer the way a reader thinks of it. It is NOT always dom0: a service requested by
    // another qube is a direct VM-to-VM vchan, and the server is that qube - measured, the failing
    // path was /local/domain/10858/data/vchan/13831/513, where 10858 is the CALLING qube and 13831
    // is this guest. Saying "dom0" there would be the misleading kind of line.
    if (domain == 0)
        StringCbCopyW(peer, sizeof(peer), L"dom0");
    else
        StringCbPrintfW(peer, sizeof(peer), L"domain %d", domain);

    // The sentence answers, in this order: what failed, who the other end was, what was actually
    // seen, and whether the reader has anything to do about it. Only the third part varies, and an
    // absence of records never becomes a claim about the shape.
    // THE LINE CARRIES WHAT WOULD HAVE BEEN PRINTED, not the whole hold. Owner 2026-10-09: "your
    // error messages are terrible in style: way too many words" - and most of the hold is the
    // libraries' own DEBUG bookkeeping ("Path: 'domid'", "Value: '13879'", "Watch handle: ..."),
    // which nothing printed before this code existed. Those stay in the DEBUG replay, where they
    // were always going. Nothing is lost at any level: below DEBUG they were never printed; at
    // DEBUG the replay carries every record verbatim.
    for (i = 0, shown = 0; i < g_XifHoldCount; i++)
    {
        if (g_XifHold[i].level <= LOG_LEVEL_WARNING)
            shown++;
    }

    shape = XifHoldShape(&rec);
    switch (shape)
    {
    case XIFSHAPE_STORE:
        StringCbPrintfW(line, sizeof(line),
                        L"QGAVCHANFAIL vchan to %s port %d never opened: xenstore node unreadable%s; request dropped.",
                        peer, port,
                        XifRecSays0x5(rec) ? L" (0x5 = absent or denied)"
                                           : L"");
        break;
    case XIFSHAPE_EVTCHN:
        StringCbPrintfW(line, sizeof(line),
                        L"QGAVCHANFAIL vchan to %s port %d never opened: event channel unbindable%s; request dropped.",
                        peer, port,
                        XifRecSays0x5(rec) ? L" (0x5 = gone or not ours)"
                                           : L"");
        break;
    case XIFSHAPE_NONE:
        StringCbPrintfW(line, sizeof(line),
                        L"QGAVCHANFAIL vchan to %s port %d never opened, libraries said nothing; request dropped.",
                        peer, port);
        break;
    default:
        StringCbPrintfW(line, sizeof(line),
                        L"QGAVCHANFAIL vchan to %s port %d never opened, cause not recognised; request dropped.",
                        peer, port);
        break;
    }
    if (shown > 0)
    {
        WCHAR head[64];
        StringCbPrintfW(head, sizeof(head), L" %lu/%lu records:", shown, g_XifHoldCount);
        (void)StringCbCatW(line, sizeof(line), head);
    }
    else if (g_XifHoldCount > 0)
    {
        WCHAR head[80];
        StringCbPrintfW(head, sizeof(head), L" %lu records held, all DEBUG - see replay", g_XifHoldCount);
        (void)StringCbCatW(line, sizeof(line), head);
    }
    for (i = 0; i < g_XifHoldCount; i++)
    {
        if (g_XifHold[i].level > LOG_LEVEL_WARNING)
            continue;   // the libraries' DEBUG bookkeeping: in the replay, not in this line

        seen = FALSE;
        for (j = 0; j < i; j++)
        {
            if (g_XifHold[j].level <= LOG_LEVEL_WARNING &&
                0 == wcscmp(g_XifHold[j].text, g_XifHold[i].text))
            {
                seen = TRUE;
                break;
            }
        }
        if (seen)
            continue;   // already in the line; repeats are counted on the first occurrence

        reps = 0;
        for (j = i; j < g_XifHoldCount; j++)
        {
            if (g_XifHold[j].level <= LOG_LEVEL_WARNING &&
                0 == wcscmp(g_XifHold[j].text, g_XifHold[i].text))
                reps++;
        }
        XifCopyForLine(clean, sizeof(clean), g_XifHold[i].text);
        if (reps > 1)
            StringCbPrintfW(part, sizeof(part), L" | %s (x%lu)", clean, reps);
        else
            StringCbPrintfW(part, sizeof(part), L" | %s", clean);
        // TRUNCATION IS NEVER SILENT. StringCbCatW truncates and returns
        // STRSAFE_E_INSUFFICIENT_BUFFER, and ignoring that would drop a fact from the one line that
        // is now the whole report - the exact loss this change exists to avoid. On overflow the
        // remaining DISTINCT texts are counted and named as omitted, and they are all still
        // replayed at DEBUG below, so nothing is unrecoverable.
        if (FAILED(StringCbCatW(line, sizeof(line), part)))
        {
            omitted++;
            continue;
        }
    }
    if (g_XifHoldLost)
    {
        // Said HERE, not by the release below: on this path an overflow means the one report was
        // built from an incomplete sequence, which a reader has to know to judge the diagnosis
        // above. The counter is cleared so the release does not report it a second time.
        WCHAR lost[160];
        StringCbPrintfW(lost, sizeof(lost),
                        L" | +%lu past %d dropped: INCOMPLETE",
                        g_XifHoldLost, XIFHOLD_MAX);
        (void)StringCbCatW(line, sizeof(line), lost);
        g_XifHoldLost = 0;
    }
    if (omitted)
    {
        WCHAR tail[128];
        StringCbPrintfW(tail, sizeof(tail), L" | +%lu did not fit (see DEBUG)",
                        omitted);
        // If even this does not fit the line is already at its bound; the DEBUG replay still carries
        // every record, and g_XifHoldLost covers anything the hold itself could not take.
        (void)StringCbCatW(line, sizeof(line), tail);
    }
    // (the empty-hold case is said in the opening sentence above, not appended here)

    // The same call shape as the replay: _LogFormat is printf-style and `line` is ALREADY formatted,
    // so it is an ARGUMENT to L"%s" and never the format itself.
    _LogFormat(LOG_LEVEL_ERROR, FALSE, "InitVchan", L"%s", line);

    // ...then the library's own sequence at DEBUG, which also clears the hold and its counters.
    XifHoldRelease(TRUE);
}

/**
 * @brief Create data vchan connection to the remote peer. Send MSG_HELLO if acting as server.
 * @param domain Remote vchan domain.
 * @param port Remote vchan port.
 * @param isServer Determines if we're acting as the vchan server.
 * @return Pointer to the vchan structure or NULL if failed.
 */
_Ret_maybenull_
libvchan_t *InitVchan(
    _In_ int domain,
    _In_ int port,
    _In_ BOOL isServer
    )
{
    libvchan_t *vchan;

    if (isServer)
    {
        vchan = libvchan_server_init(domain, port, VCHAN_BUFFER_SIZE, VCHAN_BUFFER_SIZE);
        if (!vchan)
        {
            LogError("libvchan_server_init(%d, %d) failed", domain, port);
            return NULL;
        }

        LogVerbose("server vchan: %p, waiting for data client", vchan);
        if (libvchan_wait(vchan) < 0)
        {
            LogError("libvchan_wait(%p) failed", vchan);
            libvchan_close(vchan);
            return NULL;
        }

        LogVerbose("remote peer (data client) connected");
        if (!VchanSendHello(vchan))
        {
            LogError("SendHelloToVchan(%p) failed", vchan);
            libvchan_close(vchan);
            return NULL;
        }

        LogDebug("vchan %p: hello sent", vchan);
    }
    else
    {
        // HELD while the connect runs - see XifLogger - and released in both branches below, so
        // no path can leave the hold on.
        g_XifHoldCount = 0;
        g_XifHoldLost = 0;
        g_XifHoldOn = TRUE;
        vchan = libvchan_client_init(domain, port);

        if (vchan)
        {
            // IT WORKED. Everything the libraries said on the way was a step that was retried and
            // then succeeded - including the existence probe whose own comment says it expects to
            // miss and wait again. At DEBUG. This is the whole of what this change makes quieter:
            // the stray ERROR lines a SUCCESSFUL connect to a slow peer writes today.
            XifHoldRelease(TRUE);
        }
        else
        {
            // A FAILED CONNECT IS STILL REPORTED AT ERROR - as ONE line, not four.
            //
            // WHAT IS STILL REFUSED, and why this is not it. The RCA behind the hold proposed
            // classifying a departed peer and logging it once at INFO, behind two detectors. Jev
            // refused both halves: ship_detector_b 0.16 (it needs a re-read through xencontrol, and
            // NOTHING in core-agent includes xencontrol.h or links it - the wrapper links exactly
            // libvchan.lib and windows-utils.lib - in a file that cannot be compiled on the dev
            // host, on the path of every qrexec call into the guest), and
            // detector_a_alone_sufficient 0.12, because A's verdict rests partly on which functions
            // did NOT appear, and an absence is exactly what the design rule "quiet requires two
            // agreeing POSITIVE detections" forbids as a basis for silence. Two later patches that
            // demoted these lines were refused three more times and retracted the same day.
            // ALL FIVE REFUSALS WERE OF BEING QUIET. None of them is in force here: the failure is
            // reported at ERROR with every distinct fact, and the level of nothing is lowered.
            XifHoldReleaseCollapsed(domain, port);
        }
    }

    return vchan;
}

/**
 * @brief Process vchan events, wait for process/threads exit.
 * @param child Child state.
 * @return Error code.
 */
DWORD EventLoop(
    _Inout_ PCHILD_STATE child
    )
{
    DWORD status = ERROR_NOT_ENOUGH_MEMORY;
    HANDLE waitObjects[3];
    DWORD signaled;
    BOOL run = TRUE;

    waitObjects[0] = libvchan_fd_for_select(child->Vchan);
    waitObjects[1] = child->Process;
    waitObjects[2] = g_StopEvent; // console-control shutdown (WrapperCtrlHandler); NULL if it could not be created

    // event loop
    while (run)
    {
        LogVerbose("waiting");
        signaled = WaitForMultipleObjects(g_StopEvent ? 3 : 2, waitObjects, FALSE, INFINITE) - WAIT_OBJECT_0;

        status = ERROR_INVALID_FUNCTION;

        switch (signaled)
        {
        case 0: // vchan data ready or disconnected
        {
            if (!libvchan_is_open(child->Vchan))
            {
                // DRAIN BEFORE LEAVING. A closed vchan does not mean an empty one: the peer
                // finishes sending and closes, and whatever is still sitting in the ring is data we
                // have simply not read yet. Exiting on the close alone DISCARDS it, and because it
                // depends on whether the close is noticed before or after the last read, it does so
                // intermittently - measured as ~1/3 of Content-Length-framed bodies arriving short
                // on the guest, an 80,043-byte file returned as anything from 0 to 79,389 bytes.
                //
                // Linux gets this right and says why. libqrexec/process_io.c leaves only when
                //     !libvchan_is_open(vchan) && !libvchan_data_ready(vchan) && !buffer_len(...)
                // i.e. closed AND nothing left to read AND nothing buffered - with the comment
                // "Exit the loop if vchan is disconnected (and we processed all incoming data).
                //  Check libvchan_is_open() before libvchan_data_ready() to avoid a race condition."
                // That is precisely the race this port has.
                DWORD drained = 0;
                while (VchanGetReadBufferSize(child->Vchan) > 0)
                {
                    if (HandleDataMessage(child) != ERROR_SUCCESS)
                        break;
                    drained++;
                }
                if (drained > 0)
                    LogDebug("vchan closed - drained %lu buffered message(s) before exit", drained);
                else
                    LogDebug("vchan closed");
                // This is the ORDINARY end of a guest-initiated call, so it must not leave the
                // ERROR_INVALID_FUNCTION set at the top of this pass: that was returned to wmain
                // and became the process exit code of every successful call.
                status = ERROR_SUCCESS;
                g_exitReason = EXIT_REASON_PEER_CLOSED;
                run = FALSE;
                break;
            }

            while (VchanGetReadBufferSize(child->Vchan) > 0)
            {
                status = HandleDataMessage(child);
                if (status != ERROR_SUCCESS)
                {
                    g_exitReason = EXIT_REASON_DATA_ERROR;
                    run = FALSE;
                }
            }
            break;
        }

        case 1: // child process terminated
        {
            DWORD exitCode;

            if (!GetExitCodeProcess(child->Process, &exitCode))
            {
                win_perror("GetExitCodeProcess");
                exitCode = 0;
            }

            LogDebug("child process exited with code %d", exitCode);

            // Wait for the i/o threads to DRAIN before sending the exit code.
            //
            // This used to wait only 1000 ms and then send the exit code regardless, merely
            // logging the timeout. Since the peer closes the vchan as soon as it receives the
            // exit code (see the comment in VchanSendExitCode), anything the stdout pump had not
            // yet written was silently DISCARDED - the response arrived short, under a normal
            // exit code, with nothing in the protocol to say so. Measured symptom: ~1/3 of
            // Content-Length-framed bodies short on the guest side; an 80,043-byte file returned
            // as anything from 0 to 79,389 bytes. A 64 KB vchan buffer and a slow reader make a
            // >1 s drain ordinary, so even small files reproduce it.
            //
            // The Linux implementation does not have this bug and shows the correct invariant:
            // in libqrexec/process_io.c, SIGCHLD only records the status and closes STDIN, and
            // send_exit_code() runs solely when stdin/stdout/stderr have all reached EOF -
            // data-driven, with NO timeout. The exit code is the last thing on the wire, always.
            //
            // So: wait for both pumps with no deadline. They exit on true EOF, and the write ends
            // are already closed here (see CreateChildPipes/StartChild), so EOF is guaranteed
            // unless a surviving grandchild still holds one - the same exposure Linux accepts.
            // A generous cap is kept purely as a hang backstop, and if it is ever hit that is a
            // TRUNCATED transfer and must be logged as an error, never as a routine timeout.
            // The drain ALSO wakes on a console-control stop (WrapperCtrlHandler). Measured 2026-09-29: a wrapper
            // whose child had exited but whose pipes a DETACHED grandchild still held sat in this wait; the stop
            // went unseen, the handler's 3 s ran out, and the process exited through the kernel's cleanup after
            // all. So wait on whichever i/o threads are still running plus the stop event, dropping each thread
            // as it finishes (a signalled handle left in the set would make the wait return at once forever).
            BOOL stdoutDone = FALSE, stderrDone = FALSE, stopped = FALSE;
            ULONGLONG drainEnd = GetTickCount64() + DRAIN_TIMEOUT_MS;
            status = WAIT_OBJECT_0;
            while (!(stdoutDone && stderrDone))
            {
                HANDLE drain[3];
                DWORD n = 0, stopIndex;
                if (!stdoutDone) drain[n++] = child->StdoutThread;
                if (!stderrDone) drain[n++] = child->StderrThread;
                stopIndex = n;
                if (g_StopEvent) drain[n++] = g_StopEvent;
                ULONGLONG now = GetTickCount64();
                DWORD left = now >= drainEnd ? 0 : (DWORD)(drainEnd - now);
                DWORD w = WaitForMultipleObjects(n, drain, FALSE, left);
                if (w == WAIT_TIMEOUT || w == WAIT_FAILED)
                {
                    status = w;
                    break;
                }
                DWORD idx = w - WAIT_OBJECT_0;
                if (g_StopEvent && idx == stopIndex)
                {
                    stopped = TRUE;
                    break;
                }
                if (drain[idx] == child->StdoutThread) stdoutDone = TRUE;
                else stderrDone = TRUE;
            }
            if (stopped)
            {
                LogWarning("stop requested by a console control event while draining - leaving now");
                status = ERROR_OPERATION_ABORTED;
                g_exitReason = EXIT_REASON_CTRL_STOP;
                CloseHandle(child->Process);
                child->Process = NULL;
                run = FALSE;
                break;
            }
            if (status == WAIT_TIMEOUT)
            {
                g_exitReason = EXIT_REASON_DRAIN_EXPIRED;
                LogError("i/o threads did not finish within %lu ms: the peer will close the vchan "
                         "on the exit code below, so this transfer is TRUNCATED", DRAIN_TIMEOUT_MS);
            }
            else if (status == WAIT_FAILED)
                win_perror2(GetLastError(), "wait for i/o threads");

            if (!VchanSendExitCode(child, exitCode))
                LogError("sending exit code failed");

            status = ERROR_SUCCESS;
            if (g_exitReason == EXIT_REASON_UNSET)
                g_exitReason = EXIT_REASON_CHILD_DRAINED;
            CloseHandle(child->Process);
            child->Process = NULL;
            run = FALSE;
            break;
        }

        case 2: // console-control shutdown: leave now, wmain's cleanup closes the vchan on this thread
        {
            LogWarning("stop requested by a console control event - leaving the event loop");
            status = ERROR_OPERATION_ABORTED;
            g_exitReason = EXIT_REASON_CTRL_STOP;
            run = FALSE;
            break;
        }
        }
    }

    // The pump thread handles stay open: wmain's cleanup checks them before freeing the state the pumps use.
    return status;
}

/**
 * @brief Print usage info.
 * @param name Executable name.
 */
void Usage(
    _In_ const PWSTR name
    )
{
    wprintf(L"Usage: %s domain%cport%cuser_name%cflags%ccommand_line\n", name, QUBES_ARGUMENT_SEPARATOR,
        QUBES_ARGUMENT_SEPARATOR, QUBES_ARGUMENT_SEPARATOR, QUBES_ARGUMENT_SEPARATOR);
    wprintf(L"domain:       remote domain for data vchan\n");
    wprintf(L"port:         remote port for data vchan\n");
    wprintf(L"user_name:    user name to use for the child process or (null) for current user\n");
    wprintf(L"flags:        bitmask of following possible values (decimal):\n");
    wprintf(L"         0x01 act as vchan server (default is client)\n");
    wprintf(L"         0x02 pipe child process' io to vchan (default is not)\n");
    wprintf(L"         0x04 run the child process in the interactive session (requires that a user is logged on)\n");
    wprintf(L"command_line: local program to execute and connect to data vchan or (null) if local program is not needed\n");
}

/**
 * @brief Entry point.
 * @param argc Number of command line arguments.
 * @param argv Expected arguments are: <domain> <port> <user_name> <flags> <command_line>
 *             domain:       remote domain for data vchan
 *             port:         remote port for data vchan
 *             user_name:    user name to use for the child process or (null) for current user
 *             flags:        bitmask of following possible values (decimal):
 *                      0x01 act as vchan server (default is client)
 *                      0x02 pipe child process' io to vchan (default is not)
 *                      0x04 run the child process in the interactive session (requires that a user is logged on)
 *             command_line: local program to execute and connect to data vchan
 * @return Error code.
 */
int wmain(int argc, WCHAR *argv[])
{
    UNREFERENCED_PARAMETER(argc);

    PCHILD_STATE child = NULL;
    int domain, port, flags;
    BOOL piped = FALSE, interactive;
    PWSTR domainName, portStr, flagsStr, userName, commandLine;
    DWORD status = ERROR_NOT_ENOUGH_MEMORY;
    BOOL startLocalProcess = TRUE;

    LogVerbose("start");

    domainName = GetArgument();
    portStr = GetArgument();
    userName = GetArgument();
    flagsStr = GetArgument();
    commandLine = GetArgument();

    if (!domainName || !portStr || !userName || !flagsStr || !commandLine)
    {
        Usage(argv[0]);
        return ERROR_INVALID_PARAMETER;
    }

    child = malloc(sizeof(CHILD_STATE));
    if (!child)
        goto cleanup;

    ZeroMemory(child, sizeof(*child));
    child->DataVersion = QREXEC_PROTOCOL_VERSION; // v2 limits until the peer's hello says otherwise

    InitializeCriticalSection(&g_VchanCs);

    // Before any vchan exists: a console control event from here on closes it through our own cleanup.
    g_StopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    g_ClosedEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!g_StopEvent || !g_ClosedEvent || !SetConsoleCtrlHandler(WrapperCtrlHandler, TRUE))
        LogWarning("console-control shutdown not armed (error 0x%x): a control event will exit without closing the data vchan",
                   GetLastError());
    libvchan_register_logger(XifLogger, LogGetLevel());

    domain = _wtoi(domainName);
    port = _wtoi(portStr);
    flags = _wtoi(flagsStr);

    child->IsVchanServer = !!(flags & 0x01);
    piped = !!(flags & 0x02);
    interactive = !!(flags & 0x04);

    LogDebug("domain %d, port %d, user %s, flags 0x%x, cmd '%s'", domain, port, userName, flags, commandLine);

    status = ERROR_INVALID_FUNCTION;
    child->Vchan = InitVchan(domain, port, child->IsVchanServer);
    if (!child->Vchan)
        goto cleanup;
    InterlockedExchange(&g_VchanOpen, 1);
    if (child->IsVchanServer)
        child->HelloSent = TRUE; // InitVchan sent the server's hello (it fails otherwise)

    if (wcscmp(userName, L"(null)") == 0)
        userName = NULL;
    if (wcsncmp(commandLine, L"(null)", 6) == 0)
        startLocalProcess = FALSE;

    if (!startLocalProcess)
    {
        status = ERROR_SUCCESS;
        BOOL run = TRUE;
        while (run)
        {
            HANDLE waits[2] = { libvchan_fd_for_select(child->Vchan), g_StopEvent };
            DWORD signaled = WaitForMultipleObjects(g_StopEvent ? 2 : 1, waits, FALSE, INFINITE);
            if (signaled == WAIT_OBJECT_0 + 1)
            {
                LogWarning("stop requested by a console control event");
                status = ERROR_OPERATION_ABORTED;
                goto cleanup;
            }
            if (signaled != WAIT_OBJECT_0)
            {
                status = (DWORD)-1;
                goto cleanup;
            }
            // vchan data ready or disconnected
            if (!libvchan_is_open(child->Vchan))
            {
                LogDebug("vchan closed");
                status = (DWORD)-1;
                break;
            }

            while (VchanGetReadBufferSize(child->Vchan) > 0)
            {
                status = HandleDataMessage(child);
                if (status != ERROR_SUCCESS)
                {
                    status = (DWORD)-2;
                    run = FALSE;
                    break;
                }
            }
            if (g_exitCodeReceived)
                break;
        }
        if (child)
        {
            if (child->Vchan)
            {
                libvchan_close(child->Vchan);
            }
            free(child);
            child = NULL;
        }
        goto cleanup;
    }

    status = CreatePublicPipeSecurityDescriptor(&child->PipeSd, &child->PipeAcl);
    if (ERROR_SUCCESS != status)
    {
        win_perror2(status, "create pipe security descriptor");
        goto cleanup;
    }

    status = StartChild(child, userName, commandLine, interactive, piped);
    if (ERROR_SUCCESS != status)
        goto cleanup;

    if (piped)
    {
        child->StdoutThread = CreateThread(NULL, 0, StdoutThread, child, 0, NULL);
        if (!child->StdoutThread)
        {
            status = win_perror("create stdout thread");
            goto cleanup;
        }

        child->StderrThread = CreateThread(NULL, 0, StderrThread, child, 0, NULL);
        if (!child->StderrThread)
        {
            status = win_perror("create stderr thread");
            goto cleanup;
        }

        status = EventLoop(child);
    }

cleanup:
    LogVerbose("exiting");

    if (child)
    {
        BOOL pumpsFinished = TRUE;

        if (child->Vchan)
        {
            if (libvchan_is_open(child->Vchan))
            {
                // send "exit code" (creation status really) if the io isn't piped or child creation failed
                if (!piped || status != ERROR_SUCCESS)
                {
                    // only if ours has not gone out yet - e.g. the child failed before the handshake; after data has
                    // flowed (a console-control stop, an i/o error) a second hello would be a protocol error
                    if (!child->HelloSent)
                        VchanSendHello(child->Vchan);
                    VchanSendExitCode(child, status);
                }
            }
            // ALWAYS tear the data vchan down ourselves, open or not. On the ordinary path the peer has
            // already hung up by now (it got the exit code), libvchan_is_open() says DISCONNECTED, and
            // this close used to be skipped - leaving the ring's grant mapping in OUR user address space
            // and the event channel for the kernel to reclaim during process exit, from a system worker
            // attached to the dying process. That exit-time unmap is the path the 2026-08-20 NMI dump
            // caught spinning on a single-target TLB shootdown, and per-call qrexec churn provokes the
            // guest stall (pre-registered A/Bs, 2026-09-28: SOAK 10/15 vs process-only churn 0/15; stock
            // QWT the same). Releasing it here does the unmap and the close from our own thread, while we
            // are alive.
            //
            // Under g_VchanCs, which every send holds: EventLoop returns with the output pumps STILL RUNNING when the
            // peer hangs up first, on a data error and on a console-control stop, and a pump reads child->Vchan under
            // that lock (VchanSendData) - so it finishes its send before this close or finds NULL after it, and never
            // touches a ring being unmapped. A pump waiting for room when the peer has stopped reading gives up on
            // g_VchanClosing within one poll, so taking the lock here never waits on the peer.
            InterlockedExchange(&g_VchanClosing, 1);
            EnterCriticalSection(&g_VchanCs);
            libvchan_close(child->Vchan);
            child->Vchan = NULL;
            InterlockedExchange(&g_VchanOpen, 0);
            LeaveCriticalSection(&g_VchanCs);
        }

        // A pump still running here keeps using `child` (its pipe, the pointer above) until it notices, so the state
        // is freed only when both are known to have finished; otherwise the process exit, next, reclaims it.
        if (child->StdoutThread)
        {
            if (WaitForSingleObject(child->StdoutThread, 0) != WAIT_OBJECT_0)
                pumpsFinished = FALSE;
            CloseHandle(child->StdoutThread);
        }
        if (child->StderrThread)
        {
            if (WaitForSingleObject(child->StderrThread, 0) != WAIT_OBJECT_0)
                pumpsFinished = FALSE;
            CloseHandle(child->StderrThread);
        }
        if (pumpsFinished)
            free(child);
        else if (g_exitReason == EXIT_REASON_PEER_CLOSED || g_exitReason == EXIT_REASON_CHILD_DRAINED)
            // Expected: the loop left on the ordinary end, which does not wait for the pumps. The
            // peer has what it asked for and the state is reclaimed by the process exit.
            LogDebug("an output pump is still running at exit - %s; its state is left to the process exit",
                     ExitReasonName(g_exitReason));
        else if (g_exitReason == EXIT_REASON_DRAIN_EXPIRED)
            // Already reported above as a TRUNCATED transfer; saying it twice buries the first one.
            LogDebug("an output pump is still running at exit - %s (reported above)",
                     ExitReasonName(g_exitReason));
        else
            // A data error, a stop, or a reason nobody recorded: output this call produced may
            // never have reached the peer, and nothing else says so.
            LogError("an output pump is still running at exit - %s: output this call produced may "
                     "not have reached the peer", ExitReasonName(g_exitReason));
    }
    if (g_ClosedEvent)
        SetEvent(g_ClosedEvent); // tells a waiting console-control handler that the vchan is released

    return status;
}

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

#include <windows.h>
#include <PathCch.h>
#include <Shlwapi.h>
#include <strsafe.h>
#include <wtsapi32.h>

#include <exec.h>
#include <qubesdb-client.h>
#include <log.h>
#include <config.h>
#include <qubes-io.h>

// FIXME this should be in qubesdb
#define QDB_PATH_PREFIX "/qubes-tools/"

// userName needs to be freed with WtsFreeMemory
BOOL GetCurrentUser(OUT char **userName)
{
    WTS_SESSION_INFOA *sessionInfo;
    DWORD sessionCount;
    DWORD i;
    DWORD cbUserName;
    BOOL found;

    LogVerbose("start");

    if (!WTSEnumerateSessionsA(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessionInfo, &sessionCount))
    {
        win_perror("WTSEnumerateSessionsA");
        return FALSE;
    }

    found = FALSE;

    for (i = 0; i < sessionCount; i++)
    {
        if (sessionInfo[i].State == WTSActive)
        {
            if (!WTSQuerySessionInformationA(
                WTS_CURRENT_SERVER_HANDLE,
                sessionInfo[i].SessionId, WTSUserName,
                userName,
                &cbUserName))
            {
                win_perror("WTSQuerySessionInformationA");
                goto cleanup;
            }
            LogDebug("Found session: %S\n", *userName);
            found = TRUE;
        }
    }

cleanup:
    WTSFreeMemory(sessionInfo);

    LogVerbose("found=%d", found);

    return found;
}

// append a binary exe name to the tools installation directory
BOOL PrepareExePath(OUT WCHAR *fullPath, DWORD pathSize, IN const WCHAR *exeName)
{
    DWORD status = CfgReadString(NULL, L"InstallDir", fullPath, pathSize, NULL);
    if (status != ERROR_SUCCESS)
    {
        win_perror2(status, "CfgReadString(InstallDir)");
        return FALSE;
    }

    LogVerbose("exe: '%s', install dir: '%s'", exeName, fullPath);

    if (FAILED(status = PathCchAppendEx(fullPath, pathSize, L"bin", PATHCCH_ALLOW_LONG_PATHS)))
    {
        win_perror2(status, "appending bin to path");
        return FALSE;
    }
    if (FAILED(status = PathCchAppendEx(fullPath, pathSize, exeName, PATHCCH_ALLOW_LONG_PATHS)))
    {
        win_perror2(status, "appending exe name to path");
        return FALSE;
    }

    LogVerbose("success, path: '%s'", fullPath);

    return TRUE;
}

/* TODO - make this configurable? */
BOOL CheckGuiAgentPresence(void)
{
    // this is a one-shot program, no need to cleanup allocations
    WCHAR* serviceFilePath = malloc(MAX_PATH_LONG_WSIZE);
    if (!serviceFilePath)
        return FALSE;

    LogVerbose("start");

    // FIXME hardcoded path
    if (!PrepareExePath(serviceFilePath, MAX_PATH_LONG, L"gui-agent.exe"))
        return FALSE;

    return PathFileExists(serviceFilePath);
}

BOOL NotifyDom0(void)
{
    STARTUPINFO si = { 0 };
    PROCESS_INFORMATION pi;
    WCHAR* qrexecClientVmPath = malloc(MAX_PATH_LONG_WSIZE);
    if (!qrexecClientVmPath)
        return FALSE;

    LogVerbose("start");

    // FIXME hardcoded path
    if (!PrepareExePath(qrexecClientVmPath, MAX_PATH_LONG, L"qrexec-client-vm.exe"))
        return FALSE;

    si.cb = sizeof(si);
    si.wShowWindow = SW_HIDE;
    si.dwFlags = STARTF_USESHOWWINDOW;

    WCHAR* cmdline = malloc(MAX_PATH_LONG_WSIZE);
    if (!cmdline)
        return FALSE;

    // FIXME hardcoded path
    DWORD status = StringCchPrintf(cmdline, MAX_PATH_LONG, L"qrexec-client-vm.exe dom0%cqubes.NotifyTools%c(null)%c(null)",
        QUBES_ARGUMENT_SEPARATOR, QUBES_ARGUMENT_SEPARATOR, QUBES_ARGUMENT_SEPARATOR);
    if (FAILED(status))
    {
        win_perror2(status, "formatting qrexec-client-vm command line");
        return FALSE;
    }

    LogDebug("Child command: %s", cmdline);
    if (!CreateProcess(
        qrexecClientVmPath,
        cmdline,
        NULL,
        NULL,
        FALSE,
        CREATE_NO_WINDOW,
        NULL,
        NULL,
        &si,
        &pi))
    {
        win_perror("CreateProcess(qrexec-client-vm.exe)");
        return FALSE;
    }

    /* fire and forget */
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    LogVerbose("success");

    return TRUE;
}

BOOL QdbWrite(qdb_handle_t qdb, char *path, char *value)
{
    return qdb_write(qdb, path, value, (int)strlen(value));
}

// Wait-for-logon diagnostics: first warning after 60 s, then every 5 min. The wait itself
// is NOT bounded on purpose - giving up would leave a slow-to-log-on guest permanently
// unadvertised (dom0 never sees qubes-tools/qrexec=1), which is worse than waiting.
#define LOGON_WAIT_FIRST_WARN_MS  (60 * 1000)
#define LOGON_WAIT_REPEAT_WARN_MS (5 * 60 * 1000)
// Used only if WTSRegisterSessionNotification is unavailable (anomaly, logged as error).
#define LOGON_WAIT_FALLBACK_POLL_MS ((DWORD)1000)

// Message-only window procedure: WM_WTSSESSION_CHANGE just wakes the wait loop below,
// which re-runs GetCurrentUser() as the single source of truth (same as wait-for-logon.c).
static LRESULT CALLBACK SessionWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_WTSSESSION_CHANGE)
    {
        LogDebug("WM_WTSSESSION_CHANGE event=%lu session=%lu", (ULONG)wParam, (ULONG)lParam);
        return 0;
    }
    return DefWindowProc(window, message, wParam, lParam);
}

// Blocks until an interactive session is active; *userName must be freed with WTSFreeMemory.
// Replaces the former `while (!GetCurrentUser()) Sleep(100)`: that spin enumerated WTS
// sessions at 10 Hz for the whole pre-logon phase and, when autologon did not fire, ran
// forever with nothing logged after "waiting for user logon" - dom0 saw "tools not present"
// with no guest-side trace of why. Now event-driven (WTSRegisterSessionNotification) with a
// periodic loud diagnostic so a stuck wait is visible in the log.
static void WaitForUserLogon(OUT char **userName)
{
    WNDCLASSEX wc = { 0 };
    HWND window = NULL;
    BOOL registered = FALSE;
    ULONGLONG start = GetTickCount64();
    ULONGLONG nextWarn = start + LOGON_WAIT_FIRST_WARN_MS;

    if (GetCurrentUser(userName))
        return;

    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = SessionWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = L"QubesAdvertiseToolsSessionWait";

    if (!RegisterClassEx(&wc))
    {
        win_perror("RegisterClassEx");
    }
    else
    {
        window = CreateWindowEx(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
        if (!window)
            win_perror("CreateWindowEx(HWND_MESSAGE)");
        else if (!WTSRegisterSessionNotification(window, NOTIFY_FOR_ALL_SESSIONS))
            win_perror("WTSRegisterSessionNotification");
        else
            registered = TRUE;
    }

    if (!registered)
    {
        // Fallback is an anomaly, not a mode: session notifications exist on every supported
        // guest (wait-for-logon.c relies on them). Say so loudly rather than absorb it.
        LogError("session change notification unavailable - falling back to polling every %lu ms; diagnose this",
            (ULONG)LOGON_WAIT_FALLBACK_POLL_MS);
    }

    // Re-check after registering: a logon completing between the probe above and the
    // registration produces no notification, and would otherwise be waited on forever.
    while (!GetCurrentUser(userName))
    {
        ULONGLONG now = GetTickCount64();
        DWORD timeout;

        if (now >= nextWarn)
        {
            LogWarning("still no active interactive session after %llu s - autologon not firing?",
                (now - start) / 1000);
            nextWarn = now + LOGON_WAIT_REPEAT_WARN_MS;
        }
        timeout = (DWORD)(nextWarn - now);

        if (registered)
        {
            // nCount=0: wake on posted/sent messages only, or on the diagnostic deadline.
            DWORD wait = MsgWaitForMultipleObjects(0, NULL, FALSE, timeout, QS_ALLINPUT);
            if (wait == WAIT_OBJECT_0)
            {
                MSG msg;
                while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
                {
                    TranslateMessage(&msg);
                    DispatchMessage(&msg);
                }
            }
            else if (wait == WAIT_FAILED)
            {
                win_perror("MsgWaitForMultipleObjects");
                Sleep(LOGON_WAIT_FALLBACK_POLL_MS);
            }
        }
        else
        {
            Sleep(timeout < LOGON_WAIT_FALLBACK_POLL_MS ? timeout : LOGON_WAIT_FALLBACK_POLL_MS);
        }
    }

    if (registered)
        WTSUnRegisterSessionNotification(window);
    if (window)
        DestroyWindow(window);
}

int wmain(int argc, WCHAR *argv[])
{
    qdb_handle_t qdb = NULL;
    ULONG status = ERROR_UNIDENTIFIED_ERROR;
    BOOL guiAgentPresent;
    BOOL qrexecAgentPresent = TRUE;
    CHAR *userName = NULL;

    if (argc < 2)
    {
        LogError("Usage: %s {0|1}", argv[0]);
        LogError("0 means tools are not installed");
        LogError("1 means tools are installed");
        return ERROR_BAD_ARGUMENTS;
    }

    if (argv[1][0] == '0')
    {
        LogDebug("setting tools presence to not installed");
        qrexecAgentPresent = FALSE;
        guiAgentPresent = FALSE;
    }
    else
    {
        guiAgentPresent = CheckGuiAgentPresence();
    }

    // advertise tools presence
    LogDebug("waiting for user logon");

    WaitForUserLogon(&userName);

    LogDebug("logged on user: %S", userName);

    // Open qubesdb only now, after the (unbounded) logon wait. It used to be opened before the
    // wait: a QdbDaemon restart during a slow logon left a dead pipe handle, every QdbWrite
    // below failed, and the guest stayed unadvertised for the boot. The parent (qrexec-agent)
    // already waited for qubesdb before launching us, so a failure here means qubesdb WAS up
    // and is now gone - a component loss, logged as such.
    qdb = qdb_open(NULL);
    if (!qdb)
    {
        win_perror("qdb_open");
        LogError("qubesdb was reachable when qrexec-agent launched us and is not now; tools presence NOT advertised, dom0 will not see /qubes-tools/qrexec=1");
        goto cleanup;
    }

    /* for now mostly hardcoded values, but this can change in the future */
    if (!QdbWrite(qdb, QDB_PATH_PREFIX "version", "1"))
    {
        win_perror("write 'version' entry");
        goto cleanup;
    }

    if (!QdbWrite(qdb, QDB_PATH_PREFIX "os", "Windows"))
    {
        win_perror("write 'os' entry");
        goto cleanup;
    }

    if (!QdbWrite(qdb, QDB_PATH_PREFIX "qrexec", qrexecAgentPresent ? "1" : "0"))
    {
        win_perror("write 'qrexec' entry");
        goto cleanup;
    }

    if (!QdbWrite(qdb, QDB_PATH_PREFIX "gui", guiAgentPresent ? "1" : "0"))
    {
        win_perror("write 'gui' entry");
        goto cleanup;
    }

    if (!QdbWrite(qdb, QDB_PATH_PREFIX "gui-emulated", guiAgentPresent ? "0" : "1"))
    {
        win_perror("write 'gui-emulated' entry");
        goto cleanup;
    }

    if (!QdbWrite(qdb, QDB_PATH_PREFIX "default-user", userName))
    {
        win_perror("write 'default-user' entry");
        goto cleanup;
    }

    if (!NotifyDom0())
    {
        /* error already reported */
        goto cleanup;
    }

    status = ERROR_SUCCESS;
    LogVerbose("success");

cleanup:
    if (qdb)
        qdb_close(qdb);
    if (userName)
        WTSFreeMemory(userName);
    return status;
}

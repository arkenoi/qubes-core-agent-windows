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
#include <stdio.h>
#include <stdlib.h>
#include <wtsapi32.h>
#include <shlwapi.h>
#include <strsafe.h>

#include <qubes-io.h>
#include <qubesdb-client.h>
#include <utf8-conv.h>
#include <log.h>
#include <config.h>

WCHAR *g_expectedUser;
BOOL g_sessionFound = FALSE;
BOOL g_mismatchSaid = FALSE;

// The loud half of a user mismatch (owner, 2026-11-17: "it probably should give a loud warning
// when user does not match"). A log line is not loud: qrexec-wrapper writes the same text on every
// user service of such a guest, and the reporter's 26H2 logs held 118 of them that nobody saw. So
// the mismatch goes to dom0 through the product's ONE error route, qwt-notify-error.ps1 in bin
// (docs/DESIGN-error-notify.md): ACTION, the service.notify-errors gate, one notification per boot
// for this id, the per-boot cap and the redaction all live there and none of it is copied here.
// The text is templated - the route refuses values read from the system - so the two account names
// stay in this program's log line. Started hidden, in bin, and NOT waited for: the wait never
// depends on it. The route's outcome is logged to wait-for-logon-notify-<date>.log via bin\log.ps1,
// as the installer's notices log theirs: an ERROR unless it was sent or is this boot's duplicate.
// No angle-bracket placeholders in the text: dom0's notification server may read them as markup.
// Jev on this transport (0.67) and on once per boot (0.56) was under the floor - not verdicts; the
// route was chosen because it already holds every rule a second copy would have to repeat.
// TWO MESSAGES SHARE IT (2026-11-17): the id, code and three texts are arguments, so the qubesdb
// difference reported from wWinMain takes this same route rather than a second copy of it (owner:
// "it means you should not design a new mechanism for it"). Every caller passes compile-time
// constants with no quote or angle bracket, so the command line is built here and nothing read
// from the system reaches it. CheckSession's call passes today's id, code and texts unchanged.
static void NotifyUserMismatch(const WCHAR *id, const WCHAR *code, const WCHAR *header,
                               const WCHAR *next, const WCHAR *cause)
{
    WCHAR binDir[MAX_PATH] = { 0 };
    WCHAR powershell[MAX_PATH] = { 0 };
    WCHAR commandLine[4096];
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    DWORD status;
    HRESULT hr;

    hr = StringCchPrintfW(commandLine, RTL_NUMBER_OF(commandLine),
        L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \""
        L"try { . .\\log.ps1; $QwtNotifyLog = { param($m) $null = Write-QwtLogLine I $m -Module 'wait-for-logon-notify' } } catch { }; "
        L"$s = 'unavailable:helper-absent'; "
        L"if (Test-Path -LiteralPath .\\qwt-notify-error.ps1) { try { . .\\qwt-notify-error.ps1; "
        L"$ev = 'the wait-for-logon log'; try { $ev = Join-Path (Get-QwtLogDir) ('wait-for-logon-' + (Get-Date -Format yyyyMMdd) + '.log') } catch { }; "
        L"$s = Send-QwtError -Component 'wait-for-logon' -Id '%s' -Severity ACTION "
        L"-Header '%s' "
        L"-Next '%s' "
        L"-Cause '%s' "
        L"-Tech (Format-QwtNotifyTechLine -Subject 'wait-for-logon.exe' -Code '%s' -Count 'reported once per boot' -Evidence $ev) "
        L"} catch { $s = 'failed: ' + $_.Exception.Message } }; "
        L"$lv = 'I'; $tail = ''; if ($s -ne 'send' -and $s -ne 'suppressed:duplicate') { $lv = 'E'; $tail = ' - dom0 was NOT notified of the user mismatch; the wait-for-logon log carries it' }; "
        L"try { $null = Write-QwtLogLine $lv ('dom0 notification wait-for-logon.%s: ' + $s + $tail) -Module 'wait-for-logon-notify' } catch { }\"",
        id, header, next, cause, code, id);
    if (FAILED(hr))
    {
        LogError("the notification command line for '%s' does not fit its buffer (0x%x) - dom0 is NOT notified "
                 "of the user mismatch", id, hr);
        return;
    }

    status = CfgReadString(NULL, L"InstallDir", binDir, RTL_NUMBER_OF(binDir), NULL);
    if (status != ERROR_SUCCESS)
    {
        win_perror2(status, "CfgReadString(InstallDir) - dom0 is NOT notified of the user mismatch");
        return;
    }
    if (!PathAppendW(binDir, L"bin"))
    {
        LogError("the tools bin directory does not fit a path - dom0 is NOT notified of the user mismatch");
        return;
    }
    if (!ExpandEnvironmentStringsW(L"%SystemRoot%\\System32\\WindowsPowerShell\\v1.0\\powershell.exe",
                                   powershell, RTL_NUMBER_OF(powershell)))
    {
        win_perror("ExpandEnvironmentStrings(powershell) - dom0 is NOT notified of the user mismatch");
        return;
    }
    if (!CreateProcessW(powershell, commandLine, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, binDir, &si, &pi))
    {
        win_perror("CreateProcess(powershell) - dom0 is NOT notified of the user mismatch");
        return;
    }
    LogInfo("user mismatch: the dom0 notification route started (powershell pid %lu, not waited for; "
            "its outcome goes to wait-for-logon-notify-<date>.log)", pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

BOOL CheckSession(DWORD	sessionId)
{
    WCHAR *userName;
    DWORD cbUserName;

    if (!WTSQuerySessionInformation(WTS_CURRENT_SERVER_HANDLE, sessionId, WTSUserName, &userName, &cbUserName))
    {
        win_perror("WTSQuerySessionInformation");
        return FALSE;
    }

    LogDebug("Found session: %s", userName);

    // THE NAME MUST MATCH - the stock rule, restored (owner, 2026-11-17, reverting 5bf6052): this
    // wait returns only for an active session whose account is EXACTLY the name dom0 sends, its
    // `default_user`. 5bf6052 let any active session end it, after the GWeck report (post 219) that
    // dom0's app menu started nothing on a new 26H2 template - dom0 sent 'user', his only account is
    // 'Weck', and `qvm-run --service` sends qubes.StartApp only after qubes.WaitForSession returns.
    // He has since said he changed the account himself, so a mismatch is a setting to correct in
    // dom0 (qvm-prefs <qube> default_user <account>), not a case to absorb here. What changes from
    // stock is that it is no longer silent: the first mismatched session of this call is said in
    // the log, naming both accounts, and reported to dom0 (NotifyUserMismatch above).
    if (wcscmp(userName, g_expectedUser) == 0)
    {
        g_sessionFound = TRUE;
    }
    else if (userName[0] != L'\0' && !g_mismatchSaid)
    {
        g_mismatchSaid = TRUE;
        LogWarning("requested user '%s' is not the account logged on interactively ('%s') - this wait "
                   "returns only when '%s' signs in, so dom0's app menu starts nothing in this qube "
                   "until its default_user names this guest's account", g_expectedUser, userName,
                   g_expectedUser);
        NotifyUserMismatch(L"default-user-mismatch", L"QWTUSERMISMATCH",
            L"App menu entries start nothing in this qube",
            L"They wait for the account that dom0 names as this qube default_user, and that account is not the one signed in here. Fix it in dom0 with qvm-prefs: set this qube default_user to the name of your Windows account.",
            L"Cause: default_user does not name the Windows account signed in to this qube.");
    }

    WTSFreeMemory(userName);
    return g_sessionFound;
}

BOOL CheckIfUserLoggedIn(void)
{
    WTS_SESSION_INFO *sessionInfo;
    DWORD sessionCount;
    DWORD sessionIndex;

    if (!WTSEnumerateSessions(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessionInfo, &sessionCount))
    {
        win_perror("WTSEnumerateSessions");
        return FALSE;
    }

    for (sessionIndex = 0; sessionIndex < sessionCount; sessionIndex++)
    {
        if (sessionInfo[sessionIndex].State == WTSActive)
        {
            if (CheckSession(sessionInfo[sessionIndex].SessionId))
            {
                g_sessionFound = TRUE;
            }
        }
    }

    WTSFreeMemory(sessionInfo);
    return g_sessionFound;
}

LRESULT CALLBACK WindowProc(
    HWND window,     // handle to window
    UINT message,    // WM_WTSSESSION_CHANGE
    WPARAM wParam,   // session state change event
    LPARAM lParam    // session ID
    )
{
    switch (message)
    {
    case WM_WTSSESSION_CHANGE:
        if (wParam == WTS_SESSION_LOGON)
        {
            if (CheckSession((DWORD) lParam))
                g_sessionFound = TRUE;
        }
        return TRUE;

    default:
        return DefWindowProc(window, message, wParam, lParam);
    }
}

HWND CreateMainWindow(HINSTANCE instance)
{
    WNDCLASSEX wc;
    ATOM windowClassAtom;

    wc.cbSize = sizeof(wc);
    wc.style = 0;
    wc.lpfnWndProc = (WNDPROC) WindowProc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = instance;
    wc.hIcon = NULL;
    wc.hCursor = NULL;
    wc.hbrBackground = (HBRUSH) COLOR_WINDOW;
    wc.lpszMenuName = NULL;
    wc.lpszClassName = L"MainWindowClass";
    wc.hIconSm = NULL;

    windowClassAtom = RegisterClassEx(&wc);
    if (!windowClassAtom)
    {
        win_perror("RegisterClassEx");
        return NULL;
    }

    return CreateWindow(
        wc.lpszClassName, /* class */
        TEXT("Qubes session wait service"), /* name */
        WS_OVERLAPPEDWINDOW, /* style */
        CW_USEDEFAULT, CW_USEDEFAULT, /* x,y */
        10, 10, /* w, h */
        HWND_MESSAGE, /* parent */
        NULL, /* menu */
        instance, /* instance */
        NULL);
}

// THE EXPECTED NAME COMES FROM QUBESDB, AS ON LINUX (owner, 2026-11-17: "did you implement the plan
// for matching linux core-agent to read qubesdb entries?", "write qubesdb parity decision to ADR,
// implement, do not overengineer around dom0 bubble", and, verbatim: "no new bubble" does not mean
// you should not send dom0 notification, it means you should not design a new mechanism for it).
// Linux qubes.WaitForSession (qubes-core-qrexec, qubes-rpc-base/qubes.WaitForSession) waits for
//     user="$(qubesdb-read /default-user || echo 'user')"
// and never looks at what the caller sent. Here the caller's name is still read, exactly as before,
// because it is the fallback and the thing compared.
//
// THE READ ADDS NO VCHAN TRAFFIC: the Windows qubesdb client reaches the LOCAL daemon over a named
// pipe (qdb-client.c connect_to_daemon, QDB_DAEMON_LOCAL_PATH), and the daemon answers a read from
// its own copy (daemon/db-cmds.c handle_read); only a local client's writes and removals are
// duplicated through the vchan to dom0 (handle_write, handle_rm).
//
// A DIFFERENCE IS REPORTED: when the caller sent a name and it is not the qubesdb one, this program
// waits for the qubesdb name, says both in a W line and reports it to dom0 through the existing
// route (NotifyUserMismatch, id default-user-changed) - Jev's verdict, log_and_report 0.85. The
// route already dedupes per id per boot; nothing of that is added here.
//
// NOT JEV VERDICTS, chosen here: ONE qdb_open, no retry, no thread, no bound - Jev leaned unbounded
// (0.53) at confidence 0.30, and the owner's rule is no timeouts as fixes and no retry in hope.
// advertise-tools opens it the same way: both run under qrexec-agent, which serves nothing before
// its own WaitForQdb has seen qubesdb answer. A daemon alive but not answering holds the call
// (measured 2026-10-24 on win11r-sup; the comment above WaitForQdb in qrexec-agent.c).
// When qubesdb cannot be read the wait uses the CALLER'S name - Jev leaned 0.50 at 0.33 - because
// for dom0's callers that name IS the default_user dom0 holds, while a literal 'user' would strand
// every qube whose default_user is not 'user'. 'user' is used only when the caller sent nothing.
// Returns a static buffer, or NULL after an E line that says which step failed.
static WCHAR *ReadQdbDefaultUser(void)
{
    static WCHAR userName[CONVERT_MAX_BUFFER_LENGTH];
    qdb_handle_t qdb;
    char *value;
    unsigned int cbValue = 0;
    WCHAR *result = NULL;
    DWORD status;

    qdb = qdb_open(NULL);
    if (!qdb)
    {
        win_perror("qdb_open failed - qubesdb /default-user is not read; the wait uses the name the caller sent, or 'user'");
        return NULL;
    }

    value = qdb_read(qdb, "/default-user", &cbValue);
    if (!value)
    {
        LogError("qubesdb /default-user returned nothing: the key is absent or the read failed - the wait "
                 "uses the name the caller sent, or 'user'");
        goto cleanup;
    }

    // strip end of line and spaces, as for the name read from stdin
    while (cbValue > 0 &&
        (value[cbValue - 1] == '\n' ||
        value[cbValue - 1] == '\r' ||
        value[cbValue - 1] == ' '))
        cbValue--;
    value[cbValue] = '\0';

    if (cbValue == 0)
    {
        LogError("qubesdb /default-user is empty - the wait uses the name the caller sent, or 'user'");
        goto cleanup;
    }

    status = ConvertUTF8ToUTF16(value, userName, NULL);
    if (status != ERROR_SUCCESS)
    {
        win_perror2(status, "converting qubesdb /default-user to UTF-16 failed - the wait uses the name the caller sent, or 'user'");
        goto cleanup;
    }
    result = userName;

cleanup:
    free(value);
    qdb_close(qdb);
    return result;
}

int APIENTRY wWinMain(_In_ HINSTANCE hInst, _In_opt_ HINSTANCE hPrevInst, _In_ WCHAR *pszCommandLine, _In_ int nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInst);
    UNREFERENCED_PARAMETER(nCmdShow);

    HWND hMainWindow;
    size_t cbExpectedUserUtf8;
    size_t cchExpectedUser = 0;
    char pszExpectedUserUtf8[USERNAME_LENGTH + 1];
    HANDLE hStdIn;
    WCHAR *qdbUser;

    LogVerbose("start");
    // first try command parameter
    g_expectedUser = PathGetArgs(pszCommandLine);

    // if none was given, read from stdin
    if (!g_expectedUser || g_expectedUser[0] == TEXT('\0'))
    {
        LogDebug("reading user from stdin");
        hStdIn = GetStdHandle(STD_INPUT_HANDLE);
        if (hStdIn == INVALID_HANDLE_VALUE)
        {
            win_perror("GetStdHandle");
            return 1;
        }

        cbExpectedUserUtf8 = QioReadUntilEof(hStdIn, pszExpectedUserUtf8, sizeof(pszExpectedUserUtf8));
        if (cbExpectedUserUtf8 == 0)
        {
            LogError("Failed to read the user name");
            return 1;
        }

        // strip end of line and spaces
        // FIXME: what about multibyte char with one byte '\r'/'\n'/' '?
        while (cbExpectedUserUtf8 > 0 &&
            (pszExpectedUserUtf8[cbExpectedUserUtf8 - 1] == '\n' ||
            pszExpectedUserUtf8[cbExpectedUserUtf8 - 1] == '\r' ||
            pszExpectedUserUtf8[cbExpectedUserUtf8 - 1] == ' '))
            cbExpectedUserUtf8--;
        pszExpectedUserUtf8[cbExpectedUserUtf8] = '\0';

        // this call uses a static output buffer but we only call it here
        if (ERROR_SUCCESS != ConvertUTF8ToUTF16Static(pszExpectedUserUtf8, &g_expectedUser, &cchExpectedUser))
        {
            win_perror("Converting user name to UTF16");
            return 1;
        }
    }

    // the qubesdb name, as Linux waits for (ReadQdbDefaultUser above)
    qdbUser = ReadQdbDefaultUser();
    if (qdbUser)
    {
        if (g_expectedUser[0] != L'\0' && wcscmp(g_expectedUser, qdbUser) != 0)
        {
            LogWarning("the caller sent user '%s' but this qube's qubesdb /default-user is '%s' - waiting for "
                       "'%s', as Linux qubes.WaitForSession does", g_expectedUser, qdbUser, qdbUser);
            NotifyUserMismatch(L"default-user-changed", L"QWTUSERCHANGED",
                L"App menu entries wait for a different account than dom0 named",
                L"This qube waits for the default_user in its own qubesdb, which differs from the name dom0 sent with this call. If default_user was changed in dom0 while this qube runs, restart the qube.",
                L"Cause: the name dom0 sent differs from this qube qubesdb default-user.");
        }
        g_expectedUser = qdbUser;
    }
    else if (g_expectedUser[0] == L'\0')
    {
        LogError("qubesdb /default-user is unavailable and the caller sent no user name - waiting for 'user'");
        g_expectedUser = L"user";
    }

    LogDebug("creating window");
    hMainWindow = CreateMainWindow(hInst);

    if (hMainWindow == INVALID_HANDLE_VALUE)
    {
        win_perror("Creating main window");
        return 1;
    }

    if (!WTSRegisterSessionNotification(hMainWindow, NOTIFY_FOR_ALL_SESSIONS))
    {
        win_perror("WTSRegisterSessionNotification");
        DestroyWindow(hMainWindow);
        return 1;
    }

    g_sessionFound = FALSE;
    if (!CheckIfUserLoggedIn())
    {
        BOOL bRet;
        MSG msg;

        LogDebug("Waiting for user '%s'", g_expectedUser);
        while ((bRet = GetMessage(&msg, hMainWindow, 0, 0)) != 0)
        {
            if (bRet == -1)
            {
                // handle the error and possibly exit
                break;
            }
            else
            {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
            if (g_sessionFound)
                break;
        }
    }

    WTSUnRegisterSessionNotification(hMainWindow);
    DestroyWindow(hMainWindow);

    return 0;
}

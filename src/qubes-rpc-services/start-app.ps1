<#
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
 #>

# THE CAUSE of "Run Terminal / File Manager do nothing on a Windows qube" is NOT in this file:
# dom0's per-qube launchers are wired to two FIXED desktop-entry ids that every Linux qube gets
# from qubes-core-agent-linux (app-menu/Makefile) - `qubes-run-terminal` and
# `qubes-open-file-manager` - and a Windows guest emitted neither, so they pointed at nothing.
# Both ids are handled below and emitted by get-appmenus.ps1.
#
# The rest of this file was hardened at the same time. Be honest about which parts were real:
#   MEASURED on win10-app - `Start-Process -Wait` blocked the service for the app's whole
#     lifetime (child still running after 25 s), and `explorer.exe` with no argument gives the
#     shell's own empty-title Progman window instead of a browser window.
#   HARDENING, not observed here - the log.ps1 dot-source guard and the bounded logon loop.
#     QUBES_TOOLS is set machine-wide and a session existed, so neither was the cause. Do not
#     cite them as the fix.
#
# WHY 1 - the dot-source on the first line was unguarded:
#     . $env:QUBES_TOOLS\qubes-rpc-services\log.ps1
# With QUBES_TOOLS unset in the service environment that expands to `. \qubes-rpc-services\
# log.ps1`, which THROWS, and the script is dead before it has looked at its argument.
# get-appmenus.ps1 carries an explicit warning about exactly this trap and was hardened against
# it; start-app.ps1 never was, although it is the half that actually launches things.
$logPs1 = Join-Path $PSScriptRoot 'log.ps1'
if (-not (Test-Path -LiteralPath $logPs1) -and $env:QUBES_TOOLS) {
    $logPs1 = Join-Path $env:QUBES_TOOLS 'qubes-rpc-services\log.ps1'
}
if (Test-Path -LiteralPath $logPs1) { try { . $logPs1 } catch { } }
if (-not (Get-Command LogDebug -ErrorAction SilentlyContinue)) {
    function LogDebug($m) { }
    function LogInfo($m) { }
    function LogWarning($m) { }
    function LogError($m) { }
}

# Anything written to stderr comes back to the CALLER over qrexec, so a failure is visible in
# dom0 instead of being a menu entry that does nothing. Log AND print.
function Fail($msg) {
    LogError $msg
    [Console]::Error.WriteLine("qubes.StartApp: $msg")
    exit 1
}

# Registry location for path hash -> full path mapping
$RegistryMapPath = 'HKCU:\Software\Invisible Things Lab\Qubes Tools'
$RegistryMapKey = 'AppMap'

$desktopBaseName = $args[0]
if (-not $desktopBaseName) { Fail 'no application id given' }

# BUILT-IN ENTRIES (see get-appmenus.ps1). A fresh Windows guest has almost nothing in its Start
# Menu, and in seamless mode there is no taskbar and no desktop either, so without these the qube
# has no way in at all. Resolved here rather than through the AppMap registry lookup below,
# because these are not shortcuts and because the map lives in HKCU - which is the wrong hive when
# this service happens to run as anything but the logged-on user.
$edge = @(
    "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe",
    "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe"
) | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1

# The folder "Open File Manager" should show. NOT %USERPROFILE%: this service does not always
# run as the logged-on user, and when it runs as SYSTEM that variable is
# C:\Windows\system32\config\systemprofile - measured, the window opened titled "systemprofile".
# Resolve the INTERACTIVE user's folder, which is what the Linux entry's `xdg-open .` means.
function Resolve-HomeFolder {
    try {
        $u = (Get-CimInstance Win32_ComputerSystem -ErrorAction Stop).UserName
        if ($u) {
            $p = Join-Path "$env:SystemDrive\Users" $u.Split('\')[-1]
            if (Test-Path -LiteralPath $p) { return $p }
        }
    } catch { }
    if ($env:USERPROFILE -and (Test-Path -LiteralPath $env:USERPROFILE)) { return $env:USERPROFILE }
    return "$env:SystemDrive\"
}

# "Run Terminal", mirroring the Linux agent's /usr/bin/qubes-run-terminal, which tries a list of
# emulators and execs the first one present rather than hardcoding one. The Windows order puts
# Windows Terminal first because it is the only one that is a real tabbed terminal; the rest are
# fallbacks that always exist.
function Resolve-Terminal {
    foreach ($cand in 'wt.exe', 'pwsh.exe', 'powershell.exe', 'cmd.exe') {
        $c = Get-Command $cand -ErrorAction SilentlyContinue
        if ($c) { return $c.Source }
    }
    return "$env:SystemRoot\System32\cmd.exe"
}

$builtin = @{
    'notepad'          = @{ File = "$env:SystemRoot\System32\notepad.exe"; Elevated = $false }
    # WHY 4 - File Explorer. `explorer.exe` with NO argument asks the ALREADY RUNNING shell to do
    # something and the new process exits immediately; whether a window appears depends on shell
    # state. Naming a folder makes it deterministic: it always opens a browser window on it.
    'explorer'         = @{ File = "$env:SystemRoot\explorer.exe"; Args = @((Resolve-HomeFolder)); Elevated = $false }
    'settings'         = @{ File = 'ms-settings:';                          Elevated = $false }
    # THE TWO FIXED IDS dom0's per-qube launchers are wired to. Names come from
    # qubes-core-agent-linux/app-menu (qubes-run-terminal.desktop,
    # qubes-open-file-manager.desktop) and must match exactly - get-appmenus.ps1 emits them.
    'qubes-run-terminal'      = @{ File = (Resolve-Terminal);              Elevated = $false }
    'qubes-open-file-manager' = @{ File = "$env:SystemRoot\explorer.exe"; Args = @((Resolve-HomeFolder)); Elevated = $false }
    'cmd'              = @{ File = "$env:SystemRoot\System32\cmd.exe";      Elevated = $false }
    'cmd-admin'        = @{ File = "$env:SystemRoot\System32\cmd.exe";      Elevated = $true  }
    'powershell'       = @{ File = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"; Elevated = $false }
    'powershell-admin' = @{ File = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"; Elevated = $true  }
}
if ($edge) { $builtin['edge'] = @{ File = $edge; Elevated = $false } }

# WHY 2 - this loop had NO exit. An app can only start into a session that exists, but if no
# session ever appears (or Get-CimInstance throws), the old `while (!UserName) { sleep 100ms }`
# span forever: the qrexec call hangs and the menu entry does nothing, with no error anywhere.
# Bounded, and it says which exit it took.
$deadline = (Get-Date).AddSeconds(60)
while ($true) {
    $who = $null
    try { $who = (Get-CimInstance -ClassName Win32_ComputerSystem -ErrorAction Stop).UserName } catch { }
    if ($who) { break }
    if ((Get-Date) -gt $deadline) {
        Fail 'no interactive session after 60s - nothing can be displayed, refusing to hang'
    }
    LogDebug 'Waiting for user logon'
    Start-Sleep -Milliseconds 200
}

# WHY 3 - `-Wait` held the qrexec service open for the WHOLE LIFETIME of the launched app. dom0's
# app menu just wants the thing started; the Linux qubes.StartApp does not wait either. Waiting
# also means one stuck app blocks its qrexec connection indefinitely, and it is why a manual
# `qrexec-client-vm <vm> qubes.StartApp+cmd` appears to hang. Launch and return.
try {
    if ($builtin.ContainsKey($desktopBaseName)) {
        $b = $builtin[$desktopBaseName]
        $splat = @{ FilePath = $b.File }
        if ($b.Args) { $splat['ArgumentList'] = $b.Args }
        if ($b.Elevated) {
            # -Verb RunAs, i.e. the ordinary Windows elevation prompt, deliberately: a menu entry
            # that silently handed out admin would be a hole, and since 4.3.11 the consent dialog
            # is an ordinary window in dom0 (PromptOnSecureDesktop=0), so it can be answered.
            LogDebug "Starting $($b.File) ELEVATED"
            Start-Process @splat -Verb RunAs -ErrorAction Stop
        } else {
            LogDebug "Starting $($b.File)"
            Start-Process @splat -ErrorAction Stop
        }
        exit 0
    }
} catch {
    Fail "could not start built-in '$desktopBaseName' ($($b.File)): $($_.Exception.Message)"
}

# WHY 5 - a missing AppMap value threw an unhandled terminating error, so a stale menu entry (an
# app whose shortcut has since gone) silently did nothing. Note the map lives in HKCU, which is
# the wrong hive whenever this service does not run as the logged-on user - report that clearly
# rather than dying.
$target = $null
try {
    $prop = Get-ItemProperty -Path "$RegistryMapPath\$RegistryMapKey" -Name $desktopBaseName -ErrorAction Stop
    $target = $prop.$desktopBaseName
} catch {
    Fail "'$desktopBaseName' is not a built-in and has no AppMap entry under $RegistryMapPath\$RegistryMapKey (HKCU of the service's user) - re-run qvm-sync-appmenus, or the shortcut is gone"
}
if (-not $target) { Fail "'$desktopBaseName' resolved to an empty target" }

LogDebug "Starting $target"
try { Start-Process -FilePath $target -ErrorAction Stop }
catch { Fail "could not start '$target': $($_.Exception.Message)" }
exit 0

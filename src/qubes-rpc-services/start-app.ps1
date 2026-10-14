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
# HKCU FIRST, then the machine mirror. HKCU is this qube's own sync and is the freshest thing we
# have; HKLM is inherited from the template through the root volume and is what makes a TEMPLATE
# sync reach an appvm at all (APPMAPWRONGHIVE). Read-only here - this handler never writes either.
$RegistryMapPathMachine = 'HKLM:\Software\Invisible Things Lab\Qubes Tools'

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
# $mapHit, not just $target: in PowerShell both $null and '' are falsy, so testing $target alone
# cannot tell "no AppMap entry for this name" from "the entry is there and empty". The two have
# different answers - re-sync this qube, versus a bad map value - and reporting the first for the
# second sends the operator to run a sync that will not help.
$mapHit = $false
$mapFrom = ''
$mapEmptyIn = ''
foreach ($hive in @($RegistryMapPath, $RegistryMapPathMachine)) {
    try {
        $prop = Get-ItemProperty -Path "$hive\$RegistryMapKey" -Name $desktopBaseName -ErrorAction Stop
        $v = $prop.$desktopBaseName
        # AN ENTRY THAT IS PRESENT BUT EMPTY MUST NOT END THE SEARCH. Taking the first hive that
        # merely HAS the name lets a blank HKCU value shadow a good entry in the inherited mirror -
        # and get-image-rgba.c, the other reader of this map, correctly moves on in that case, so
        # the two consumers would have disagreed about the same name. Remember where the blank was
        # so the "resolved to an empty target" diagnosis is still available if no hive answers.
        if ([string]::IsNullOrWhiteSpace($v)) { if (-not $mapEmptyIn) { $mapEmptyIn = $hive }; continue }
        $target = $v
        $mapHit = $true
        $mapFrom = $hive
        break
    } catch { }
}
if (-not $mapHit -and $mapEmptyIn) {
    Fail "'$desktopBaseName' has an AppMap entry in $mapEmptyIn\$RegistryMapKey but it is EMPTY, and no other hive carries a usable one - the map was written wrong, so re-run qvm-sync-appmenus for THIS qube"
}

# WHY 7 - THE MAP IS READ IN A DIFFERENT QUBE FROM THE ONE IT IS WRITTEN IN, and the fix is NOT
# to scan at launch time.
#
# dom0 builds a template-based qube's application list from its TEMPLATE, so get-appmenus.ps1 runs
# there and writes AppMap into the template's user hive, while qubes.StartApp runs in the APPVM
# against the appvm's own hive. MEASURED on win11-app 2026-10-11, the owner launching Calculator
# from the menu on a cold qube:
#   [13:29:33] StartChild: Running '...start-app.ps1 "Calculator"' as user 'user'
#   [13:29:53] 'Calculator' is not a built-in and has no AppMap entry under HKCU:\...\AppMap
# The qube started and nothing appeared. Its user hive did hold 93 entries - a sync HAD run there -
# but from before the store-app scan existed, so Calculator was not among them.
#
# A LAUNCH-TIME FALLBACK WAS TRIED HERE AND REMOVED. Resolving the id by enumerating installed
# packages took longer than 200 s on this guest and returned nothing, leaving a powershell process
# running and the caller hanging - for a menu click that is indistinguishable from the silent
# failure this is all about, so it is a worse defect than the one it patched. The enumeration
# belongs in the sync, where it is paid once and where it already is. The answer to a stale map is
# to RE-SYNC THAT QUBE, which the message below says in those words.
if (-not $mapHit) {
    Fail "'$desktopBaseName' is not a built-in and has no AppMap entry in either hive: $RegistryMapPath\$RegistryMapKey (the HKCU of '$env:USERNAME', this qube's own sync) or $RegistryMapPathMachine\$RegistryMapKey (the machine mirror inherited from the template) - run qvm-sync-appmenus for THIS qube, or re-sync the template so the mirror carries it, or the app is gone"
}
# WHICH hive answered is the diagnostic that distinguishes "this qube is synced" from "it is
# running on the template's inherited mirror", and the two have different staleness.
LogDebug "AppMap hit for '$desktopBaseName' in $mapFrom"
if (-not $target) { Fail "'$desktopBaseName' resolved to an empty target" }

LogDebug "Starting $target"
# WHY 6 - A UWP APP HAS NO FILE TO START. Everything above assumes the AppMap value is a path
# (a .lnk, or an .exe), because the scan only ever wrote .lnk paths. A modern Windows app -
# Calculator, Photos, Terminal, the Store - has no .lnk anywhere: it exists only in AppsFolder
# under its AUMID, so it could not be reported by the scan and could not be launched here.
# MEASURED on win11-app 2026-10-11: the guest's Start Menu holds 41 shortcuts, every one a legacy
# or admin tool, and none of them Calculator - while the package itself is installed and runnable
# (STATUS Ok, USER user state=Installed). The owner's words: "there is no calculator entry in
# start menu, to begin with".
# The moniker is handed to explorer, which is what performs the activation; Start-Process on the
# moniker itself is interpreted as a file path and fails.
try {
    if ($target -like 'shell:AppsFolder\*') {
        LogDebug "AppsFolder activation: $target"
        Start-Process -FilePath 'explorer.exe' -ArgumentList $target -ErrorAction Stop
    } else {
        Start-Process -FilePath $target -ErrorAction Stop
    }
}
catch { Fail "could not start '$target': $($_.Exception.Message)" }
exit 0

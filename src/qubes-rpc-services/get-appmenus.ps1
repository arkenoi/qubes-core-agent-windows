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

# THIS SERVICE MUST NOT FAIL. dom0 (qubesappmenus/receive.py) treats a non-zero exit as fatal:
#   if p.returncode != 0: raise QubesException("Error getting application list")
# and qvm-sync-appmenus then exits 1 with no application list at all - the failure a user sees as
# "Refresh applications ... returned non-zero exit status 1". A single throwing statement anywhere
# in here - an absent Start Menu folder, a shortcut that cannot be read, a registry write denied -
# used to take the whole sync down with it. Every stage is now guarded and the script always exits
# 0: reporting FEWER apps is a bad day, reporting NONE is a broken qube.
#
# Two more constraints from that same parser, both silent when violated:
#   * it decodes each line as ASCII and DROPS lines that are not - so a shortcut named in Cyrillic
#     or with a curly apostrophe loses its Name line and the entry is discarded with a warning;
#   * it reads at most 1024 bytes per line.
# Emit-Entry enforces both.
$ErrorActionPreference = 'Continue'

# log.ps1 sits next to this script. Do not depend on %QUBES_TOOLS% being set in the service
# environment: if it is not, dot-sourcing "\qubes-rpc-services\log.ps1" throws and the sync dies
# before it has read a single shortcut.
$logPs1 = Join-Path $PSScriptRoot 'log.ps1'
if (-not (Test-Path -LiteralPath $logPs1) -and $env:QUBES_TOOLS) {
    $logPs1 = Join-Path $env:QUBES_TOOLS 'qubes-rpc-services\log.ps1'
}
if (Test-Path -LiteralPath $logPs1) {
    try { . $logPs1 } catch { }
}
if (-not (Get-Command LogDebug -ErrorAction SilentlyContinue)) {
    function LogDebug($m) { }
    function LogInfo($m) { }
    function LogWarning($m) { }
    function LogError($m) { }
}

# Registry location for path hash -> full path mapping for GetImageRGBA
$RegistryMapPath = 'HKCU:\Software\Invisible Things Lab\Qubes Tools'
$RegistryMapKey = 'AppMap'

try { New-Item -Path $RegistryMapPath -Name $RegistryMapKey -Force -ErrorAction Stop | Out-Null }
catch { LogWarning "could not create the AppMap key: $($_.Exception.Message)" }

try { $WshShell = New-Object -ComObject 'WScript.Shell' -ErrorAction Stop }
catch { $WshShell = $null; LogWarning "WScript.Shell unavailable: $($_.Exception.Message)" }

$Sha1 = [System.Security.Cryptography.SHA1]::Create()
Function GetHash($string)
{
    $hash = [BitConverter]::ToString($Sha1.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($string))).Replace("-", "")
    return $hash.ToLowerInvariant()
}

$script:EmittedIds = @{}
$script:EmittedNames = @{}
$script:RecommendedIds = @()

# ------------------------------------------- which entries a fresh qube should have ENABLED
#
# NOT which are reported. Everything found is reported, always - see the note in Sweep. These
# folders are the ones whose contents are system administration consoles rather than applications,
# and the only thing they shape is the RECOMMENDED DEFAULT SELECTION this script writes to its log,
# which an admin can hand to dom0 as `menu-items`. Availability is untouched.
#
# MEASURED on win11de-qwt (German 25H2, 26200.8037) 2026-10-07: the sweep below reported 38
# entries and TWENTY of them - more than half the menu - came from one Start Menu folder,
# "Administrative Tools": Component Services, Computer Management, dfrgui, Disk Cleanup, Event
# Viewer, iSCSI Initiator, Memory Diagnostics Tool, ODBC Data Sources (32-bit), ODBC Data Sources
# (64-bit), Performance Monitor, Print Management, RecoveryDrive, Registry Editor, Resource
# Monitor, Security Configuration Management, services, System Configuration, System Information,
# Task Scheduler, Windows Defender Firewall with Advanced Security.
#
# That is what a FRESHLY CREATED qube shows, because dom0 enables everything available until
# somebody makes a selection, and it never takes that selection from the guest. So the first thing
# a user saw in a new Windows qube's menu was twenty MMC snap-ins with the real applications buried
# among them.
#
# THE FIX IS THE DEFAULT SELECTION, NOT THE REPORT. An earlier version of this file dropped these
# twenty from the report, which also dropped them from dom0's AVAILABLE list, which meant nobody
# could ever tick them in Settings -> Applications again. The owner rejected that, correctly: "you
# silently dropped most of the apps and tell me that it is fine?" Availability is the user's choice
# and this service does not get to make it. All of them are reported; the list below only decides
# what the logged recommendation leaves OUT.
#
# The folder name is matched LITERALLY, which is correct on a localized Windows: the measurement
# above is a German guest and its paths still read
# "...\Start Menu\Programs\Administrative Tools\..." - Windows localizes the DISPLAYED folder
# name through desktop.ini and leaves the directory name alone.
$script:ExcludedMenuFolders = @(
    'Administrative Tools'          # Win11, and Win10 1809+
    'Windows Administrative Tools'  # the same folder's Win10 1703-1803 spelling
)

# Pure. Exactly what Emit-Entry will put on the wire for a value. dom0 decodes each line as ASCII
# and DROPS the line if it is not, and reads at most 1024 bytes of it - see the header.
Function Get-QwtSafeValue($value)
{
    $safe = ("$value" -replace '[^\x20-\x7E]', '')
    if ($safe.Length -gt 400) { $safe = $safe.Substring(0, 400) }
    return $safe
}

# Pure. The key an id is remembered under, so an id inserted by one path and looked up by another
# is the SAME key. The two sides disagreed: scanned entries inserted a sanitised, lower-cased id
# while the built-in check looked its own up raw - it only ever worked because every built-in id
# happens to be lower-case already.
Function Get-QwtIdKey($id)
{
    return (("$id" -replace '[^a-zA-Z0-9._-]', '_')).ToLowerInvariant()
}

# Pure. The desktop-entry NAME dom0 will end up with - what Emit-Entry prints before the colon, and
# therefore the only string `menu-items` can be matched against (receive.py keys the whitelist on
# os.path.basename). Shared with Emit-Entry rather than re-derived: the recommendation below was
# built from the RAW id and so named "Windows_PowerShell-Windows_PowerShell_ISE_(x86).desktop",
# while the line actually emitted reads "..._ISE__x86_.desktop". `menu-items` ignores a name it
# cannot match, silently, so an admin pasting that value would have lost that one entry from the
# menu with nothing to indicate why. Measured on the guest 2026-10-07.
Function Get-QwtDesktopName($id)
{
    $safe = ("$id" -replace '[^a-zA-Z0-9._-]', '_')
    if ($safe -notlike '*.desktop') { $safe = "$safe.desktop" }
    return $safe
}

# Pure. The key a DISPLAYED name is remembered under: what dom0 will actually receive, lower-cased.
# Going through Get-QwtSafeValue is the point, not tidiness - two names are the same menu label iff
# their keys match. Keying on the RAW name let two shortcuts emit one identical label while the
# collision check saw two different strings, because the umlaut it compared on never reaches dom0:
# "Zubehor" with an o-umlaut arrives as "Zubehr". On a German guest that is the norm, not an edge.
Function Get-QwtNameKey($name)
{
    return (Get-QwtSafeValue $name).ToLowerInvariant()
}

# Pure. The path of a shortcut RELATIVE to the Start Menu root it was found under.
Function Get-QwtMenuRelativePath($fullPath, $basePath)
{
    $b = "$basePath".TrimEnd('\', '/')
    if ($b.Length -gt 0 -and $fullPath.Length -gt ($b.Length + 1) -and
        $fullPath.Substring(0, $b.Length) -eq $b) {
        return $fullPath.Substring($b.Length + 1)
    }
    return [System.IO.Path]::GetFileName($fullPath)
}

# Pure. Is this shortcut in a folder that holds system administration consoles rather than
# applications? Only DIRECTORY components are considered - a file's own name never excludes it.
Function Test-QwtMenuExcluded($relativePath)
{
    if (-not $relativePath) { return $false }
    $parts = @($relativePath -split '[\\/]')
    for ($i = 0; $i -lt ($parts.Count - 1); $i++) {
        foreach ($ex in $script:ExcludedMenuFolders) {
            if ($parts[$i] -eq $ex) { return $true }
        }
    }
    return $false
}

# Pure. The label the user sees. The folder prefix belongs in the desktop-entry ID, where it makes
# the id unique - putting it in the NAME is what produced, in the same measurement,
# "Windows PowerShell Windows PowerShell ISE", "Accessories-System Tools Character Map" and
# "Administrative Tools dfrgui". Use the shortcut's own name, and fall back to the prefixed form
# only when a different shortcut already took it, so two entries are never labelled identically.
Function Get-QwtMenuName($relativePath, $baseName, $takenNames)
{
    $candidates = @("$baseName")
    $parts = @("$relativePath" -split '[\\/]')
    if ($parts.Count -gt 1) {
        $dir = ($parts[0..($parts.Count - 2)] -join '-')
        if ($dir) { $candidates += "$dir $baseName" }
    }
    if (-not $takenNames) { return $candidates[0] }
    foreach ($c in $candidates) {
        if (-not $takenNames.ContainsKey((Get-QwtNameKey $c))) { return $c }
    }
    # Every form is taken, which has a real cause and not only a contrived one: two shortcuts whose
    # names differ ONLY in characters dom0 never receives are one single label by the time they
    # arrive - "Zubehor" with an o-umlaut and "Zubehr" both reach the menu as "Zubehr", and a
    # shortcut at the Programs root has no folder to fall back on either. Number it rather than
    # emit a second entry the user cannot tell apart. Bounded, because this service must not hang.
    $last = $candidates[$candidates.Count - 1]
    for ($n = 2; $n -le 99; $n++) {
        $c = "$last ($n)"
        if (-not $takenNames.ContainsKey((Get-QwtNameKey $c))) { return $c }
    }
    return $last
}

# Pure. Would reporting this built-in duplicate something the Start Menu sweep already reported?
# ID *or* the name the user would see - the id-only test let "Microsoft Edge" into the menu TWICE
# in the 2026-10-07 measurement: "Programs\Microsoft Edge.lnk" is id Microsoft_Edge and the
# built-in is id edge, so the ids never collided, while both lines read Name=Microsoft Edge and
# the entries were indistinguishable in dom0's menu.
Function Test-QwtBuiltinRedundant($id, $name, $emittedIds, $emittedNames)
{
    if ($emittedIds -and $emittedIds.ContainsKey((Get-QwtIdKey $id))) { return 'id' }
    if ($emittedNames -and $emittedNames.ContainsKey((Get-QwtNameKey $name))) { return 'name' }
    return ''
}

# One output line, ASCII-only and length-capped, because dom0 silently discards anything else.
Function Emit-Entry($id, $key, $value)
{
    # Stock emits "<name>.desktop:Key=Value" and dom0's parser makes the suffix optional, so both
    # shapes work - but the whole point of this fork is that its output is indistinguishable from
    # stock except where we mean it. Keep the suffix.
    $safeId = Get-QwtDesktopName $id
    Write-Host "$($safeId):$key=$(Get-QwtSafeValue $value)"
}

Function Set-AppMap($name, $value)
{
    try { New-ItemProperty -Path "$RegistryMapPath\$RegistryMapKey" -Name $name -PropertyType String -Value $value -Force -ErrorAction Stop | Out-Null }
    catch { LogWarning "AppMap write failed for '$name': $($_.Exception.Message)" }
}

# Extract data from one shortcut file
Function ProcessLink($pathObj, $basepath)
{
    try {
        $relativePath = Get-QwtMenuRelativePath $pathObj.FullName $basepath
        $desktopFileName = $relativePath.Replace(' ', '_').Replace('\','-')
        $linkBaseName = $desktopFileName.Replace('.lnk', '.desktop') # fixme: check if it's at the end of the string

        $description = ''
        if ($WshShell) {
            try { $description = $WshShell.CreateShortcut($pathObj.FullName).Description } catch { }
        }
        $targetPath = "cmd.exe /c `"$($pathObj.FullName)`""
        # We send .LNK file hash as icon name since the name can't contain some characters that can be in a file path
        # and the GetImageRGBA Qubes service needs to retrieve bitmap from this name alone.
        $targetHash = GetHash($pathObj.FullName)

        # Store the hash-LNK mapping in the registry for easy retrieval by GetImageRGBA.
        Set-AppMap $targetHash $pathObj.FullName
        # Store also basename-LNK location for qubes.StartApp service
        Set-AppMap $desktopFileName.Replace('.lnk', '') $pathObj.FullName

        LogDebug "$targetHash -> $targetPath"

        $id = $linkBaseName.Replace('.desktop','')
        $menuName = Get-QwtMenuName $relativePath $pathObj.BaseName $script:EmittedNames
        Emit-Entry $id 'Name' $menuName
        Emit-Entry $id 'Exec' $targetPath.Replace('\','\\')
        Emit-Entry $id 'Comment' $description
        Emit-Entry $id 'Icon' $targetHash
        $script:EmittedIds[(Get-QwtIdKey $id)] = $true
        $script:EmittedNames[(Get-QwtNameKey $menuName)] = $true
        # reported either way; only the RECOMMENDATION skips the administration folders
        if (-not (Test-QwtMenuExcluded $relativePath)) { $script:RecommendedIds += (Get-QwtDesktopName $id) }
    } catch {
        # One unreadable shortcut must not cost the user every other application.
        LogWarning "skipping shortcut '$($pathObj.FullName)': $($_.Exception.Message)"
    }
}

# todo: check if there are duplicated entries

Function Sweep($folderName)
{
    if (-not $WshShell) { return }
    try {
        $p = $WshShell.SpecialFolders.item($folderName)
        if (-not $p -or -not (Test-Path -LiteralPath $p)) {
            LogWarning "start menu folder '$folderName' not present - skipping"
            return
        }
        # EVERY shortcut is reported. What this service prints becomes dom0's AVAILABLE list
        # (qubesappmenus get_available_filenames: one .desktop template per entry reported), and
        # both selection paths are list comprehensions OVER that list - `menu-items` and
        # whitelisted-appmenus.list can only ever NARROW it, never add to it. So an entry we do not
        # report is one nobody can ever tick in Settings -> Applications.
        #
        # This loop briefly EXCLUDED the Administrative Tools folder - 20 of the 28 shortcuts on the
        # measured guest - to make a fresh qube's menu usable. That was the wrong fix and the owner
        # rejected it: "you silently dropped most of the apps and tell me that it is fine?" The
        # problem was never which apps are AVAILABLE, it was which are ENABLED BY DEFAULT, and the
        # lever for that is dom0's `menu-items` / `default-menu-items` - a stated prerequisite,
        # exactly like `vmexec` and `qrexec_timeout` already are. See docs/QVM-FEATURES.md.
        $shortcuts = @(Get-ChildItem -Path $p -Filter '*.lnk' -Recurse -ErrorAction SilentlyContinue)
        foreach ($s in $shortcuts) { ProcessLink $s $p }
    } catch {
        LogWarning "sweep of '$folderName' failed: $($_.Exception.Message)"
    }
}

Sweep 'AllUsersPrograms'   # "All users" menu
Sweep 'StartMenu'          # Current user menu

# Pinned Start Menu items
# FIXME: this doesn't work in win10 and there seems to be no official way of getting pinned tiles

# ---------------------------------------------------------------- built-in entries
#
# A freshly installed Windows guest has almost nothing in its Start Menu that the sweep above
# finds, so dom0's application list comes up nearly empty and the qube looks unusable: in seamless
# mode there is no taskbar and no desktop either, so the app menu is the ONLY way in. Report a
# small fixed set that every Windows has, so a new qube is usable the moment it is synced.
#
# The two "(Administrator)" entries launch through the ordinary Windows elevation prompt - see
# start-app.ps1. They deliberately do NOT use any silent-elevation trick: a menu entry that
# quietly hands out admin would be a hole, and since 4.3.11 the consent dialog is an ordinary
# window in dom0, so answering it is normal.
try {
    $edge = @(
        "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe",
        "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe"
    ) | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1

    # Icon for the Run Terminal entry: whatever start-app.ps1 will actually launch, so the icon
    # matches the app rather than always showing cmd.exe's.
    $terminalIcon = @(
        (Get-Command 'wt.exe' -ErrorAction SilentlyContinue).Source,
        (Get-Command 'pwsh.exe' -ErrorAction SilentlyContinue).Source,
        "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe",
        "$env:SystemRoot\System32\cmd.exe"
    ) | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1

    $builtins = @(
        @{ id = 'notepad';          name = 'Notepad';                            icon = "$env:SystemRoot\System32\notepad.exe";  comment = 'Text editor' }
        @{ id = 'explorer';         name = 'File Explorer';                      icon = "$env:SystemRoot\explorer.exe";          comment = 'Browse files in this qube' }
        @{ id = 'settings';         name = 'Settings';                           icon = "$env:SystemRoot\ImmersiveControlPanel\SystemSettings.exe"; comment = 'Windows settings' }
        # THE TWO FIXED IDS. dom0's per-qube launchers are wired to specific desktop-entry
        # names that qubes-core-agent-linux installs on every Linux qube (app-menu/Makefile:
        # qubes-run-terminal.desktop and qubes-open-file-manager.desktop). A Windows guest
        # emits neither, so those launchers have nothing to point at and simply do nothing -
        # which is the whole reason "Run Terminal" and the file manager did not work here.
        # The IDs must match EXACTLY; the names below are the upstream ones verbatim.
        @{ id = 'qubes-run-terminal';     name = 'Run Terminal';      icon = $terminalIcon;                  comment = 'Terminal - Windows Terminal if installed, otherwise PowerShell or Command Prompt' }
        @{ id = 'qubes-open-file-manager'; name = 'Open File Manager'; icon = "$env:SystemRoot\explorer.exe"; comment = 'Open File Explorer in this qube' }
        @{ id = 'cmd';              name = 'Command Prompt';                     icon = "$env:SystemRoot\System32\cmd.exe";      comment = 'Command Prompt' }
        @{ id = 'cmd-admin';        name = 'Command Prompt (Administrator)';     icon = "$env:SystemRoot\System32\cmd.exe";      comment = 'Command Prompt, elevated - Windows will ask for confirmation' }
        @{ id = 'powershell';       name = 'Windows PowerShell';                 icon = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"; comment = 'Windows PowerShell' }
        @{ id = 'powershell-admin'; name = 'Windows PowerShell (Administrator)'; icon = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"; comment = 'Windows PowerShell, elevated - Windows will ask for confirmation' }
    )
    if ($edge) {
        $builtins += @{ id = 'edge'; name = 'Microsoft Edge'; icon = $edge; comment = 'Web browser' }
    }

    # The sweep ran first, so where a built-in and a real shortcut collide the guest's own shortcut
    # wins - which keeps its localized description, and it launches through AppMap exactly like
    # every other scanned entry.
    foreach ($b in $builtins) {
        $redundant = Test-QwtBuiltinRedundant $b.id $b.name $script:EmittedIds $script:EmittedNames
        if ($redundant) {
            LogDebug "built-in '$($b.id)' already provided by a Start Menu shortcut (same $redundant) - skipping"
            continue
        }
        # The icon source doubles as the AppMap value, exactly like a .lnk path does, so
        # qubes.GetImageRGBA can pull the icon out of the executable.
        $iconSource = $b.icon
        if (-not (Test-Path -LiteralPath $iconSource)) { $iconSource = "$env:SystemRoot\System32\shell32.dll" }
        $hash = GetHash($iconSource)
        Set-AppMap $hash $iconSource
        # start-app.ps1 has its own table for these, but keep the mapping so the lookup path that
        # every other entry uses works for them too.
        Set-AppMap $b.id $iconSource

        Emit-Entry $b.id 'Name' $b.name
        Emit-Entry $b.id 'Exec' "qubes-rpc-multiplexer qubes.StartApp+$($b.id)"
        Emit-Entry $b.id 'Comment' $b.comment
        Emit-Entry $b.id 'Icon' $hash
        $script:EmittedIds[(Get-QwtIdKey $b.id)] = $true
        $script:EmittedNames[(Get-QwtNameKey $b.name)] = $true
        $script:RecommendedIds += (Get-QwtDesktopName $b.id)
    }
} catch {
    LogWarning "built-in entries failed: $($_.Exception.Message)"
}

# ---------------------------------------------------------- the recommended default selection
#
# To the LOG, never to stdout: dom0 parses stdout and prints "Warning: ignoring key" for anything
# it does not recognise, so a recommendation on stdout would be noise in the admin's own output.
#
# Everything above is AVAILABLE. This line says which of it is worth having ENABLED on a fresh
# qube, and dom0 is the only place that can be set (qubesappmenus reads `menu-items` on the qube,
# else `default-menu-items` inherited from its template, else shows everything available; it never
# takes a default from the guest). It is a stated prerequisite, like `vmexec` and `qrexec_timeout`
# - we ship nothing into dom0 and run nothing there. docs/QVM-FEATURES.md has the whole story.
try {
    $rec = @($script:RecommendedIds | Sort-Object -Unique)
    $all = $script:EmittedIds.Count
    if ($rec.Count -gt 0 -and $rec.Count -lt $all) {
        LogInfo "MENU-RECOMMENDATION $($rec.Count) of $all reported entries are applications; the rest are system administration consoles. All $all stay AVAILABLE. For a clean default menu, in dom0: qvm-features <qube> menu-items '$($rec -join ' ')'  (or default-menu-items on its template, which new AppVMs inherit)"
    } else {
        LogInfo "MENU-RECOMMENDATION all $all reported entries look like applications - no default selection needed"
    }
} catch {
    LogWarning "could not compose the menu recommendation: $($_.Exception.Message)"
}

# ALWAYS 0. See the header: a non-zero exit here is what dom0 turns into
# "qvm-sync-appmenus ... returned non-zero exit status 1", losing the whole application list.
exit 0

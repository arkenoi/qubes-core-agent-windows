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

# One output line, ASCII-only and length-capped, because dom0 silently discards anything else.
Function Emit-Entry($id, $key, $value)
{
    # Stock emits "<name>.desktop:Key=Value" and dom0's parser makes the suffix optional, so both
    # shapes work - but the whole point of this fork is that its output is indistinguishable from
    # stock except where we mean it. Keep the suffix.
    $safeId = (($id -replace '[^a-zA-Z0-9._-]', '_'))
    if ($safeId -notlike '*.desktop') { $safeId = "$safeId.desktop" }
    $safeValue = ($value -replace '[^\x20-\x7E]', '')
    if ($safeValue.Length -gt 400) { $safeValue = $safeValue.Substring(0, 400) }
    Write-Host "$($safeId):$key=$safeValue"
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
        $desktopFileName = ($pathObj.FullName).SubString($basepath.Length+1).Replace(' ', '_').Replace('\','-')
        $linkBaseName = $desktopFileName.Replace('.lnk', '.desktop') # fixme: check if it's at the end of the string
        if ($pathObj.DirectoryName -ne $basepath) {
            $appmenuLocation = $pathObj.DirectoryName.SubString($basepath.Length+1).Replace('\','-') + " "
        } else {
            $appmenuLocation = ""
        }

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
        Emit-Entry $id 'Name' "$appmenuLocation$($pathObj.BaseName)"
        Emit-Entry $id 'Exec' $targetPath.Replace('\','\\')
        Emit-Entry $id 'Comment' $description
        Emit-Entry $id 'Icon' $targetHash
        $script:EmittedIds[($id -replace '[^a-zA-Z0-9._-]', '_').ToLowerInvariant()] = $true
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
        $shortcuts = Get-ChildItem -Path $p -Filter '*.lnk' -Recurse -ErrorAction SilentlyContinue
        $shortcuts | ForEach-Object { ProcessLink $_ $p }
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

    $builtins = @(
        @{ id = 'notepad';          name = 'Notepad';                            icon = "$env:SystemRoot\System32\notepad.exe";  comment = 'Text editor' }
        @{ id = 'explorer';         name = 'File Explorer';                      icon = "$env:SystemRoot\explorer.exe";          comment = 'Browse files in this qube' }
        @{ id = 'settings';         name = 'Settings';                           icon = "$env:SystemRoot\ImmersiveControlPanel\SystemSettings.exe"; comment = 'Windows settings' }
        @{ id = 'cmd';              name = 'Command Prompt';                     icon = "$env:SystemRoot\System32\cmd.exe";      comment = 'Command Prompt' }
        @{ id = 'cmd-admin';        name = 'Command Prompt (Administrator)';     icon = "$env:SystemRoot\System32\cmd.exe";      comment = 'Command Prompt, elevated - Windows will ask for confirmation' }
        @{ id = 'powershell';       name = 'Windows PowerShell';                 icon = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"; comment = 'Windows PowerShell' }
        @{ id = 'powershell-admin'; name = 'Windows PowerShell (Administrator)'; icon = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"; comment = 'Windows PowerShell, elevated - Windows will ask for confirmation' }
    )
    if ($edge) {
        $builtins += @{ id = 'edge'; name = 'Microsoft Edge'; icon = $edge; comment = 'Web browser' }
    }

    foreach ($b in $builtins) {
        if ($script:EmittedIds.ContainsKey($b.id)) {
            LogDebug "built-in '$($b.id)' already provided by a Start Menu shortcut - skipping"
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
    }
} catch {
    LogWarning "built-in entries failed: $($_.Exception.Message)"
}

# ALWAYS 0. See the header: a non-zero exit here is what dom0 turns into
# "qvm-sync-appmenus ... returned non-zero exit status 1", losing the whole application list.
exit 0

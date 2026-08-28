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

. $env:QUBES_TOOLS\qubes-rpc-services\log.ps1

# Registry location for path hash -> full path mapping for GetImageRGBA
$RegistryMapPath = 'HKCU:\Software\Invisible Things Lab\Qubes Tools'
$RegistryMapKey = 'AppMap'

New-Item -Path $RegistryMapPath -Name $RegistryMapKey -Force | Out-Null

# Instantiate the wscript.shell COM object
$WshShell = new-object -comobject "WScript.Shell"

$Sha1 = [System.Security.Cryptography.SHA1]::Create()
Function GetHash($string)
{
    $hash = [BitConverter]::ToString($Sha1.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($string))).Replace("-", "")
    return $hash.ToLowerInvariant()
}

# Extract data from one shortcut file
$script:EmittedIds = @{}

Function ProcessLink($pathObj, $basepath)
{
    $desktopFileName = ($pathObj.FullName).SubString($basepath.Length+1).Replace(' ', '_').Replace('\','-')
    $linkBaseName = $desktopFileName.Replace('.lnk', '.desktop') # fixme: check if it's at the end of the string
    if ($pathObj.DirectoryName -ne $basepath) {
        $appmenuLocation = $pathObj.DirectoryName.SubString($basepath.Length+1).Replace('\','-') + " "
    } else {
        $appmenuLocation = ""
    }

    $linkObj = $WshShell.CreateShortcut($pathObj.FullName)
    $targetPath = "cmd.exe /c `"$($pathObj.FullName)`""
    # We send .LNK file hash as icon name since the name can't contain some characters that can be in a file path
    # and the GetImageRGBA Qubes service needs to retrieve bitmap from this name alone.
    $targetHash = GetHash($pathObj.FullName)

    # Store the hash-LNK mapping in the registry for easy  retrieval by GetImageRGBA.
    New-ItemProperty -Path "$RegistryMapPath\$RegistryMapKey" -Name $targetHash -PropertyType String -Value $pathObj.FullName | Out-Null
    # Store also basename-LNK location for qubes.StartApp service
    New-ItemProperty -Path "$RegistryMapPath\$RegistryMapKey" -Name $desktopFileName.Replace('.lnk', '') -PropertyType String -Value $pathObj.FullName | Out-Null

    LogDebug "$targetHash -> $targetPath"

    Write-Host "$($linkBaseName):Name=$appmenuLocation$($pathObj.BaseName)"
    Write-Host "$($linkBaseName):Exec=$($targetPath.Replace('\','\\'))"
    Write-Host "$($linkBaseName):Comment=$($linkObj.Description)"
    Write-Host "$($linkBaseName):Icon=$targetHash"
    $script:EmittedIds[$linkBaseName.Replace('.desktop','').ToLowerInvariant()] = $true
}

# todo: check if there are duplicated entries

# "All users" menu
$p = $WshShell.SpecialFolders.item("AllUsersPrograms")
$shortcuts = get-childitem -path $p -filter "*.lnk" -rec
$shortcuts | foreach-object {ProcessLink $_  $p }

# Current user menu
$p = $WshShell.SpecialFolders.item("StartMenu")
$shortcuts = get-childitem -path $p -filter "*.lnk" -rec
$shortcuts | foreach-object {ProcessLink $_  $p }

# Pinned Start Menu items
# FIXME: this doesn't work in win10 and there seems to be no official way of getting pinned tiles
#$p = Join-Path $WshShell.SpecialFolders.item("AppData") "Microsoft\Internet Explorer\Quick Launch\User Pinned\StartMenu"
#$shortcuts = get-childitem -path $p -filter "*.lnk" -rec
#$shortcuts | foreach-object {ProcessLink $_  $p }


# ---------------------------------------------------------------- built-in entries
#
# A freshly installed Windows guest has almost nothing in its Start Menu that survives the sweep
# above, so dom0's application list comes up nearly empty and the qube looks unusable: in seamless
# mode there is no taskbar and no desktop either, so the app menu is the ONLY way in. Report a
# small fixed set that every Windows has, so a new qube is usable the moment it is synced.
#
# The two "(Administrator)" entries launch through the ordinary Windows elevation prompt - see
# start-app.ps1. They deliberately do NOT use any silent-elevation trick: a menu entry that
# quietly hands out admin would be a hole, and since 4.3.11 the consent dialog is an ordinary
# window in dom0, so answering it is normal.
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
    New-ItemProperty -Path "$RegistryMapPath\$RegistryMapKey" -Name $hash -PropertyType String -Value $iconSource -Force | Out-Null
    # start-app.ps1 has its own table for these, but keep the mapping so the lookup path that
    # every other entry uses works for them too.
    New-ItemProperty -Path "$RegistryMapPath\$RegistryMapKey" -Name $b.id -PropertyType String -Value $iconSource -Force | Out-Null

    Write-Host "$($b.id).desktop:Name=$($b.name)"
    Write-Host "$($b.id).desktop:Exec=qubes-rpc-multiplexer qubes.StartApp+$($b.id)"
    Write-Host "$($b.id).desktop:Comment=$($b.comment)"
    Write-Host "$($b.id).desktop:Icon=$hash"
}

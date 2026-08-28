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

# Registry location for path hash -> full path mapping
$RegistryMapPath = 'HKCU:\Software\Invisible Things Lab\Qubes Tools'
$RegistryMapKey = 'AppMap'

$desktopBaseName = $args[0]

# BUILT-IN ENTRIES (see get-appmenus.ps1). A fresh Windows guest has almost nothing in its Start
# Menu, and in seamless mode there is no taskbar and no desktop either, so without these the qube
# has no way in at all. Resolved here rather than through the AppMap registry lookup below,
# because these are not shortcuts and because the map lives in HKCU - which is the wrong hive when
# this service happens to run as anything but the logged-on user.
$edge = @(
    "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe",
    "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe"
) | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1

$builtin = @{
    'notepad'          = @{ File = "$env:SystemRoot\System32\notepad.exe"; Elevated = $false }
    'explorer'         = @{ File = "$env:SystemRoot\explorer.exe";          Elevated = $false }
    'settings'         = @{ File = 'ms-settings:';                          Elevated = $false }
    'cmd'              = @{ File = "$env:SystemRoot\System32\cmd.exe";      Elevated = $false }
    'cmd-admin'        = @{ File = "$env:SystemRoot\System32\cmd.exe";      Elevated = $true  }
    'powershell'       = @{ File = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"; Elevated = $false }
    'powershell-admin' = @{ File = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"; Elevated = $true  }
}
if ($edge) { $builtin['edge'] = @{ File = $edge; Elevated = $false } }

# An app can only be started into a session that exists.
while (! (Get-CimInstance -ClassName Win32_ComputerSystem).UserName) {
    LogDebug "Waiting for user logon"
    Start-Sleep -Milliseconds 100
}

if ($builtin.ContainsKey($desktopBaseName)) {
    $b = $builtin[$desktopBaseName]
    if ($b.Elevated) {
        # -Verb RunAs, i.e. the ordinary Windows elevation prompt, deliberately: a menu entry that
        # silently handed out admin would be a hole, and since 4.3.11 the consent dialog is an
        # ordinary window in dom0 (PromptOnSecureDesktop=0), so it can actually be answered.
        LogDebug "Starting $($b.File) ELEVATED"
        Start-Process -FilePath $b.File -Verb RunAs -Wait
    } else {
        LogDebug "Starting $($b.File)"
        Start-Process -FilePath $b.File -Wait
    }
    return
}

$desktopFullName = Get-ItemProperty -Path "$RegistryMapPath\$RegistryMapKey" -Name $desktopBaseName
$target = $desktopFullName.$desktopBaseName

LogDebug "Starting $target"

Start-Process -Wait $target

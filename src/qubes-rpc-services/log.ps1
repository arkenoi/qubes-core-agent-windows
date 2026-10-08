# The documented default, used only when the registry cannot be read. It is the ONE common
# location's default, not a second place to look: "<systemdrive>\Qubes Logs".
function QwtDefaultLogDir {
    if ($env:SystemDrive) { return (Join-Path $env:SystemDrive 'Qubes Logs') }
    # Non-Windows host: only the offline suites get here.
    return ([IO.Path]::GetTempPath())
}

function LogStart {
    # -ErrorAction Stop AND a catch. Without it the read is NON-TERMINATING under the default
    # $ErrorActionPreference='Continue', which is what the guest runs these under: $logDir became
    # $null, "$logDir\$logName" became the RELATIVE "\VMExec.ps1-20261008.log", and the line was
    # written to the ROOT OF THE CURRENT DRIVE - a new stray log outside the one common location,
    # which nothing collects. Measured 2026-10-08 (tools/tests/ps-log-sink-test.ps1).
    $logDir = $null
    try { $logDir = Get-ItemPropertyValue "hklm:\Software\Invisible Things Lab\Qubes Tools" LogDir -ErrorAction Stop } catch { }
    $fellBack = $false
    if (-not $logDir) {
        $logDir = QwtDefaultLogDir
        $fellBack = $true
    }
    # get the originating script path
    $basePath = (Get-PSCallStack | Where-Object { $_.ScriptName } | Select-Object -Last 1).ScriptName
    $baseName = Split-Path -Leaf -Path $basePath
    # ONE FILE PER SCRIPT PER DAY, not one per process. A per-pid name turned a few hundred qrexec
    # calls into a few hundred files, which starved the log sweep's file budget and made it skip the
    # death record the release gate exists to read. The DATE stays in the name because the retention
    # sweep deletes by age: a name with no date would keep its first creation time for ever.
    $logname = "$baseName-$(Get-Date -Format "yyyyMMdd").log"
    $global:qwtLogPath = Join-Path $logDir $logName
    # AN UNREADABLE LogLevel MUST NOT SILENCE ANYTHING, LEAST OF ALL AN ERROR. This read had the
    # same missing -ErrorAction, so a failure left $qwtLogLevel unset - and `1 -le $null` is False
    # on PowerShell, measured. Every level, LogError included, returned without writing a line and
    # without a word on stderr: a guest whose LogLevel value was gone went completely dark and the
    # sweep read it as clean. The default is 3 (INFO), which is what the MSI ships.
    $lvl = $null
    try { $lvl = Get-ItemPropertyValue "hklm:\Software\Invisible Things Lab\Qubes Tools" LogLevel -ErrorAction Stop } catch { }
    $lvlBad = $false
    if ($null -eq $lvl -or -not ([int]::TryParse([string]$lvl, [ref]$null)) -or [int]$lvl -lt 1 -or [int]$lvl -gt 5) {
        $lvl = 3
        $lvlBad = $true
    }
    $global:qwtLogLevel = [int]$lvl
    # SAY IT, once, in the log itself as well as on stderr: a qrexec service's stderr goes back to
    # dom0, but a task-hosted one's is discarded, and a sink that moves or re-levels itself in
    # silence is the same defect as any other quiet fallback here.
    if ($fellBack) {
        $m = "qwt log: LogDir is unreadable - this log is at $global:qwtLogPath, NOT in the configured location"
        try { [Console]::Error.WriteLine($m) } catch { }
        LogAppendLine "[$(Get-Date -Format 'yyyyMMdd.HHmmss.fff')-$PID-W] $m"
    }
    if ($lvlBad) {
        $m = "qwt log: LogLevel is unreadable or out of range - defaulting to 3 (INFO)"
        try { [Console]::Error.WriteLine($m) } catch { }
        LogAppendLine "[$(Get-Date -Format 'yyyyMMdd.HHmmss.fff')-$PID-W] $m"
    }
}

function Log {
    param (
        [ValidateRange(1,5)][int]$level,
        [string]$msg
    )

    if ($qwtLogPath -eq $null) {
        LogStart
    }

    if ($level -le $qwtLogLevel) {
        $ts = Get-Date -Format "yyyyMMdd.HHmmss.fff"
        # THE PID IS PART OF THE PREFIX, not of the filename. tools/log-sweep.py's WINUTILS_RE
        # requires a pid between the timestamp and the level; without it every one of these logs
        # fell to the 'plain' family, where parse_plain records each line with ts=None - so the
        # lines could not be joined to a boot and no --since window could place them. An ERROR the
        # gate cannot timestamp is an error it cannot attribute. (The FILENAME keeps no pid: one
        # file per script per day. A per-pid name turned a few hundred qrexec calls into a few
        # hundred files and starved the sweep's file budget.)
        LogAppendLine "[$ts-$PID-$("EWIDV"[$level-1])] $msg"
    }
}

function LogAppendLine {
    # Concurrent invocations of one script now share one file, so an append can lose the file to
    # another writer (ERROR_SHARING_VIOLATION, surfaced as IOException). PowerShell has no
    # atomic-append primitive, so the collision is RETRIED - briefly and a bounded number of times -
    # and a line that still cannot be written SAYS SO on stderr. A dropped log line is never silent.
    param([string]$line)

    for ($attempt = 1; $attempt -le 20; $attempt++) {
        try {
            Add-Content -LiteralPath $qwtLogPath -Value $line -ErrorAction Stop
            return
        } catch [System.IO.IOException] {
            Start-Sleep -Milliseconds 15
        } catch {
            # Not a collision - the path is wrong, the disk is full, the directory is gone. Retrying
            # that would only hide it.
            [Console]::Error.WriteLine("qwt log: line LOST ($($_.Exception.Message)): $line")
            return
        }
    }
    [Console]::Error.WriteLine("qwt log: line LOST after 20 contended attempts: $line")
}

function LogError {
    param([string]$msg)
    Log 1 $msg
}

function LogWarning {
    param([string]$msg)
    Log 2 $msg
}

function LogInfo {
    param([string]$msg)
    Log 3 $msg
}

function LogDebug {
    param([string]$msg)
    Log 4 $msg
}

function LogVerbose {
    param([string]$msg)
    Log 5 $msg
}

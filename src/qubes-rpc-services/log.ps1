function LogStart {
    $logDir = Get-ItemPropertyValue "hklm:\Software\Invisible Things Lab\Qubes Tools" LogDir
    # get the originating script path
    $basePath = (Get-PSCallStack | Where-Object { $_.ScriptName } | Select-Object -Last 1).ScriptName
    $baseName = Split-Path -Leaf -Path $basePath
    # ONE FILE PER SCRIPT PER DAY, not one per process. A per-pid name turned a few hundred qrexec
    # calls into a few hundred files, which starved the log sweep's file budget and made it skip the
    # death record the release gate exists to read. The DATE stays in the name because the retention
    # sweep deletes by age: a name with no date would keep its first creation time for ever.
    $logname = "$baseName-$(Get-Date -Format "yyyyMMdd").log"
    $global:qwtLogPath = "$logDir\$logName"
    $global:qwtLogLevel = Get-ItemPropertyValue "hklm:\Software\Invisible Things Lab\Qubes Tools" LogLevel
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
        LogAppendLine "[$ts-$("EWIDV"[$level-1])] $msg"
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

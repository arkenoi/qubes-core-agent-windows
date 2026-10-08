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

# qubes.SetDateTime: dom0 hands us the time on stdin and we set the guest's clock to it.
#
# WHAT WAS WRONG, measured 2026-10-08 on win11r-logvol: its clock was THREE HOURS ahead of the
# host's with its timezone set to UTC, so it believed that was UTC. Every --since log window on that
# guest silently admitted three hours of older lines, which made a clean boot read as two errors.
# This script was two lines - `$in = [Console]::In.ReadLine(); Set-Date $in` - with:
#   * NO LOGGING, so a failure left nothing behind anywhere and could not be diagnosed at all;
#   * NO INVARIANT PARSE. `Set-Date` on a string parses with the CURRENT CULTURE, and this product
#     ships German goldens (win11de-*). An ISO-8601 string is not guaranteed to parse under every
#     culture, and when it throws, see the previous point;
#   * NO VERIFICATION, so "it ran" and "the clock is right" were indistinguishable;
#   * NO UTC HANDLING. The input carries an offset (+0000) and the guest's own zone may be anything;
#     setting a DateTime whose Kind is Unspecified sets LOCAL time, which on a guest whose zone is
#     wrong puts the clock out by exactly that zone - the shape of the skew measured above.
#
# It now parses invariantly, sets the clock in UTC terms, verifies the result and logs all of it
# through the product's own logger, so a failure is visible in the one place the sweep reads.
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\log.ps1"

$raw = $null
try { $raw = [Console]::In.ReadLine() } catch { }
if ([string]::IsNullOrWhiteSpace($raw)) {
    LogError "SETTIME no time on stdin (got $(if ($null -eq $raw) { 'nothing' } else { "'" + $raw + "'" })) - the clock is UNCHANGED"
    exit 1
}
$raw = $raw.Trim()

# INVARIANT, and every shape dom0 has been seen to send. The documented form carries an offset
# (2014-09-29T22:59:21+0000); older dom0s send it without one, which is UTC by convention here.
$inv = [Globalization.CultureInfo]::InvariantCulture
$styles = [Globalization.DateTimeStyles]::AdjustToUniversal -bor [Globalization.DateTimeStyles]::AssumeUniversal
$formats = @(
    "yyyy-MM-ddTHH:mm:ssK", "yyyy-MM-ddTHH:mm:sszzz", "yyyy-MM-ddTHH:mm:sszz",
    "yyyy-MM-ddTHH:mm:ss.fffK", "yyyy-MM-ddTHH:mm:ss", "yyyy-MM-dd HH:mm:ss",
    "ddd MMM d HH:mm:ss yyyy"
)
[datetime]$utc = [datetime]::MinValue
if (-not [datetime]::TryParseExact($raw, $formats, $inv, $styles, [ref]$utc)) {
    if (-not [datetime]::TryParse($raw, $inv, $styles, [ref]$utc)) {
        LogError "SETTIME could not parse '$raw' as a time under the invariant culture - the clock is UNCHANGED"
        exit 1
    }
}
$utc = [datetime]::SpecifyKind($utc, [DateTimeKind]::Utc)

$before = (Get-Date).ToUniversalTime()
$skew = [math]::Round(($utc - $before).TotalSeconds, 1)

try {
    # CORRECTED 2026-10-08, MEASURED ON A FIELD-FAITHFUL GUEST. This line used to pass $utc with the
    # comment "Set-Date takes a DateTime and honours its Kind, so a Utc value sets the clock in UTC
    # terms rather than writing a UTC instant into the guest's local frame." THAT IS FALSE.
    # Set-Date sets the LOCAL clock from the value's components (it ends in Win32 SetLocalTime); the
    # DateTimeKind is not consulted. So handing it a Utc value of 15:05:09 on a UTC+2 guest set LOCAL
    # time to 15:05:09, making the guest read 13:05:09 UTC - off by exactly the zone offset, and the
    # correction made the skew WORSE each pass.
    #
    # WHY IT WAS NEVER SEEN HERE: every rig guest runs UTC, where the offset is zero and the bug is
    # invisible. It was found on a clone built to match a German reporter's template (Europe/Berlin,
    # +2 in October): a boot-scoped measurement of his environment carried exactly one ERROR, and it
    # was this - "SETTIME the clock did not take: asked for ...T15:05:09Z, it reads ...T13:05:09Z -
    # still -7200 s out (was -3604.3 s out before)".
    #
    # ToLocalTime() converts the instant into this guest's frame, which is what Set-Date wants. The
    # verification below still compares in UTC, so a zone this gets wrong cannot pass silently.
    Set-Date -Date $utc.ToLocalTime() -ErrorAction Stop | Out-Null
} catch {
    LogError "SETTIME Set-Date to $($utc.ToString('o')) failed: $($_.Exception.Message) - the clock is UNCHANGED (was off by $skew s)"
    exit 1
}

# VERIFY. "It ran" and "the clock is right" were indistinguishable before this.
$after = (Get-Date).ToUniversalTime()
$residual = [math]::Round(($after - $utc).TotalSeconds, 1)
if ([math]::Abs($residual) -gt 5) {
    LogError ("SETTIME the clock did not take: asked for $($utc.ToString('o')), it reads " +
              "$($after.ToString('o')) - still $residual s out (was $skew s out before)")
    exit 1
}
LogInfo ("SETTIME clock set to $($utc.ToString('o')) (was $skew s out, now $residual s; " +
         "guest zone $((Get-TimeZone).Id), offset $((Get-TimeZone).BaseUtcOffset))")
exit 0

@echo off

pushd "%QUBES_TOOLS%"\bin

rem FOUR FIELDS. qrexec-client-vm parses its raw command line with GetArgument(), which splits on
rem '|' and returns NULL once no separator is left, so it needs domain|service|LOCAL USER|LOCAL
rem PROGRAM. This line passed THREE and would end in "wmain: Usage: ..." - measured 2026-10-08 when
rem guest\sync-clock-from-dom0.ps1 made the same mistake and produced 34 of those in one cycle.
rem Consistent with this file being called by nothing but qubes.SuspendPostAll: it cannot ever have
rem worked. The service's output is connected by qrexec to the local program, so set-time.ps1 reads
rem the date on its stdin.
qrexec-client-vm.exe @default^|qubes.GetDate^|SYSTEM^|powershell.exe -executionpolicy bypass -noninteractive -inputformat none -file "%QUBES_TOOLS%\qubes-rpc-services\set-time.ps1"

popd

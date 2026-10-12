# Verify the packaging contract independently of how CMake/linker flags were
# specified: native x64, a GUI subsystem for double-click, and a console CLI.
[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$GuiExe,
      [Parameter(Mandatory = $true)][string]$ConsoleExe)
$ErrorActionPreference = 'Stop'
foreach ($entry in @(@{ Path = $GuiExe; Subsystem = 2 }, @{ Path = $ConsoleExe; Subsystem = 3 })) {
    $bytes = [IO.File]::ReadAllBytes($entry.Path)
    if ($bytes.Length -lt 256 -or $bytes[0] -ne 0x4d -or $bytes[1] -ne 0x5a) { throw 'Invalid executable.' }
    $peOffset = [BitConverter]::ToInt32($bytes, 0x3c)
    if ($peOffset -lt 0 -or $peOffset + 96 -gt $bytes.Length -or
        [BitConverter]::ToUInt32($bytes, $peOffset) -ne 0x4550 -or
        [BitConverter]::ToUInt16($bytes, $peOffset + 4) -ne 0x8664 -or
        [BitConverter]::ToUInt16($bytes, $peOffset + 24) -ne 0x20b) {
        throw "Expected a native x64 PE executable: $($entry.Path)"
    }
    $subsystem = [BitConverter]::ToUInt16($bytes, $peOffset + 24 + 68)
    if ($subsystem -ne $entry.Subsystem) {
        throw "Wrong executable subsystem $subsystem (expected $($entry.Subsystem)): $($entry.Path)"
    }
    $text = [Text.Encoding]::UTF8.GetString($bytes)
    if (-not $text.Contains('PerMonitorV2') -or -not $text.Contains('>UTF-8<') -or
        -not $text.Contains('asInvoker')) { throw 'Required Windows manifest settings are absent.' }
    Write-Output "PASS: $($entry.Path) is x64 with subsystem $subsystem and the Windows manifest."
}

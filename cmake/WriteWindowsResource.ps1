# Write a Win32 .res containing RT_MANIFEST, ID 1. This uses the documented
# binary resource header, avoiding a separate Windows SDK resource compiler
# for the portable toolchain. The normal MSVC build uses its own manifest tool.
[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$Manifest,
      [Parameter(Mandatory = $true)][string]$Output)
$ErrorActionPreference = 'Stop'
$manifestBytes = [IO.File]::ReadAllBytes($Manifest)
$stream = [IO.File]::Open($Output, [IO.FileMode]::Create)
$writer = New-Object IO.BinaryWriter($stream)
try {
    # Every .res starts with a null resource header.
    foreach ($value in @([uint32]0, [uint32]32, [uint32]65535, [uint32]65535,
                        [uint32]0, [uint32]0, [uint32]0, [uint32]0)) { $writer.Write([uint32]$value) }
    $writer.Write([uint32]$manifestBytes.Length)
    $writer.Write([uint32]32)
    $writer.Write([uint16]65535); $writer.Write([uint16]24) # RT_MANIFEST
    $writer.Write([uint16]65535); $writer.Write([uint16]1)  # CREATEPROCESS_MANIFEST_RESOURCE_ID
    $writer.Write([uint32]0) # DataVersion
    $writer.Write([uint16]0x1030) # MOVEABLE | PURE | DISCARDABLE
    $writer.Write([uint16]0) # neutral language
    $writer.Write([uint32]0); $writer.Write([uint32]0)
    $writer.Write([byte[]]$manifestBytes)
    while ($stream.Position % 4) { $writer.Write([byte]0) }
} finally {
    $writer.Dispose()
}

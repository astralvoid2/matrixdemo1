<#
.SYNOPSIS
Builds native x64 Matrix Rain with MSVC, fetching local build tools as needed.
.EXAMPLE
.\Build-Windows.ps1 -Test
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')]
    [string]$Configuration = 'Release',
    [string]$CMakePath,
    [string]$GlfwSourceDir,
    [switch]$PortableCompiler,
    [switch]$Test,
    [switch]$Run
)

$ErrorActionPreference = 'Stop'
$repoRoot = $PSScriptRoot
$compilerName = if ($PortableCompiler) { 'portable' } else { 'msvc' }
$buildDir = Join-Path $repoRoot "build\windows-$compilerName-$Configuration"
$toolsDir = Join-Path $repoRoot '.tools'

function Get-PortableTool {
    param([string]$Url, [string]$ArchiveName, [string]$Hash, [string]$Destination)
    New-Item -ItemType Directory -Path $toolsDir -Force | Out-Null
    $archivePath = Join-Path $toolsDir $ArchiveName
    Write-Host "Downloading $ArchiveName into .tools (no system-wide installation)."
    Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $archivePath
    if ((Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash -ne $Hash) {
        throw "$ArchiveName failed SHA256 verification."
    }
    Expand-Archive -LiteralPath $archivePath -DestinationPath $Destination -Force
}

# VS component metadata can be stale after an update. Check the tool files,
# rather than relying only on vswhere's -requires component filtering.
if (-not $PortableCompiler -and ($env:VSCMD_ARG_TGT_ARCH -ne 'x64' -or -not (Get-Command cl.exe -ErrorAction SilentlyContinue))) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vsRoots = @()
    if (Test-Path -LiteralPath $vswhere) {
        $vsRoots = @(& $vswhere -all -products '*' -property installationPath)
    }
    $vcvars = $null
    foreach ($vsRoot in $vsRoots) {
        $candidate = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
        if ((Test-Path -LiteralPath $candidate) -and
            (Test-Path -LiteralPath (Join-Path (Split-Path -Parent $candidate) 'vcvarsall.bat'))) {
            $vcvars = $candidate; break
        }
    }
    if (-not $vcvars) {
        throw "A complete MSVC x64 installation was not found. In Visual Studio Installer, add or repair 'Desktop development with C++' (MSVC v143 and a Windows SDK), or run this script with -PortableCompiler."
    }
    Write-Host "Loading the x64 compiler environment from $vcvars"
    $environmentLines = & $env:ComSpec /d /c ('call "{0}" >nul && set' -f $vcvars)
    if ($LASTEXITCODE -ne 0) { throw 'Visual Studio could not initialize its C++ build environment.' }
    foreach ($line in $environmentLines) {
        if ($line -match '^([^=]+)=(.*)$') {
            [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
        }
    }
}
if (-not $PortableCompiler -and (-not (Get-Command cl.exe -ErrorAction SilentlyContinue) -or
    -not (Get-Command nmake.exe -ErrorAction SilentlyContinue))) {
    throw 'MSVC/NMake are missing. Add the Desktop development with C++ workload in Visual Studio Installer.'
}

if (-not $CMakePath) {
    $cmakeCommand = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($cmakeCommand) { $CMakePath = $cmakeCommand.Source }
    elseif ($env:VSINSTALLDIR) {
        $candidate = Join-Path $env:VSINSTALLDIR 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
        if (Test-Path -LiteralPath $candidate) { $CMakePath = $candidate }
    }
}
if (-not $CMakePath) {
    $CMakePath = Join-Path $toolsDir 'cmake-3.31.8-windows-x86_64\bin\cmake.exe'
    if (-not (Test-Path -LiteralPath $CMakePath)) {
        Get-PortableTool -Url 'https://github.com/Kitware/CMake/releases/download/v3.31.8/cmake-3.31.8-windows-x86_64.zip' -ArchiveName 'cmake-3.31.8.zip' -Hash '81AA9964DBABD71FE02E7EC50472FD3AD56138C49944515ECE9001EFBFF8D719' -Destination $toolsDir
    }
}
$CMakePath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($CMakePath)
if (-not (Test-Path -LiteralPath $CMakePath)) { throw "CMake executable not found: $CMakePath" }
$configureArgs = @('-S', $repoRoot, '-B', $buildDir, "-DCMAKE_BUILD_TYPE=$Configuration")
if ($PortableCompiler) {
    $zigPath = Join-Path $toolsDir 'zig-x86_64-windows-0.14.1\zig.exe'
    $ninjaPath = Join-Path $toolsDir 'ninja\ninja.exe'
    if (-not (Test-Path -LiteralPath $zigPath)) {
        Get-PortableTool -Url 'https://ziglang.org/download/0.14.1/zig-x86_64-windows-0.14.1.zip' -ArchiveName 'zig-0.14.1.zip' -Hash '554F5378228923FFD558EAC35E21AF020C73789D87AFEABF4BFD16F2E6FEED2C' -Destination $toolsDir
    }
    if (-not (Test-Path -LiteralPath $ninjaPath)) {
        Get-PortableTool -Url 'https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-win.zip' -ArchiveName 'ninja-1.12.1.zip' -Hash 'F550FEC705B6D6FF58F2DB3C374C2277A37691678D6ABA463ADCBB129108467A' -Destination (Join-Path $toolsDir 'ninja')
    }
    $env:ZIG_GLOBAL_CACHE_DIR = Join-Path $toolsDir 'zig-cache'
    $env:ZIG_LOCAL_CACHE_DIR = Join-Path $toolsDir 'zig-local-cache'
    $configureArgs += @('-G', 'Ninja', "-DCMAKE_TOOLCHAIN_FILE=$(Join-Path $repoRoot 'cmake\ZigWindows.cmake')",
        "-DCMAKE_MAKE_PROGRAM=$ninjaPath", "-DMATRIXRAIN_ZIG_EXECUTABLE=$zigPath")
} else {
    $configureArgs += @('-G', 'NMake Makefiles')
}
if ($GlfwSourceDir) {
    $glfwPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($GlfwSourceDir)
    if (-not (Test-Path -LiteralPath (Join-Path $glfwPath 'CMakeLists.txt'))) {
        throw "GLFW source directory is invalid: $glfwPath"
    }
    $configureArgs += "-DFETCHCONTENT_SOURCE_DIR_GLFW=$glfwPath"
} else {
    # PowerShell uses the Windows certificate store for the download. This
    # also permits repeat builds without network access once cached locally.
    $glfwPath = Join-Path $toolsDir 'glfw-3.4'
    if (-not (Test-Path -LiteralPath (Join-Path $glfwPath 'deps\glad\gl.h'))) {
        Get-PortableTool -Url 'https://github.com/glfw/glfw/archive/refs/tags/3.4.zip' -ArchiveName 'glfw-3.4.zip' -Hash 'A133DDC3D3C66143EBA9035621DB8E0BCF34DBA1EE9514A9E23E96AFD39FD57A' -Destination $toolsDir
    }
    $configureArgs += "-DFETCHCONTENT_SOURCE_DIR_GLFW=$glfwPath"
}
Write-Host 'Configuring Matrix Rain with GLFW 3.4 source.'
& $CMakePath @configureArgs
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
$buildArgs = @('--build', $buildDir)
if ($PortableCompiler) { $buildArgs += @('--parallel', '2') }
& $CMakePath @buildArgs
if ($LASTEXITCODE -ne 0) { throw 'Windows compilation failed.' }
if ($Test) {
    $ctestPath = Join-Path (Split-Path -Parent $CMakePath) 'ctest.exe'
    & $ctestPath --test-dir $buildDir --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Build verification failed.' }
}
Write-Host "Built $(Join-Path $repoRoot 'bin\matrixrain.exe')"
Write-Host "Console tools: $(Join-Path $repoRoot 'bin\matrixrain-console.exe')"
if ($Run) {
    Start-Process -FilePath (Join-Path $repoRoot 'bin\matrixrain.exe') -WorkingDirectory $repoRoot -WindowStyle Hidden
}

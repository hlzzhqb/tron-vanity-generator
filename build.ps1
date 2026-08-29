# Build with an installed Visual Studio (or VS Build Tools) + its bundled CMake/Ninja.
# Plain PowerShell, no "Developer Command Prompt" needed:
#   powershell -ExecutionPolicy Bypass -File build.ps1
#
# Requires: Visual Studio 2022/2026 or Build Tools with the C++ workload, git, GitHub access.
# The resulting exe statically links the runtime and can be copied to another x64 Windows PC.
param(
    [string]$Config = "Release"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

if (-not (Test-Path "$root\third_party\secp256k1\CMakeLists.txt")) {
    Write-Host "Cloning libsecp256k1 ..."
    git clone --depth 1 https://github.com/bitcoin-core/secp256k1.git "$root\third_party\secp256k1"
}

# --- locate Visual Studio ---
$installerDir = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
$vswhere = Join-Path $installerDir "vswhere.exe"
$vsPath = $null
if (Test-Path $vswhere) {
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (-not $vsPath) {
    foreach ($p in @("D:\devtools\VS2026BuildTools",
                     "${env:ProgramFiles}\Microsoft Visual Studio\2022\BuildTools",
                     "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community",
                     "${env:ProgramFiles}\Microsoft Visual Studio\2022\Professional")) {
        if (Test-Path "$p\VC\Auxiliary\Build\vcvars64.bat") { $vsPath = $p; break }
    }
}
if (-not $vsPath) { throw "Visual Studio / Build Tools not found (need the C++ workload)" }
Write-Host "VS: $vsPath"

# vcvars calls vswhere internally; put the Installer dir on PATH so a fresh env resolves it
$env:PATH = "$installerDir;$env:PATH"

$cmake = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if (-not (Test-Path $cmake)) { $cmake = "cmake" }
$ninja = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match "^([^=]+)=(.*)$") { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
}
if (-not $env:INCLUDE) { throw "MSVC environment not loaded (INCLUDE is empty)" }

if (Test-Path $ninja) {
    $gen = @("-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=$ninja")
} else {
    $gen = @("-G", "NMake Makefiles")
}

& $cmake -B build $gen "-DCMAKE_BUILD_TYPE=$Config"
if ($LASTEXITCODE) { throw "cmake configure failed" }
& $cmake --build build --config $Config
if ($LASTEXITCODE) { throw "build failed" }

Write-Host ""
Write-Host "Done: $root\build\tron_vanity_generator.exe"
& "$root\build\tron_vanity_generator.exe" --selftest
& "$root\build\tron_vanity_generator.exe" --list

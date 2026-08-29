# 用已安装的 VS2026 生成工具 + 自带 CMake 构建。
# 用法（普通 PowerShell 即可，无需先开 “开发者命令提示符”）：
#   powershell -ExecutionPolicy Bypass -File build.ps1
param(
    [string]$Config = "Release"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

if (-not (Test-Path "$root\third_party\secp256k1\CMakeLists.txt")) {
    Write-Host "克隆 libsecp256k1 ..."
    git clone --depth 1 https://github.com/bitcoin-core/secp256k1.git "$root\third_party\secp256k1"
}

# 定位 VS
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { $vsPath = "D:\devtools\VS2026BuildTools" }

$cmake = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if (-not (Test-Path $cmake)) { $cmake = "cmake" }
$ninja = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

# 导入 MSVC 环境变量
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
Write-Host "载入 MSVC 环境: $vcvars"
cmd /c "`"$vcvars`" && set" | ForEach-Object {
    if ($_ -match "^([^=]+)=(.*)$") { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
}

$gen = if (Test-Path $ninja) { @("-G","Ninja","-DCMAKE_MAKE_PROGRAM=$ninja") } else { @("-G","NMake Makefiles") }

& $cmake -B build $gen "-DCMAKE_BUILD_TYPE=$Config"
& $cmake --build build --config $Config
Write-Host ""
Write-Host "完成: $root\build\tron_vanity_generator.exe"
& "$root\build\tron_vanity_generator.exe" --list

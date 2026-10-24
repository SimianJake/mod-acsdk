# Shared paths for the build scripts. Dot-source this: . "$PSScriptRoot\env.ps1"
# Override any value by setting the environment variable before running a script.

$RepoRoot = Split-Path -Parent $PSScriptRoot

function Get-Setting($name, $default) {
    $value = [Environment]::GetEnvironmentVariable($name)
    if ([string]::IsNullOrEmpty($value)) { return $default } else { return $value }
}

# AzerothCore source checkout (master). Kept next to this repo.
$AcSource     = Get-Setting "ACSDK_AC_SOURCE"   (Join-Path (Split-Path -Parent $RepoRoot) "azerothcore-wotlk")
# Build/install/vcpkg/MySQL data live on a short path without spaces.
$BuildRoot    = Get-Setting "ACSDK_BUILD_ROOT"  "C:\acsdk-build"
$AcBuild      = Join-Path $BuildRoot "ac"
$AcInstall    = Join-Path $BuildRoot "server"
$VcpkgRoot    = Get-Setting "VCPKG_ROOT"        "C:\vcpkg"
$VcpkgTriplet = "x64-windows-static-md"
$VcpkgInstalled = Join-Path $BuildRoot "vcpkg_installed"
$MySqlRoot    = Get-Setting "ACSDK_MYSQL_ROOT"  "C:\Program Files\MySQL\MySQL Server 8.4"
$MySqlData    = Join-Path $BuildRoot "mysql"

# Tools installed by winget may not be on PATH in an already-open shell.
$env:Path = [Environment]::GetEnvironmentVariable("Path", "Machine") + ";" + [Environment]::GetEnvironmentVariable("Path", "User")

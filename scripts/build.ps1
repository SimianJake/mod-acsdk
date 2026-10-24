# Configures and builds AzerothCore + mod-acsdk (Visual Studio, x64) and installs to $AcInstall.
#   .\scripts\build.ps1                  # configure (if needed) + build RelWithDebInfo + install
#   .\scripts\build.ps1 -Config Debug
#   .\scripts\build.ps1 -Reconfigure     # force a fresh CMake configure
param(
    [ValidateSet("RelWithDebInfo", "Debug", "Release")] [string] $Config = "RelWithDebInfo",
    [switch] $Reconfigure,
    [string] $Generator = "Visual Studio 18 2026"
)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\env.ps1"

& "$PSScriptRoot\link_module.ps1"

# Native deps (Boost, OpenSSL, gRPC, protobuf) from vcpkg manifest deps\vcpkg.json.
& (Join-Path $VcpkgRoot "vcpkg.exe") install `
    --triplet $VcpkgTriplet `
    "--x-manifest-root=$(Join-Path $RepoRoot 'deps')" `
    "--x-install-root=$VcpkgInstalled" `
    --disable-metrics
if ($LASTEXITCODE -ne 0) { throw "vcpkg install failed" }

if ($Reconfigure -and (Test-Path (Join-Path $AcBuild "CMakeCache.txt"))) {
    Remove-Item (Join-Path $AcBuild "CMakeCache.txt")
}

if (-not (Test-Path (Join-Path $AcBuild "CMakeCache.txt"))) {
    $toolchain = (Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake") -replace "\\", "/"
    $protoDir  = (Join-Path $RepoRoot "proto") -replace "\\", "/"
    cmake -S $AcSource -B $AcBuild -G $Generator -A x64 `
        "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
        "-DVCPKG_TARGET_TRIPLET=$VcpkgTriplet" `
        "-DVCPKG_INSTALLED_DIR=$($VcpkgInstalled -replace '\\', '/')" `
        "-DVCPKG_MANIFEST_MODE=OFF" `
        "-DCMAKE_INSTALL_PREFIX=$($AcInstall -replace '\\', '/')" `
        "-DMYSQL_ROOT_DIR=$($MySqlRoot -replace '\\', '/')" `
        "-DACSDK_PROTO_DIR=$protoDir" `
        "-DACSDK_BUILD_TESTS=ON" `
        "-DTOOLS_BUILD=all" `
        "-DSCRIPTS=static" `
        "-DMODULES=static"
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
}

cmake --build $AcBuild --config $Config --parallel -- /m /nologo /v:minimal
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

cmake --install $AcBuild --config $Config
if ($LASTEXITCODE -ne 0) { throw "Install failed" }

Write-Host "Installed to $AcInstall"

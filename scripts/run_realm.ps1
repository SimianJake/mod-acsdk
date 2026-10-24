# Starts the local test realm: MySQL (if not running), authserver and worldserver, each in its own window.
#   .\scripts\run_realm.ps1                     # RelWithDebInfo build
#   .\scripts\run_realm.ps1 -Config Debug
#   .\scripts\run_realm.ps1 -DataDir D:\wow-data  # folder holding dbc/ maps/ vmaps/ mmaps/
# On first run it creates worldserver.conf / authserver.conf / mod_acsdk.conf from the .dist files
# and generates a random ACSDK.AuthToken (printed once; read it back from configs\modules\mod_acsdk.conf).
param(
    [ValidateSet("RelWithDebInfo", "Debug", "Release")] [string] $Config = "RelWithDebInfo",
    [string] $DataDir = "",
    [switch] $NoAuthServer,
    [switch] $SetupOnly       # start MySQL and write configs, but don't launch the servers
)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\env.ps1"
if ([string]::IsNullOrEmpty($DataDir)) { $DataDir = Join-Path $BuildRoot "data" }

$bin = Join-Path $AcBuild "bin\$Config"
$configs = Join-Path $bin "configs"
if (-not (Test-Path (Join-Path $bin "worldserver.exe"))) { throw "worldserver.exe not found in $bin; run scripts\build.ps1 first." }

# --- MySQL (plain process, data in $MySqlData) ---
$mysqladmin = Join-Path $MySqlRoot "bin\mysqladmin.exe"
& $mysqladmin -u root --host=127.0.0.1 --connect-timeout=2 ping 2>$null | Out-Null
if ($LASTEXITCODE -ne 0) {
    Write-Host "Starting MySQL..."
    Start-Process -FilePath (Join-Path $MySqlRoot "bin\mysqld.exe") -ArgumentList "--defaults-file=`"$MySqlData\my.ini`"" -WindowStyle Hidden
    for ($i = 0; $i -lt 30; $i++) {
        Start-Sleep 1
        & $mysqladmin -u root --host=127.0.0.1 --connect-timeout=2 ping 2>$null | Out-Null
        if ($LASTEXITCODE -eq 0) { break }
    }
    if ($LASTEXITCODE -ne 0) { throw "MySQL did not start" }
}

# worldserver/authserver load libmysql.dll at runtime; it brings MySQL's own OpenSSL DLLs. Those stay
# private to libmysql (worldserver itself links vcpkg's OpenSSL statically).
Copy-Item (Join-Path $MySqlRoot "lib\libmysql.dll") $bin -Force
Copy-Item (Join-Path $MySqlRoot "bin\libssl-3-x64.dll"), (Join-Path $MySqlRoot "bin\libcrypto-3-x64.dll") $bin -Force

# AzerothCore asserts that legacy.dll sits next to the exe (OpenSSLCrypto.cpp). Our static vcpkg OpenSSL
# is built with no-module, so the legacy provider is compiled in and the file is never loaded; we ship
# the matching-version legacy.dll from vcpkg's dynamic openssl build only to satisfy the check.
$legacyDll = Join-Path $BuildRoot "vcpkg_openssl_dll\x64-windows\bin\legacy.dll"
if (-not (Test-Path $legacyDll)) {
    & (Join-Path $VcpkgRoot "vcpkg.exe") install openssl:x64-windows --classic --disable-metrics "--x-install-root=$(Join-Path $BuildRoot 'vcpkg_openssl_dll')"
    if ($LASTEXITCODE -ne 0) { throw "vcpkg install openssl:x64-windows failed" }
}
Copy-Item $legacyDll $bin -Force

# --- Configs ---
function Initialize-Config($dist, $conf, [hashtable] $overrides) {
    if (Test-Path $conf) { return }
    $text = Get-Content $dist -Raw
    foreach ($key in $overrides.Keys) {
        $pattern = "(?m)^" + [regex]::Escape($key) + "\s*=.*$"
        $text = [regex]::Replace($text, $pattern, "$key = $($overrides[$key])")
    }
    Set-Content -Path $conf -Value $text -Encoding ascii
    Write-Host "Created $conf"
}

$dataDirConf = '"' + ($DataDir -replace "\\", "/") + '"'
Initialize-Config (Join-Path $configs "worldserver.conf.dist") (Join-Path $configs "worldserver.conf") @{
    "DataDir" = $dataDirConf
    "Console.Enable" = "1"
}
Initialize-Config (Join-Path $configs "authserver.conf.dist") (Join-Path $configs "authserver.conf") @{}

$modConf = Join-Path $configs "modules\mod_acsdk.conf"
if (-not (Test-Path $modConf)) {
    $bytes = New-Object byte[] 32
    [System.Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
    $token = -join ($bytes | ForEach-Object { $_.ToString("x2") })
    Initialize-Config (Join-Path $configs "modules\mod_acsdk.conf.dist") $modConf @{ "ACSDK.AuthToken" = "`"$token`"" }
    Write-Host "Generated ACSDK.AuthToken in $modConf"
}

# --- Servers ---
if ($SetupOnly) { return }
if (-not $NoAuthServer) {
    Start-Process -FilePath (Join-Path $bin "authserver.exe") -WorkingDirectory $bin
}
Start-Process -FilePath (Join-Path $bin "worldserver.exe") -WorkingDirectory $bin
Write-Host "Started realm from $bin (DataDir $DataDir)"

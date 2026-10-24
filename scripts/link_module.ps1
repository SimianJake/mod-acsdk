# Junctions this repository (it is the module) into the AzerothCore source tree's modules\ folder.
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\env.ps1"

$target = $RepoRoot
$link   = Join-Path $AcSource "modules\mod-acsdk"

if (Test-Path $link) {
    $item = Get-Item $link -Force
    if ($item.LinkType -eq "Junction" -and $item.Target -contains $target) {
        Write-Host "Already linked: $link -> $target"
        return
    }
    throw "$link already exists and is not a junction to $target; remove it first."
}

New-Item -ItemType Junction -Path $link -Target $target | Out-Null
Write-Host "Linked: $link -> $target"

# Stage the three Ryujinx projects of nxnative (seedds/nxnative, MIT; dm/third_party/ryujinx, with its Ampere decoder and emitters)
# into tools\shaders\third_party\nxnative (git-ignored). Source: the read-only clone refs\nxnative. Builds write obj/bin next to the
# sources, so we build from this copy and never touch refs\.
param([string]$Nxnative = (Join-Path $PSScriptRoot '..\..\..\..\..\..\refs\nxnative'))
$ErrorActionPreference = 'Stop'
$src = Join-Path $Nxnative 'dm\third_party\ryujinx'
if (-not (Test-Path "$src\src\Ryujinx.Graphics.Shader")) { throw "no nxnative clone at $Nxnative (git clone https://github.com/seedds/nxnative)" }
$dst = Join-Path $PSScriptRoot '..\..\third_party\nxnative'
New-Item -ItemType Directory -Force "$dst\src" | Out-Null
foreach ($p in 'Ryujinx.Common','Spv.Generator','Ryujinx.Graphics.Shader') {
    robocopy "$src\src\$p" "$dst\src\$p" /MIR /XD obj bin /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy $p failed ($LASTEXITCODE)" }
}
foreach ($f in 'Directory.Build.props','Directory.Packages.props','LICENSE.txt') { Copy-Item "$src\$f" "$dst\$f" -Force }
Write-Host "staged nxnative's Ryujinx sources in $dst"
$global:LASTEXITCODE = 0

# Build and run ONE NVN unit test without CMake (several people can do this at once, each with its own output folder).
#   pwsh scripts\nvn_unit.ps1 -Name sampler -Test tests\gfx\test_nvn_sampler.cpp -Src src\gfx\nvn\api_sampler.cpp,more.cpp [-Group gfx_nvn_sampler]
# -Src, -Lib and -Inc take a comma separated list (also through pwsh -File). Always linked in: src\gfx\nvn\nvn_loader.cpp, nvn_abi.cpp,
# tests\test_main.cpp and build\release\nxrt.lib (the OS layer, built by the normal build: scripts\build.ps1; this script never touches
# build\release). Output: build\unit\<Name>\unit.exe.
# Same flags as the CMake build: static MSVC runtime (/MT, like nxrt.lib), C++20. nxrt.lib needs the Windows libraries below and libvpx (the
# video decoders in nxrt.lib; vcpkg libvpx:x64-windows-static, found like cmake\media_deps.cmake does: $env:NXRT_LIBVPX_ROOT, then $env:VCPKG_ROOT,
# $env:VCPKG_ROOT). Without libvpx the link fails with vpx_* unresolved only if nxrt.lib was built with it.
# Note: PowerShell variable names are case-insensitive, so the parameter $Lib and the library path must not share a name.
param([Parameter(Mandatory)][string]$Name, [Parameter(Mandatory)][string]$Test, [string[]]$Src = @(), [string]$Group = "", [string[]]$Lib = @(), [string[]]$Inc = @(), [switch]$NoRun)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root "build\unit\$Name"
New-Item -ItemType Directory -Force $out | Out-Null
. (Join-Path $PSScriptRoot "proj.ps1")
function Split-List([string[]]$v) { @($v | ForEach-Object { $_ -split ',' } | Where-Object { $_ }) }
$Src = Split-List $Src; $Lib = Split-List $Lib; $Inc = Split-List $Inc
$srcs = @($Test) + $Src + @("src\gfx\nvn\nvn_loader.cpp", "src\gfx\nvn\nvn_abi.cpp", "tests\test_main.cpp") | ForEach-Object { if ([System.IO.Path]::IsPathRooted($_)) { $_ } else { Join-Path $root $_ } }
$srcList = ($srcs | ForEach-Object { '"' + $_ + '"' }) -join " "
$nxrtLib = Join-Path $root "build\release\nxrt.lib"
if (-not (Test-Path $nxrtLib)) { throw "build\release\nxrt.lib not found: build the runtime first (pwsh scripts\build.ps1)" }
# libvpx, when the machine has it (nxrt.lib references it if it was built with it)
$vpx = @($env:NXRT_LIBVPX_ROOT, $(if ($env:VCPKG_ROOT) { Join-Path $env:VCPKG_ROOT "installed\x64-windows-static" })) |
    Where-Object { $_ } | ForEach-Object { Join-Path $_ "lib\vpx.lib" } | Where-Object { Test-Path $_ } | Select-Object -First 1
$sys = @("psapi.lib", "user32.lib", "gdi32.lib", "shell32.lib", "ole32.lib", "oleaut32.lib", "advapi32.lib", "winmm.lib", "comdlg32.lib", "ws2_32.lib", "shlwapi.lib", "version.lib")
$libs = (@($Lib) + @($vpx) + $sys | Where-Object { $_ } | ForEach-Object { '"' + $_ + '"' }) -join " "
$incs = ($Inc | ForEach-Object { '/I"' + $_ + '"' }) -join " "
$env_cmd = Join-Path $PSScriptRoot "env.cmd"
$cl = "clang-cl /nologo /std:c++20 /O1 /EHsc /MT /W3 -Wno-unused-parameter -Wno-missing-field-initializers -Wno-cast-function-type-mismatch -Wno-cast-function-type-strict -Wno-language-extension-token " +
      "/DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /D_WIN32_WINNT=0x0A00 " +
      "/I`"$root\include`" /I`"$root\src\gfx`" /I`"$root\src\gfx\nvn`" /I`"$root\src\gfx\nvn\third_party`" /I`"$root\tests`" /I`"$root\build\release\_deps\volk-src`" /I`"$root\build\release\_deps\vulkan_headers-src\include`" /I`"$recomp`" $incs " +
      "$srcList /Fe`"$out\unit.exe`" /Fo`"$out/`" /link `"$nxrtLib`" $libs /STACK:8388608 /HIGHENTROPYVA:NO"
cmd /c "call `"$env_cmd`" && $cl"
if ($LASTEXITCODE -ne 0) { Write-Host "BUILD FAILED"; exit 1 }
if ($NoRun) { exit 0 }
$g = if ($Group) { $Group } else { "--all" }
& "$out\unit.exe" $g
exit $LASTEXITCODE

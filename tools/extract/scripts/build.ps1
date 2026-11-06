# Configure + build nx_extract (and its test) with CMake + Ninja + clang-cl; -Test runs the test.
#   pwsh -File tools\extract\scripts\build.ps1 [-Clean] [-Test] [-BuildDir path]
# Output: <BuildDir>\bin\nx_extract.exe (default tools\extract\build\release). NSZ/XCZ support needs tools\setup\scripts\fetch_zstd.ps1 first.
param([switch]$Clean, [switch]$Test, [string]$BuildDir = '')
$ErrorActionPreference = 'Stop'
$root  = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$build = if ($BuildDir) { $BuildDir } else { Join-Path $root 'build\release' }
$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
foreach ($line in (cmd /c "`"$vcvars`" >nul 2>&1 && set")) {
    $i = $line.IndexOf('=')
    if ($i -gt 0) { Set-Item -Path ("Env:" + $line.Substring(0, $i)) -Value $line.Substring($i + 1) }
}
$env:PATH = "C:\Program Files\LLVM\bin;C:\Program Files\CMake\bin;$env:LOCALAPPDATA\Microsoft\WinGet\Links;$env:PATH"
if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }
cmake -S $root -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
if ($LASTEXITCODE) { throw 'cmake configure failed' }
cmake --build $build
if ($LASTEXITCODE) { throw 'build failed' }
if ($Test) {
    ctest --test-dir $build --output-on-failure
    if ($LASTEXITCODE) { throw 'tests failed' }
}
Write-Output "built: $build\bin"

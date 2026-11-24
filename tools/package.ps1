<#
.SYNOPSIS
  Builds the distribution packages.

    ZombieDinos-<version>-setup.exe      single-file installer (Inno Setup 6)
    ZombieDinos-<version>-portable.zip   unzip and run; also what the updater downloads

.DESCRIPTION
  Default:  the recompiled executable, in dist\. It contains a translation of the
            game's own code, so keep it to yourself.
  -Public   dist\public\: the same program without any game code. It interprets
            the program on the player's disc, with identical results. This is
            the variant tools\release.ps1 publishes.

  Needs Visual Studio 2022 (C), CMake; for setup.exe also Inno Setup 6
  (winget install JRSoftware.InnoSetup); for the default variant also Python and
  the disc image in rom\.

    powershell -ExecutionPolicy Bypass -File tools\package.ps1 [-Public] [-SkipInstaller]
#>
param([switch]$Public, [switch]$SkipInstaller)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$src = Get-Content runtime\version.h -Raw
$ver = [regex]::Match($src, '#define APP_VERSION "([^"]+)"').Groups[1].Value
$repo = [regex]::Match($src, '#define UPDATE_REPO "([^"]+)"').Groups[1].Value
if (-not $ver -or -not $repo) { throw 'runtime\version.h: version or repository not found' }
$kind = if ($Public) { 'public' } else { 'recompiled' }
$out = if ($Public) { "$root\dist\public" } else { "$root\dist" }
$interp = if ($Public) { 'ON' } else { 'OFF' }
Write-Host "Packaging $ver ($kind)"

# 1. program
if (-not $Public) {
    python tools\gen.py
    if ($LASTEXITCODE) { throw 'gen.py failed (is the disc image in rom\?)' }
}
cmake -S . -B "build\package-$kind" -G 'Visual Studio 17 2022' -A x64 "-DRECOMP_INTERP_ONLY=$interp" -DRECOMP_PACKAGE=ON | Out-Null
if ($LASTEXITCODE) { throw 'cmake configure failed' }
cmake --build "build\package-$kind" --config Release --target game --parallel | Out-Null
if ($LASTEXITCODE) { throw 'build failed' }

# 2. staging folder: exactly what the player gets
$stage = "$root\build\package-$kind\stage"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage, $out | Out-Null
Copy-Item "build\package-$kind\Release\zombiedinos.exe", 'installer\README.txt' $stage

# it must need nothing that Windows does not have
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
$dumpbin = Get-ChildItem "$vs\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe" -ErrorAction SilentlyContinue | Select-Object -Last 1
if ($dumpbin) {
    $dlls = & $dumpbin /nologo /dependents "$stage\zombiedinos.exe" | Select-String '^\s+(\S+\.dll)$' | ForEach-Object { $_.Matches[0].Groups[1].Value }
    Write-Host "  uses: $($dlls -join ', ')"
    foreach ($d in $dlls) { if (-not (Test-Path "$env:SystemRoot\System32\$d") -or $d -match '^(vcruntime|msvcp|ucrtbase|api-ms)') { throw "unexpected dependency $d" } }
}

# 3. portable zip (flat: the updater copies these files over the program folder)
$zip = "$out\ZombieDinos-$ver-portable.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path "$stage\*" -DestinationPath $zip -CompressionLevel Optimal
Write-Host "  $zip"

# 4. installer
if (-not $SkipInstaller) {
    $iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
              "$env:ProgramFiles\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $iscc) { throw 'Inno Setup 6 not found (winget install JRSoftware.InnoSetup), or use -SkipInstaller' }
    & $iscc /Q "/DAppVersion=$ver" "/DRepo=$repo" "/DStageDir=$stage" "/DOutDir=$out" installer\zombiedinos.iss
    if ($LASTEXITCODE) { throw 'Inno Setup failed' }
    Write-Host "  $out\ZombieDinos-$ver-setup.exe"
}

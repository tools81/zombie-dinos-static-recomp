<#
  Publishes the version in runtime\version.h as a GitHub release, which installed
  copies then pick up. Builds and uploads the -Public packages only (no game code).
  Asks before uploading. Needs the GitHub CLI (gh auth login) and the repository
  named in version.h, public and with at least one commit.

    powershell -ExecutionPolicy Bypass -File tools\release.ps1 [-Notes "what changed"]
#>
param([string]$Notes = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$src = Get-Content runtime\version.h -Raw
$ver = [regex]::Match($src, '#define APP_VERSION "([^"]+)"').Groups[1].Value
$repo = [regex]::Match($src, '#define UPDATE_REPO "([^"]+)"').Groups[1].Value

$vis = gh repo view $repo --json visibility --jq .visibility
if ($LASTEXITCODE) { throw "repository $repo not found" }
if ($vis -ne 'PUBLIC') { throw "$repo is $vis. The update check is anonymous, so it needs a public repository." }
if ((gh release list -R $repo --json tagName --jq '.[].tagName') -contains "v$ver") { throw "v$ver is already released: raise APP_VERSION" }

& "$PSScriptRoot\package.ps1" -Public
$files = "dist\public\ZombieDinos-$ver-portable.zip", "dist\public\ZombieDinos-$ver-setup.exe"
if ((Read-Host "Publish v$ver to $repo? (y/n)") -ne 'y') { exit 1 }
gh release create "v$ver" @files -R $repo --title "Zombie Dinos $ver" --notes $Notes
if ($LASTEXITCODE) { throw 'gh release create failed' }

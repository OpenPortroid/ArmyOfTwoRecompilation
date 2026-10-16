# Prepares the ReXGlue SDK source used by this port: clones the SDK at the base
# commit (if the folder does not exist yet) and copies the port's changed files over it.
#   powershell -ExecutionPolicy Bypass -File sdk_changes\apply.ps1 [-Sdk C:\aot2\sdk]
param(
  [string]$Sdk = (Join-Path $PSScriptRoot "..\sdk"),
  [string]$Repo = "https://github.com/rexglue/rexglue-sdk",
  [string]$Base = "1406e1b"
)
$ErrorActionPreference = "Stop"
if (-not (Test-Path $Sdk)) {
  git clone $Repo $Sdk
  git -C $Sdk checkout $Base
  git -C $Sdk submodule update --init --recursive
}
$src = Join-Path $PSScriptRoot "files"
Get-ChildItem $src -Recurse -File | ForEach-Object {
  $rel = $_.FullName.Substring($src.Length + 1)
  $dst = Join-Path $Sdk $rel
  New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
  Copy-Item $_.FullName $dst -Force
  Write-Host $rel
}
Write-Host "Done. Build rexruntime with build-sdk.cmd."

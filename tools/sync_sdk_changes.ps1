# Copies the SDK files changed by the port (C:\aot2\sdk working tree vs. the base
# commit) into C:\aot2\sdk_changes, keeping their paths. Run after editing the SDK.
param(
  [string]$Sdk = "C:\aot2\sdk",
  [string]$Base = "1406e1b",
  [string]$Out = "C:\aot2\sdk_changes\files"
)
$ErrorActionPreference = "Stop"
$files = git -C $Sdk diff --name-only --diff-filter=AM $Base
$files += git -C $Sdk ls-files --others --exclude-standard src include
if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
foreach ($f in $files | Sort-Object -Unique) {
  $dst = Join-Path $Out $f
  New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
  Copy-Item (Join-Path $Sdk $f) $dst
  Write-Host $f
}
$deleted = git -C $Sdk diff --name-only --diff-filter=D $Base
if ($deleted) { Write-Warning "Deleted in the port (not handled by apply.ps1): $deleted" }

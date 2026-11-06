# End-to-end test of the launcher's self-update: fake packages and releases in a temporary folder, served through
# --releases-file with file:// URLs. Nothing of the game is run or needed, and the network is not used.
#   launcher\tools\test_update.ps1 [-Launcher <exe>]
param(
  [string]$Launcher = (Join-Path $PSScriptRoot "..\bin\ArmyOfTwoLauncher.exe")
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.IO.Compression.FileSystem
$Launcher = (Resolve-Path $Launcher).Path
$sandbox = Join-Path ([IO.Path]::GetTempPath()) ("ao2_update_test_" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory $sandbox | Out-Null
$failures = 0

function Write-Text($path, $text) {
  New-Item -ItemType Directory -Force (Split-Path $path) | Out-Null
  [IO.File]::WriteAllText($path, $text)
}

# The installed package, version "old".
function New-Package($name) {
  $dir = Join-Path $sandbox $name
  New-Item -ItemType Directory $dir | Out-Null
  Copy-Item $Launcher (Join-Path $dir "ArmyOfTwoLauncher.exe")
  Write-Text "$dir\armyoftworecomp.exe" "old"
  Write-Text "$dir\rexruntime.dll" "old"
  Write-Text "$dir\zzz_last.dll" "old"
  Write-Text "$dir\settings\hardware.toml" "user"
  return $dir
}

# A release: the archive, and SHA256SUMS.txt unless $Sums is "none"; "wrong" gives a wrong hash, "other" names
# another file. Returns the release list file for --releases-file.
function New-Release($name, $version, $Sums = "good") {
  $dir = Join-Path $sandbox $name
  $pkg = "$dir\stage\ArmyOfTwo"
  Copy-Item $Launcher (New-Item -ItemType Directory -Force $pkg).FullName
  Write-Text "$pkg\armyoftworecomp.exe" "new"
  Write-Text "$pkg\rexruntime.dll" "new"
  Write-Text "$pkg\zzz_last.dll" "new"
  Write-Text "$pkg\settings\hardware.toml" "default"
  Write-Text "$pkg\new_folder\extra.txt" "new"
  $zipName = "ArmyOfTwo-v$version.zip"
  $zip = "$dir\$zipName"
  [IO.Compression.ZipFile]::CreateFromDirectory("$dir\stage", $zip)
  $assets = @(@{ browser_download_url = ([Uri]$zip).AbsoluteUri })
  if ($Sums -ne "none") {
    $hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant()
    $line = switch ($Sums) {
      "good" { "$hash *$zipName" }
      "wrong" { ("0" * 64) + " *$zipName" }
      "other" { "$hash *something_else.zip" }
    }
    Write-Text "$dir\SHA256SUMS.txt" (("1" * 64) + "  avx1-version.aot`n$line`n")
    $assets += @{ browser_download_url = ([Uri]"$dir\SHA256SUMS.txt").AbsoluteUri }
  }
  $json = "$dir\releases.json"
  # An array of one: PowerShell would unwrap it.
  # tag_name first, as in GitHub's answer: the launcher reads a release's fields after it.
  Write-Text $json ("[" + ([ordered]@{ tag_name = "v$version"; draft = $false; prerelease = $false; assets = $assets } | ConvertTo-Json -Depth 4) + "]")
  return $json
}

# Runs the launcher to its end and returns what --self-update logged.
function Invoke-Update($pkg, $json, $current) {
  $log = "$pkg\self_update.log"
  $p = Start-Process "$pkg\ArmyOfTwoLauncher.exe" -PassThru -Wait -WorkingDirectory $pkg `
    -ArgumentList "--self-update=$log", "--releases-file=$json", "--pretend-version=$current"
  if ($p.ExitCode -ne 0) { throw "the launcher exited with $($p.ExitCode)" }
  $text = Get-Content $log -Raw
  Write-Host ($text -replace "(?m)^", "    | ")
  return $text
}

function Check($case, $ok, $what) {
  if ($ok) {
    Write-Host "  ok    $what"
  } else {
    Write-Host "  FAIL  $what"
    $script:failures++
  }
}

function Assert-Old($case, $pkg) {
  Check $case ((Get-Content "$pkg\armyoftworecomp.exe" -Raw) -eq "old") "the game is the old one"
  Check $case ((Get-Content "$pkg\rexruntime.dll" -Raw) -eq "old") "the runtime is the old one"
  Check $case ((Get-Content "$pkg\zzz_last.dll" -Raw) -eq "old") "the last file is the old one"
  Check $case (-not (Test-Path "$pkg\new_folder\extra.txt")) "no new file is left"
  Check $case (-not (Test-Path "$pkg\ArmyOfTwoLauncher.exe.old")) "the launcher was not swapped"
  Check $case (Test-Path "$pkg\ArmyOfTwoLauncher.exe") "the launcher is there"
}

try {
  Write-Host "good release"
  $pkg = New-Package "good"
  $log = Invoke-Update $pkg (New-Release "good_rel" "9.9.9") "1.4.3"
  Check "good" ($log -match "(?m)^OK") "logged OK"
  Check "good" ((Get-Content "$pkg\armyoftworecomp.exe" -Raw) -eq "new") "the game is replaced"
  Check "good" ((Get-Content "$pkg\zzz_last.dll" -Raw) -eq "new") "the last file is replaced"
  Check "good" ((Get-Content "$pkg\new_folder\extra.txt" -Raw) -eq "new") "a new file is added"
  Check "good" ((Get-Content "$pkg\settings\hardware.toml" -Raw) -eq "user") "the user's settings are kept"
  Check "good" (Test-Path "$pkg\ArmyOfTwoLauncher.exe.old") "the running launcher is set aside"
  Check "good" (-not (Test-Path "$pkg\update.tmp")) "update.tmp is removed"

  Write-Host "wrong hash"
  $pkg = New-Package "wrong"
  $log = Invoke-Update $pkg (New-Release "wrong_rel" "9.9.9" "wrong") "1.4.3"
  Check "wrong" ($log -match "damaged or was changed") "refused"
  Assert-Old "wrong" $pkg

  Write-Host "sums without the archive"
  $pkg = New-Package "other"
  $log = Invoke-Update $pkg (New-Release "other_rel" "9.9.9" "other") "1.4.3"
  Check "other" ($log -match "has no ArmyOfTwo-v9\.9\.9\.zip") "refused"
  Assert-Old "other" $pkg

  Write-Host "new release without sums"
  $pkg = New-Package "nosums"
  $log = Invoke-Update $pkg (New-Release "nosums_rel" "9.9.9" "none") "1.4.3"
  Check "nosums" ($log -match "it cannot be checked") "refused"
  Assert-Old "nosums" $pkg

  Write-Host "old release without sums"
  $pkg = New-Package "oldrel"
  $log = Invoke-Update $pkg (New-Release "oldrel_rel" "1.4.3" "none") "1.4.2"
  Check "oldrel" ($log -match "(?m)^OK") "installed"
  Check "oldrel" ((Get-Content "$pkg\armyoftworecomp.exe" -Raw) -eq "new") "the game is replaced"

  Write-Host "a file in use: rollback"
  $pkg = New-Package "locked"
  $json = New-Release "locked_rel" "9.9.9"
  $lock = [IO.File]::Open("$pkg\zzz_last.dll", "Open", "Read", "None")
  try {
    $log = Invoke-Update $pkg $json "1.4.3"
  } finally {
    $lock.Close()
  }
  Check "locked" ($log -match "the previous version is restored") "says it restored the package"
  Assert-Old "locked" $pkg
  Check "locked" ((Get-Content "$pkg\settings\hardware.toml" -Raw) -eq "user") "the user's settings are kept"
  Check "locked" (-not (Test-Path "$pkg\update.tmp")) "update.tmp is removed"

  Write-Host "interrupted update: recovery at the next start"
  $pkg = New-Package "crash"
  # The state a power cut leaves: a file replaced, one new. The marker's process id is another program's now.
  Write-Text "$pkg\update.tmp\installing" "0"
  Move-Item "$pkg\armyoftworecomp.exe" (New-Item -ItemType Directory "$pkg\update.tmp\backup").FullName
  Write-Text "$pkg\armyoftworecomp.exe" "new, cut sho"
  Write-Text "$pkg\new_folder\extra.txt" "new"
  Write-Text "$pkg\update.tmp\created.txt" "new_folder\extra.txt`r`n"
  Start-Process "$pkg\ArmyOfTwoLauncher.exe" -Wait -WorkingDirectory $pkg -ArgumentList "--cpu-check=$pkg\cpu.txt"
  Assert-Old "crash" $pkg
  Check "crash" (-not (Test-Path "$pkg\update.tmp")) "update.tmp is removed"

  Write-Host "another launcher still installing: hands off"
  $pkg = New-Package "live"
  # A process with the launcher's name that keeps running: a copy of cmd.exe, so no window opens.
  New-Item -ItemType Directory "$sandbox\busy" | Out-Null
  Copy-Item "$env:WINDIR\System32\cmd.exe" "$sandbox\busy\ArmyOfTwoLauncher.exe"
  $busy = Start-Process "$sandbox\busy\ArmyOfTwoLauncher.exe" -PassThru -WindowStyle Hidden -ArgumentList "/c ping -n 60 127.0.0.1"
  try {
    Write-Text "$pkg\update.tmp\installing" "$($busy.Id)"
    Move-Item "$pkg\armyoftworecomp.exe" (New-Item -ItemType Directory "$pkg\update.tmp\backup").FullName
    Write-Text "$pkg\armyoftworecomp.exe" "new"
    Start-Process "$pkg\ArmyOfTwoLauncher.exe" -Wait -WorkingDirectory $pkg -ArgumentList "--cpu-check=$pkg\cpu.txt"
    Check "live" ((Get-Content "$pkg\armyoftworecomp.exe" -Raw) -eq "new") "the file is left alone"
    Check "live" (Test-Path "$pkg\update.tmp\backup\armyoftworecomp.exe") "the backup is left alone"
  } finally {
    Stop-Process -Id $busy.Id -Force -ErrorAction SilentlyContinue
    $busy.WaitForExit()
  }
} finally {
  Remove-Item $sandbox -Recurse -Force -ErrorAction SilentlyContinue
}

if ($failures) {
  Write-Host "$failures check(s) failed"
  exit 1
}
Write-Host "all checks passed"
exit 0

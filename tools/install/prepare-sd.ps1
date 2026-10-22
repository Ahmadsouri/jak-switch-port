<#
  Jak and Daxter: The Precursor Legacy - Nintendo Switch port
  One-click data preparation.

  Turns a copy of the game that YOU own into the data folder the port loads from
  the SD card. Nothing is downloaded except the official OpenGOAL extractor.

  Usage (from the release bundle):
      prepare-sd.bat "D:\Jak and Daxter.iso"          # drag the ISO onto the .bat

  Or from PowerShell:
      .\prepare-sd.ps1 -Iso "D:\game.iso"
      .\prepare-sd.ps1 -Iso "D:\game.iso" -SdDrive E:   # also copy to the SD card
#>
[CmdletBinding()]
param(
    # Path to your own ISO (or an already extracted "jak1" folder with -Extracted).
    [Parameter(Position = 0)][string]$Iso,
    # Optional: extractor.exe if you already have one (otherwise it is downloaded).
    [string]$Extractor,
    # Optional: drive letter of the SD card, e.g. E:
    [string]$SdDrive,
    # Optional: where the heavy work happens (must be a short path).
    [string]$WorkDir = (Join-Path $env:LOCALAPPDATA 'jak1switch'),
    # Recompile from an extraction that is already there.
    [switch]$Recompile,
    # Input is a premade "iso_data\jak1" folder instead of an ISO.
    [switch]$Extracted
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
trap {
    Write-Host ''
    Write-Host "STOPPED: $_" -ForegroundColor Red
    exit 1
}
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$OpenGoalVersion = 'v0.3.8'
$OpenGoalZipUrl  = "https://github.com/open-goal/jak-project/releases/download/$OpenGoalVersion/opengoal-windows-$OpenGoalVersion.zip"

function Write-Step([string]$Text) {
    Write-Host ''
    Write-Host "==> $Text" -ForegroundColor Cyan
}
function Write-Ok([string]$Text)   { Write-Host "    $Text" -ForegroundColor Green }
function Write-Warn2([string]$Text){ Write-Host "    $Text" -ForegroundColor Yellow }

function Find-BundleRoot {
    # setup\ lives next to switch\; when run from the git checkout we walk up to the repo root.
    $candidates = @(
        (Join-Path $PSScriptRoot '..'),          # setup\ -> </>Jak1-Switch-v1.0.0
        (Join-Path $PSScriptRoot '..\..'),       # tools\install\ -> repo root
        $PSScriptRoot
    )
    foreach ($c in $candidates) {
        $full = [IO.Path]::GetFullPath($c)
        if (Test-Path (Join-Path $full 'switch\jak1')) { return $full }
        if (Test-Path (Join-Path $full 'goal_src'))   { return $full }
    }
    throw "Cannot find the bundle root (the folder that contains 'switch' or 'goal_src')."
}

function Find-Proj {
    param([string]$Root)
    $local = Join-Path $PSScriptRoot 'proj'
    if (Test-Path (Join-Path $local 'goal_src')) { return $local }
    if (Test-Path (Join-Path $Root 'goal_src'))  { return $Root }
    throw "Cannot find the compiler project folder (goal_src)."
}

function Resolve-Extractor {
    param([string]$Explicit, [string]$Proj, [string]$Work)
    if ($Explicit) {
        if (-not (Test-Path $Explicit)) { throw "extractor.exe not found at $Explicit" }
        return (Get-Item $Explicit).FullName
    }
    foreach ($c in @((Join-Path $PSScriptRoot 'extractor.exe'),
                     (Join-Path $Proj 'extractor.exe'),
                     (Join-Path $Work 'opengoal\extractor.exe'))) {
        if (Test-Path $c) { return (Get-Item $c).FullName }
    }
    $zip = Join-Path $Work "opengoal-$OpenGoalVersion.zip"
    Write-Step "Downloading the official OpenGOAL $OpenGoalVersion tools (~43 MB, one time)"
    if (-not (Test-Path $zip)) {
        Invoke-WebRequest -Uri $OpenGoalZipUrl -OutFile $zip -UseBasicParsing
    }
    $dest = Join-Path $Work 'opengoal'
    if (Test-Path $dest) { Remove-Item $dest -Recurse -Force }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::ExtractToDirectory($zip, $dest)
    $exe = Join-Path $dest 'extractor.exe'
    if (-not (Test-Path $exe)) { throw "extractor.exe missing from the downloaded zip." }
    Write-Ok "extractor: $exe"
    return $exe
}

function Invoke-Extractor {
    param([string]$Exe, [string]$Arguments, [string]$What)
    # Start-Process passes the argument line verbatim, which is what we need for the
    # --decomp-config-override JSON (PowerShell 5.1 would otherwise strip its quotes).
    $p = Start-Process -FilePath $Exe -ArgumentList $Arguments -NoNewWindow -Wait -PassThru
    if ($p.ExitCode -ne 0) { throw "$What failed (extractor exit code $($p.ExitCode))." }
}

# ---------------------------------------------------------------- paths ----
$root = Find-BundleRoot
$projSource = Find-Proj -Root $root
$WorkDir = [IO.Path]::GetFullPath($WorkDir)
$proj = Join-Path $WorkDir 'proj'
# Running inside the git checkout? Use it in place, there is nothing to copy.
$inRepo = Test-Path (Join-Path $projSource 'CMakeLists.txt')
if ($inRepo) { $proj = $projSource }
$isoJak1 = Join-Path $proj 'iso_data\jak1'
$outJak1 = Join-Path $proj 'out\jak1'
$destDir = Join-Path $root 'switch\jak1\data\out\jak1'

Write-Host 'Jak 1 on Nintendo Switch - data preparation' -ForegroundColor White
Write-Host 'This uses a copy of the game that you own. No game data is included.' -ForegroundColor DarkGray

# ---------------------------------------------------------------- input ----
if (-not $Iso) {
    Write-Host ''
    Write-Host 'Drag your "Jak and Daxter - The Precursor Legacy" ISO onto this window and press Enter:'
    $Iso = (Read-Host).Trim('"', ' ')
}
if (-not $Iso) { throw 'No ISO given.' }
$Iso = [IO.Path]::GetFullPath($Iso)
if (-not (Test-Path $Iso)) { throw "Not found: $Iso" }
if (-not $Extracted -and (Get-Item $Iso).PSIsContainer) { $Extracted = $true }
if (-not $Extracted -and $Iso -notmatch '\.iso$') {
    Write-Warn2 "That file is not a .iso, continuing anyway."
}

# ---------------------------------------------------------------- work -----
Write-Step "Preparing the work folder: $WorkDir"
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
if ($inRepo) {
    Write-Ok "using the checkout in place: $proj"
} elseif (-not (Test-Path $proj)) {
    Write-Ok 'copying the compiler project (about 150 MB, one time)'
    Copy-Item -Path $projSource -Destination $proj -Recurse -Force
    New-Item -ItemType Directory -Force -Path (Join-Path $proj 'iso_data') | Out-Null
} else {
    Write-Ok 're-using the previous work folder'
}
$exe = Resolve-Extractor -Explicit $Extractor -Proj $proj -Work $WorkDir

# ------------------------------------------------------- 1. extract -------
if ($Extracted) {
    Write-Step '1/3 Extracting the ISO - skipped, using the folder you gave'
    if (-not (Test-Path $isoJak1)) {
        New-Item -ItemType Directory -Force -Path $isoJak1 | Out-Null
        Copy-Item -Path (Join-Path $Iso '*') -Destination $isoJak1 -Recurse -Force
    }
} elseif (Test-Path (Join-Path $isoJak1 'DGO')) {
    Write-Step '1/3 Extracting the ISO - already done'
} else {
    Write-Step '1/3 Extracting the ISO (~1.4 GB, a few minutes)'
    $a = '--proj-path "{0}" --extract-path "{1}" -e -v --disable-ansi "{2}"' -f $proj, (Join-Path $proj 'iso_data'), $Iso
    Invoke-Extractor -Exe $exe -Arguments $a -What 'Extraction'
    if (-not (Test-Path (Join-Path $isoJak1 'DGO'))) { throw "Extraction produced no data at $isoJak1" }
}

function Count-Files([string]$Path) {
    if (-not (Test-Path $Path)) { return 0 }
    return (Get-ChildItem -Path $Path -Recurse -File).Count
}

# --------------------------------------------------- 2. decompile levels --
# Apart from decompiler_out\, this step is what produces out\jak1\fr3 (the level art).
# A compile on its own only ever writes the test-zone .fr3.
$fr3Count = Count-Files (Join-Path $outJak1 'fr3')
if ((Test-Path (Join-Path $proj 'decompiler_out')) -and $fr3Count -ge 20 -and -not $Recompile) {
    Write-Step '2/3 Reading the level data - already done'
} else {
    Write-Step '2/3 Reading the level data (a few minutes)'
    # decompile_code:false -> only the level geometry/entities the compiler needs.
    $json = '{\"decompile_code\": false, \"levels_extract\": true, \"allowed_objects\": []}'
    $a = '--proj-path "{0}" -g jak1 -d --disable-ansi --decomp-config-override "{1}" "{2}"' -f $proj, $json, $isoJak1
    Invoke-Extractor -Exe $exe -Arguments $a -What 'Level extraction'
}

# ------------------------------------------------- 3. compile for arm64 ---
Write-Step '3/3 Compiling the game data for ARM64 (the long one, a few minutes)'
# The compiler opens out\jak1\fr3\... but does not create the folder itself.
foreach ($sub in @('obj', 'iso', 'fr3')) {
    New-Item -ItemType Directory -Force -Path (Join-Path $outJak1 $sub) | Out-Null
}
$objCount = Count-Files (Join-Path $outJak1 'obj')
$isoCount = Count-Files (Join-Path $outJak1 'iso')
if ($Recompile -or $objCount -lt 1000 -or $isoCount -lt 300) {
    $a = '--proj-path "{0}" -g jak1 -c --instruction-set arm64 --disable-ansi "{1}"' -f $proj, $isoJak1
    Invoke-Extractor -Exe $exe -Arguments $a -What 'Compilation'
} else {
    Write-Ok 'already compiled'
}
$objCount = Count-Files (Join-Path $outJak1 'obj')
$isoCount = Count-Files (Join-Path $outJak1 'iso')
$fr3Count = Count-Files (Join-Path $outJak1 'fr3')
Write-Ok "compiled: $objCount object files, $isoCount level files, $fr3Count level-art files"
if ($objCount -lt 1000 -or $isoCount -lt 300 -or $fr3Count -lt 20) {
    throw "The result looks incomplete (obj=$objCount iso=$isoCount fr3=$fr3Count). Re-run with -Recompile."
}

# ------------------------------------------------------------- deliver ---
Write-Step 'Putting the result next to gk.nro'
$dataDir = Join-Path $root 'switch\jak1\data'
New-Item -ItemType Directory -Force -Path (Join-Path $dataDir 'out') | Out-Null
if (Test-Path $destDir) { Remove-Item $destDir -Recurse -Force }
try {
    Move-Item -Path $outJak1 -Destination $destDir
} catch {
    Copy-Item -Path $outJak1 -Destination $destDir -Recurse -Force
}
$size = (Get-ChildItem -Path $destDir -Recurse -File | Measure-Object -Property Length -Sum).Sum
Write-Ok ('ready: ' + $destDir + '  (' + [math]::Round($size / 1GB, 2) + ' GB)')

# ---------------------------------------------------------------- SD ------
if ($SdDrive) {
    $target = Join-Path $SdDrive 'switch\jak1'
    Write-Step "Copying to the SD card: $target"
    if (-not (Test-Path $SdDrive)) { throw "Drive not found: $SdDrive" }
    New-Item -ItemType Directory -Force -Path $target | Out-Null
    Copy-Item -Path (Join-Path $root 'switch\jak1\*') -Destination $target -Recurse -Force
    Write-Ok 'copied to the SD card.'
} else {
    Write-Host ''
    Write-Host 'NOW:' -ForegroundColor White
    Write-Host "  Copy the folder  switch  from"
    Write-Host "      $root"
    Write-Host '  to the root of your SD card, so you end up with'
    Write-Host '      sdmc:/switch/jak1/gk.nro'
    Write-Host '      sdmc:/switch/jak1/data/...'
}
Write-Host ''
Write-Host 'Launch gk.nro from the homebrew menu. Hold R while starting it if you get a memory error.' -ForegroundColor Green

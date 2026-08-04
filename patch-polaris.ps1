<#
  Benro Polaris libgphoto2 patcher - Windows launcher (PowerShell)

  Everything runs inside Docker; the only host requirement is Docker Desktop.

  Usage:
    .\patch-polaris.ps1 -FwPkt <FwPkt-folder-or-zip> [options]

  Options:
    -FwPkt PATH          stock FwPkt folder (has firmwareInfo) or FwPkt.zip  [required]
    -Libgphoto2 VER      libgphoto2 release to build            (default 2.5.34)
    -Out DIR             output directory                       (default .\out)
    -SelfTest            qemu-emulate the driver load (R5 II registration)
    -NoFixTypo           do NOT correct the upstream "EOS 5Rm2" model typo
    -Image NAME          docker image tag              (default polaris-patcher)

  READ THE README AND DISCLAIMERS FIRST. Tested ONLY against FwVer 4.0.0.32
  with a Canon EOS R5 Mark II. Flashing firmware is at YOUR OWN RISK.
#>
param(
  [Parameter(Mandatory=$true)][string]$FwPkt,
  [string]$Libgphoto2 = "2.5.34",
  [string]$Out = "",
  [switch]$SelfTest,
  [switch]$NoFixTypo,
  [string]$Image = "polaris-patcher"
)
$ErrorActionPreference = "Stop"
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrEmpty($Out)) { $Out = Join-Path $Here "out" }

if (-not (Get-Command docker -ErrorAction SilentlyContinue)) { throw "docker not found. Install Docker Desktop." }
docker info *> $null; if ($LASTEXITCODE -ne 0) { throw "Docker daemon not running." }

# resolve input into a folder containing firmwareInfo
$Stage = Join-Path ([System.IO.Path]::GetTempPath()) ("polpatch_" + [System.Guid]::NewGuid().ToString("N"))
$In = $null
try {
  if ((Test-Path -PathType Container $FwPkt) -and (Test-Path (Join-Path $FwPkt "firmwareInfo"))) { $In = (Resolve-Path $FwPkt).Path }
  elseif ((Test-Path -PathType Container $FwPkt) -and (Test-Path (Join-Path $FwPkt "FwPkt\firmwareInfo"))) { $In = (Resolve-Path (Join-Path $FwPkt "FwPkt")).Path }
  elseif (Test-Path -PathType Leaf $FwPkt) {
    Write-Host "[*] extracting $FwPkt ..."
    New-Item -ItemType Directory -Force -Path $Stage | Out-Null
    Expand-Archive -Path $FwPkt -DestinationPath $Stage -Force
    if (Test-Path (Join-Path $Stage "firmwareInfo")) { $In = $Stage }
    elseif (Test-Path (Join-Path $Stage "FwPkt\firmwareInfo")) { $In = (Join-Path $Stage "FwPkt") }
    else { throw "could not find firmwareInfo inside the zip." }
  } else { throw "-FwPkt must be a FwPkt folder (with firmwareInfo) or a FwPkt.zip" }

  New-Item -ItemType Directory -Force -Path $Out | Out-Null
  Write-Host "[*] building docker image '$Image' (first run only)..."
  docker build -q -t $Image -f (Join-Path $Here "docker\Dockerfile") $Here | Out-Null

  $fix = if ($NoFixTypo) { "0" } else { "1" }
  $st  = if ($SelfTest)  { "1" } else { "0" }
  Write-Host "[*] running patcher..."
  docker run --rm `
    -e LIBGPHOTO2_VERSION=$Libgphoto2 -e FIX_R5M2_TYPO=$fix -e SELFTEST=$st `
    -v "${In}:/in:ro" -v "${Out}:/out" `
    $Image

  Write-Host ""
  Write-Host "[OK] Output in: $Out"
  Write-Host "     - $Out\FwPkt\        (unpacked custom firmware)"
  Write-Host "     - $Out\FwPkt.zip     (copy this to your SD card)"
  Write-Host "     Keep your STOCK FwPkt as the factory-restore image."
}
finally {
  if (Test-Path $Stage) { Remove-Item -Recurse -Force $Stage -ErrorAction SilentlyContinue }
}

<#
  Benro Polaris libgphoto2 patcher - Windows launcher (PowerShell)

  Everything runs inside Docker; the only host requirement is Docker Desktop
  (with the Linux container engine, i.e. WSL2 or Hyper-V backend).

  Usage:
    .\patch-polaris.ps1 -FwPkt <FwPkt-folder-or-zip> [options]

  Options:
    -FwPkt PATH          stock FwPkt folder (has firmwareInfo) or FwPkt.zip  [required]
    -Libgphoto2 VER      libgphoto2 release to build            (default 2.5.34)
    -Out DIR             output directory                       (default .\out)
    -Ptp2Only            conservative fallback: keep the stock 2.5.27 core, swap only
                         the ptp2 camlib + usb1 iolib (+ 14-byte pgphoto patch).
                         DEFAULT (no flag) is the full-libgphoto2 stack swap.
    -SelfTest            qemu-emulate the driver load (R5 II registration)
    -NoFixTypo           do NOT correct the upstream "EOS 5Rm2" model typo
    -NoUsb1              (ptp2-only) do NOT swap the usb1 iolib; patch ptp2 + pgphoto only
    -SshKey KEY          enable SSH debugging: authorise this PUBLIC key for root
                         login (path to a .pub / authorized_keys file, or the key
                         line itself). Adds one new appfs file; the stock firmware
                         already runs sshd. Accepts several: -SshKey a.pub,b.pub
    -Image NAME          docker image tag              (default polaris-patcher)

  READ THE README AND DISCLAIMERS FIRST. Tested ONLY against FwVer 4.0.0.32
  with a Canon EOS R5 Mark II. Flashing firmware is at YOUR OWN RISK.
#>
param(
  [Parameter(Mandatory=$true)][string]$FwPkt,
  [string]$Libgphoto2 = "2.5.34",
  [string]$Out = "",
  [switch]$Ptp2Only,
  [switch]$SelfTest,
  [switch]$NoFixTypo,
  [switch]$NoUsb1,
  [string[]]$SshKey = @(),
  [string]$Image = "polaris-patcher"
)
$ErrorActionPreference = "Stop"
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrEmpty($Out)) { $Out = Join-Path $Here "out" }

# Docker Desktop wants a real drive-letter path with forward slashes for -v.
# Resolve-Path can hand back a PowerShell-provider path (FileSystem::C:\...),
# and a trailing backslash would eat the ':' separator in "path:/in:ro".
function ConvertTo-DockerPath {
  param([Parameter(Mandatory=$true)][string]$Path)
  $full = (Resolve-Path -LiteralPath $Path).ProviderPath
  if ($full.StartsWith("\\")) {
    throw "Docker cannot bind-mount a UNC path ($full). Copy it to a local drive, or map it to a drive letter."
  }
  return ($full.TrimEnd('\','/') -replace '\\','/')
}

if (-not (Get-Command docker -ErrorAction SilentlyContinue)) { throw "docker not found. Install Docker Desktop." }
docker info *> $null; if ($LASTEXITCODE -ne 0) { throw "Docker daemon not running." }

# ---- collect SSH public keys (files or literal key lines) -------------------
$SshPubKey = ""
if ($SshKey.Count -gt 0) {
  $lines = @()
  foreach ($k in $SshKey) {
    if (Test-Path -LiteralPath $k -PathType Leaf) {
      $lines += (Get-Content -LiteralPath $k)
    } elseif ($k -match '^(ssh-|ecdsa-|rsa-sha2-|sk-)') {
      $lines += $k
    } else {
      throw "-SshKey '$k' is neither a readable .pub/authorized_keys file nor a public key line."
    }
  }
  $lines = $lines | ForEach-Object { $_.TrimEnd("`r") } | Where-Object { $_.Trim() -ne "" }
  if ($lines -match 'PRIVATE KEY') { throw "-SshKey was given a PRIVATE key. Pass the .pub file instead." }
  $SshPubKey = ($lines -join "`n")
}

# resolve input into a folder containing firmwareInfo
$Stage = Join-Path ([System.IO.Path]::GetTempPath()) ("polpatch_" + [System.Guid]::NewGuid().ToString("N"))
$In = $null
try {
  if ((Test-Path -PathType Container $FwPkt) -and (Test-Path (Join-Path $FwPkt "firmwareInfo"))) { $In = (Resolve-Path $FwPkt).ProviderPath }
  elseif ((Test-Path -PathType Container $FwPkt) -and (Test-Path (Join-Path $FwPkt "FwPkt\firmwareInfo"))) { $In = (Resolve-Path (Join-Path $FwPkt "FwPkt")).ProviderPath }
  elseif (Test-Path -PathType Leaf $FwPkt) {
    Write-Host "[*] extracting $FwPkt ..."
    New-Item -ItemType Directory -Force -Path $Stage | Out-Null
    Expand-Archive -Path $FwPkt -DestinationPath $Stage -Force
    if (Test-Path (Join-Path $Stage "firmwareInfo")) { $In = $Stage }
    elseif (Test-Path (Join-Path $Stage "FwPkt\firmwareInfo")) { $In = (Join-Path $Stage "FwPkt") }
    else { throw "could not find firmwareInfo inside the zip." }
  } else { throw "-FwPkt must be a FwPkt folder (with firmwareInfo) or a FwPkt.zip" }

  New-Item -ItemType Directory -Force -Path $Out | Out-Null
  $InMount  = ConvertTo-DockerPath $In
  $OutMount = ConvertTo-DockerPath $Out

  Write-Host "[*] building docker image '$Image' (first run only)..."
  # NOT quiet, and the exit code IS checked: a silent build failure used to let
  # this script sail on and report success while the container was unusable.
  docker build -t $Image -f (Join-Path $Here "docker\Dockerfile") $Here
  if ($LASTEXITCODE -ne 0) { throw "docker build failed (exit $LASTEXITCODE). The build output above says why; nothing was patched." }

  $fix  = if ($NoFixTypo) { "0" } else { "1" }
  $st   = if ($SelfTest)  { "1" } else { "0" }
  $usb1 = if ($NoUsb1)    { "0" } else { "1" }
  $mode = if ($Ptp2Only)  { "ptp2only" } else { "full" }
  Write-Host "[*] running patcher (mode: $mode)..."
  if ($SshPubKey -ne "") {
    Write-Host ("[*] SSH debugging: authorising {0} public key(s) for root login" -f ($SshPubKey -split "`n").Count)
  }

  $runArgs = @(
    "run", "--rm",
    "-e", "MODE=$mode",
    "-e", "LIBGPHOTO2_VERSION=$Libgphoto2",
    "-e", "FIX_R5M2_TYPO=$fix",
    "-e", "SELFTEST=$st",
    "-e", "SWAP_USB1=$usb1",
    "-e", "SSH_PUBKEY=$SshPubKey",
    "-v", "${InMount}:/in:ro",
    "-v", "${OutMount}:/out",
    $Image
  )
  docker @runArgs
  if ($LASTEXITCODE -ne 0) { throw "the patcher container failed (exit $LASTEXITCODE). No usable firmware was written - do NOT flash anything from $Out." }

  Write-Host ""
  Write-Host "[OK] Output in: $Out"
  Write-Host "     - $Out\FwPkt\        (unpacked custom firmware)"
  Write-Host "     - $Out\FwPkt.zip     (copy this to your SD card)"
  if ($SshPubKey -ne "") {
    Write-Host "     - $Out\ssh-debug\    (the SSH hook that went in, + how to install/remove it)"
  }
  Write-Host "     Keep your STOCK FwPkt as the factory-restore image."
}
finally {
  if (Test-Path $Stage) { Remove-Item -Recurse -Force $Stage -ErrorAction SilentlyContinue }
}

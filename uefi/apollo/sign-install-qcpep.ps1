<#
Test-sign the LMh-patched qcpep8250.sys (patch-qcpep-lmh.py) and put it into the phone's offline Windows,
replacing every copy of the original 1.0.2140.0 driver; optionally turn the kernel debugger off in the BCD.

Run in an elevated PowerShell on the Windows PC, with the phone in OrangeFox mass-storage mode and its Windows
partition (and EFI partition, for -EspDrive) mounted:

  powershell -ExecutionPolicy Bypass -File sign-install-qcpep.ps1 -Sys .\qcpep8250-lmhfix.sys -WindowsDrive W: -EspDrive S: -DebugOff

qcpep's INF installs it "run from driver store" (%13%), so the copy Windows loads is
Windows\System32\DriverStore\FileRepository\qcpep.*\qcpep8250.sys. Every original copy is kept as
qcpep8250.sys.orig next to it; -Restore puts them back.
#>
param(
    [string]$Sys,
    [Parameter(Mandatory = $true)][string]$WindowsDrive,
    [string]$EspDrive,
    [switch]$DebugOff,
    [switch]$Restore
)
$ErrorActionPreference = 'Stop'
$OriginalSha256 = '39FAFCD60536C7D36DD95187DC9F51BFC4AF661B614BCE4A5E9EC0729FE24E01'
$Subject = 'CN=renegade-apollo test signing'

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run this from an elevated (Administrator) PowerShell.' }

$win = Join-Path $WindowsDrive 'Windows'
if (-not (Test-Path (Join-Path $win 'System32\config\SYSTEM'))) { throw "$WindowsDrive does not look like the phone's Windows partition." }

$targets = @(Get-ChildItem -Path (Join-Path $win 'System32\DriverStore\FileRepository') -Directory -Filter 'qcpep*' |
    ForEach-Object { Join-Path $_.FullName 'qcpep8250.sys' } | Where-Object { Test-Path $_ })
$drv = Join-Path $win 'System32\drivers\qcpep8250.sys'
if (Test-Path $drv) { $targets += $drv }
if ($targets.Count -eq 0) { throw "No qcpep8250.sys under $win. Are the Kona drivers installed?" }

function Unlock-File($path) {
    takeown /f $path /a | Out-Null
    icacls $path /grant '*S-1-5-32-544:F' | Out-Null      # Administrators
}

if ($Restore) {
    foreach ($t in $targets) {
        if (Test-Path "$t.orig") { Unlock-File $t; Copy-Item "$t.orig" $t -Force; Write-Host "restored $t" }
    }
    return
}

if (-not $Sys -or -not (Test-Path $Sys)) { throw 'Give -Sys <qcpep8250-lmhfix.sys from patch-qcpep-lmh.py>.' }
$work = Join-Path $env:TEMP 'renegade-apollo-qcpep'
New-Item -ItemType Directory -Force -Path $work | Out-Null
$signed = Join-Path $work 'qcpep8250.sys'
Copy-Item $Sys $signed -Force

# 1. Test-sign. Windows in test-signing mode loads kernel drivers signed with any certificate.
$cert = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert | Where-Object Subject -eq $Subject | Select-Object -First 1
if (-not $cert) {
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $Subject -CertStoreLocation Cert:\CurrentUser\My `
        -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(5)
    Write-Host "created signing certificate $($cert.Thumbprint)"
}
Set-AuthenticodeSignature -FilePath $signed -Certificate $cert -HashAlgorithm SHA256 | Out-Null
$sig = Get-AuthenticodeSignature -FilePath $signed
if (-not $sig.SignerCertificate -or $sig.SignerCertificate.Thumbprint -ne $cert.Thumbprint) { throw 'Signing failed.' }
Write-Host "signed (status '$($sig.Status)'; 'UnknownError'/'NotTrusted' is expected for a test certificate)"

# 2. Replace every original copy, keeping a backup.
foreach ($t in $targets) {
    $hash = (Get-FileHash $t -Algorithm SHA256).Hash
    if (-not (Test-Path "$t.orig")) {
        if ($hash -ne $OriginalSha256) { Write-Warning "skipping $t : not the 1.0.2140.0 driver ($hash)"; continue }
        Copy-Item $t "$t.orig"
    }
    Unlock-File $t
    Copy-Item $signed $t -Force
    Write-Host "installed $t"
}

# 3. BCD: keep test signing on; optionally turn the kernel debugger off.
if ($EspDrive) {
    $bcd = Join-Path $EspDrive 'EFI\Microsoft\Boot\BCD'
    if (-not (Test-Path $bcd)) { throw "No BCD at $bcd" }
    bcdedit /store $bcd /set '{default}' testsigning on | Out-Null
    if ($DebugOff) { bcdedit /store $bcd /set '{default}' debug off | Out-Null; Write-Host 'kernel debugger: off' }
    bcdedit /store $bcd /enum '{default}' | Select-String -Pattern 'testsigning|debug|nointegritychecks'
}
Write-Host 'done.'

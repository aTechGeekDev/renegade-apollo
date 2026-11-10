<#
Install the apollo Bluetooth driver (apollobt.sys, QCA6390 over UART) on the phone's running Windows.
Test-signing must be on and the UEFI's DSDT must have \_SB.BTH0 (install-apollo.sh, BT=1 is the default).

  powershell -ExecutionPolicy Bypass -File install-apollobt.ps1 -Firmware <folder with htbtfw20.tlv and htnv20.bin>
  powershell -ExecutionPolicy Bypass -File install-apollobt.ps1 -Status
  powershell -ExecutionPolicy Bypass -File install-apollobt.ps1 -Remove
  optional: -Address 22:22:AA:BB:CC:DD   (the radio's address; without it the NVM file's own is used)
            -Steps N                     (1-4: only diagnostic steps, the radio is switched off again; 5: normal)

Run it from the folder holding apollobt.sys, apollobt.inf and apollobt.cat (the driver-builds files).
The firmware is the phone's own Qualcomm Bluetooth firmware from its `bluetooth` partition (image/htbtfw20.tlv,
the ROM patch, and image/htnv20.bin, the settings), as ios-apollo's tools/pull-bt-fw.sh takes it out; both are
checked and copied to System32\drivers, where the driver loads them at every start.
-Status prints what the driver reported (Status, Chip, Firmware, Address, Traffic) and the Bluetooth devices.
#>
param(
    [string]$Firmware,
    [string]$Address,
    [int]$Steps = 0,
    [switch]$Status,
    [switch]$Remove
)
$ErrorActionPreference = 'Stop'
$Name = 'apollobt'
$Drivers = Join-Path $env:SystemRoot 'System32\drivers'
$Subject = 'CN=renegade-apollo test signing'
$Par = "HKLM:\SYSTEM\CurrentControlSet\Services\$Name\Parameters"

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run this from an elevated (Administrator) PowerShell.' }

function Show-Status {
    $dev = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object InstanceId -like 'ACPI\APLO0002\*'
    if ($dev) { $dev | Format-Table Status, Class, FriendlyName, InstanceId -AutoSize | Out-Host }
    else { Write-Host 'no ACPI\APLO0002 device: the running UEFI has no BTH0 (rebuild it with install-apollo.sh, BT=1)' }
    if (Test-Path $Par) {
        $v = Get-ItemProperty $Par
        foreach ($k in 'Status', 'Chip', 'Firmware', 'Address', 'Traffic') { if ($v.$k) { Write-Host ("{0,-9} {1}" -f $k, $v.$k) } }
    } else { Write-Host 'the driver has not reported anything yet (no Parameters key)' }
    $bt = Get-PnpDevice -Class Bluetooth -ErrorAction SilentlyContinue | Where-Object Present
    if ($bt) { $bt | Format-Table Status, FriendlyName -AutoSize | Out-Host } else { Write-Host 'no Bluetooth radio in Windows yet' }
}

if ($Status) { Show-Status; return }

if ($Remove) {
    $infs = pnputil /enum-drivers | Out-String
    foreach ($m in [regex]::Matches($infs, '(?ms)Published Name:\s+(oem\d+\.inf)\s+Original Name:\s+apollobt\.inf')) {
        pnputil /delete-driver $m.Groups[1].Value /uninstall /force | Out-Host
    }
    if (Test-Path $Par) { Remove-Item $Par -Recurse -Force }
    Write-Host 'removed (the firmware files stay in System32\drivers).'
    return
}

$here = $PSScriptRoot
foreach ($f in "$Name.sys", "$Name.inf", "$Name.cat") {
    if (-not (Test-Path (Join-Path $here $f))) { throw "missing $f next to this script" }
}

# 1. firmware: the patch (TLV type 1, for the QCA6390: product 0x10, ROM 0x200) and the NVM (TLV type 2)
if ($Firmware) {
    $patch = Get-ChildItem $Firmware -Recurse -Filter htbtfw20.tlv | Select-Object -First 1
    $nvm = Get-ChildItem $Firmware -Recurse -Filter htnv20.bin | Select-Object -First 1
    if (-not $patch -or -not $nvm) { throw "no htbtfw20.tlv and htnv20.bin under $Firmware" }
    $p = [IO.File]::ReadAllBytes($patch.FullName); $n = [IO.File]::ReadAllBytes($nvm.FullName)
    $plen = $p[1] + 256 * $p[2] + 65536 * $p[3]; $nlen = $n[1] + 256 * $n[2] + 65536 * $n[3]
    if ($p[0] -ne 1 -or $plen -ne $p.Length - 4 -or ($p[16] + 256 * $p[17]) -ne 0x10 -or ($p[18] + 256 * $p[19]) -ne 0x200) {
        throw "$($patch.FullName) is not the QCA6390 ROM patch (TLV type 1, product 0x10, ROM 0x200)"
    }
    if ($n[0] -ne 2 -or $nlen -ne $n.Length - 4) { throw "$($nvm.FullName) is not a QCA NVM file (TLV type 2)" }
    Copy-Item $patch.FullName, $nvm.FullName $Drivers -Force
    Write-Host "firmware OK: patch $($p.Length) bytes, NVM $($n.Length) bytes -> $Drivers"
} elseif (-not ((Test-Path "$Drivers\htbtfw20.tlv") -and (Test-Path "$Drivers\htnv20.bin"))) {
    throw 'Give -Firmware <folder with htbtfw20.tlv and htnv20.bin> (the phone''s bluetooth partition, image\).'
}

# 2. certificate, trusted by the machine (certutil: Import-Certificate is refused for Root on apollo)
$cert = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert | Where-Object Subject -eq $Subject | Select-Object -First 1
if (-not $cert) {
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $Subject -CertStoreLocation Cert:\CurrentUser\My `
        -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(5)
}
$cer = Join-Path $env:TEMP 'renegade-apollo-test.cer'
Export-Certificate -Cert $cert -FilePath $cer | Out-Null
foreach ($store in 'Root', 'TrustedPublisher') {
    if (-not (Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object Thumbprint -eq $cert.Thumbprint)) {
        certutil -addstore -f $store $cer | Out-Null
        if ($LASTEXITCODE) { throw "certutil -addstore $store failed ($LASTEXITCODE)" }
    }
}

# 3. sign a copy of the package
$pkg = Join-Path $env:TEMP "$Name-pkg"
Remove-Item $pkg -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory $pkg | Out-Null
foreach ($f in "$Name.sys", "$Name.inf", "$Name.cat") { Copy-Item (Join-Path $here $f) $pkg }
foreach ($f in "$Name.sys", "$Name.cat") {
    $fp = Join-Path $pkg $f
    Set-AuthenticodeSignature -FilePath $fp -Certificate $cert -HashAlgorithm SHA256 | Out-Null
    if ((Get-AuthenticodeSignature $fp).SignerCertificate.Thumbprint -ne $cert.Thumbprint) { throw "signing $f failed" }
}

# 4. parameters (fresh diagnostics), then install on \_SB.BTH0
if (Test-Path $Par) { Remove-Item $Par -Recurse -Force }
New-Item -Path $Par -Force | Out-Null
if ($Steps) { Set-ItemProperty -Path $Par -Name Steps -Value $Steps -Type DWord }
if ($Address) {
    $b = $Address -split '[:-]' | ForEach-Object { [Convert]::ToByte($_, 16) }
    if ($b.Count -ne 6 -or ($b[0] -band 1)) { throw "-Address must be 6 bytes like 22:22:AA:BB:CC:DD (first byte even)" }
    Set-ItemProperty -Path $Par -Name Address -Value ([byte[]]$b) -Type Binary
}
pnputil /add-driver (Join-Path $pkg "$Name.inf") /install | Out-Host
if ($LASTEXITCODE -eq 259) {
    Write-Warning 'pnputil kept the installed driver (same or newer version). To force this one: run with -Remove, then install again.'
} elseif ($LASTEXITCODE -and $LASTEXITCODE -ne 3010) { throw "pnputil failed ($LASTEXITCODE)" }
Write-Host 'waiting 15 s for the chip to power up and take its firmware...'
Start-Sleep 15
Show-Status

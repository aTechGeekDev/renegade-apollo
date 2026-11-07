<#
Install the apollo touch screen driver (apollotouch.sys, NT36675) on the phone's running Windows.
Test-signing must be on (it is for apollo) and the UEFI's DSDT must have \_SB.TCH0 (install-apollo.sh TOUCH=1).

  powershell -ExecutionPolicy Bypass -File install-apollotouch.ps1 -Firmware .\novatek_nt36675_j3s_fw01.bin.ihex
  powershell -ExecutionPolicy Bypass -File install-apollotouch.ps1 -Status
  powershell -ExecutionPolicy Bypass -File install-apollotouch.ps1 -Remove

Run it from the folder holding apollotouch.sys, apollotouch.inf and apollotouch.cat (the driver-builds files).
-Firmware takes Xiaomi's .ihex (MiCode Xiaomi_Kernel_OpenSource, branch apollo-q-oss, firmware/) or an already
converted .bin; either way it must come out as the known 139264-byte image (SHA-256 checked) and is copied to
System32\drivers\novatek_nt36675_j3s_fw01.bin, where the driver loads it from at every start.
The driver package is test-signed with the same self-made certificate as apollocpufreq; that certificate is
added to the machine's Root and TrustedPublisher stores so pnputil accepts the package.
-Status prints what the driver reported (Status, TrimId, Firmware, Reports) and the device state.
#>
param(
    [string]$Firmware,
    [switch]$Status,
    [switch]$Remove,
    [string]$ConvertTo        # CI/self-test: convert -Firmware to this .bin path, check it, and stop
)
$ErrorActionPreference = 'Stop'
$Name = 'apollotouch'
$FwName = 'novatek_nt36675_j3s_fw01.bin'
$FwDest = Join-Path $env:SystemRoot "System32\drivers\$FwName"
$FwSize = 139264
$FwSha = '429137D357F25C62BE6652C179041FE2F1A815E4890BBD531EB1506110BBE628'
$Subject = 'CN=renegade-apollo test signing'
$Par = "HKLM:\SYSTEM\CurrentControlSet\Services\$Name\Parameters"

function Read-Firmware([string]$Path) {
    $raw = [IO.File]::ReadAllBytes((Resolve-Path $Path))
    if ($raw.Length -gt 0 -and $raw[0] -ne [byte][char]':') { return , $raw }
    # Intel HEX -> flat image (data, extended segment and extended linear address records)
    $mem = New-Object byte[] (1MB); $max = 0; $base = 0; $eof = $false
    foreach ($line in [IO.File]::ReadAllLines((Resolve-Path $Path))) {
        $l = $line.Trim()
        if (-not $l) { continue }
        if ($eof) { break }
        if ($l[0] -ne ':' -or ($l.Length % 2) -ne 1) { throw "not an Intel HEX line: $l" }
        $b = New-Object byte[] ([int](($l.Length - 1) / 2))
        $sum = 0
        for ($i = 0; $i -lt $b.Length; $i++) { $b[$i] = [Convert]::ToByte($l.Substring(1 + 2 * $i, 2), 16); $sum += $b[$i] }
        if (($sum -band 0xff) -ne 0) { throw "HEX checksum error: $l" }
        $n = [int]$b[0]; $addr = ([int]$b[1] -shl 8) -bor $b[2]
        switch ([int]$b[3]) {
            0 { $a = $base + $addr; [Array]::Copy($b, 4, $mem, $a, $n); if ($a + $n -gt $max) { $max = $a + $n } }
            1 { $eof = $true }
            2 { $base = ((([int]$b[4] -shl 8) -bor $b[5]) * 16) }
            4 { $base = ((([int]$b[4] -shl 8) -bor $b[5]) * 65536) }
        }
    }
    $out = New-Object byte[] $max
    [Array]::Copy($mem, $out, $max)
    return , $out
}

function Test-Firmware([byte[]]$Bytes) {
    $h = ([BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($Bytes))) -replace '-', ''
    if ($Bytes.Length -ne $FwSize -or $h -ne $FwSha) {
        throw "firmware is not the expected image ($($Bytes.Length) bytes, SHA-256 $h; want $FwSize bytes, $FwSha)"
    }
    Write-Host "firmware OK: $($Bytes.Length) bytes, SHA-256 matches"
}

if ($ConvertTo) {
    $fw = Read-Firmware $Firmware
    Test-Firmware $fw
    [IO.File]::WriteAllBytes($ConvertTo, $fw)
    return
}

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run this from an elevated (Administrator) PowerShell.' }

function Show-Status {
    $dev = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object InstanceId -like 'ACPI\APLO0001\*'
    if ($dev) { $dev | Format-Table Status, Class, FriendlyName, InstanceId -AutoSize | Out-Host }
    else { Write-Host 'no ACPI\APLO0001 device: the running UEFI has no TCH0 (rebuild it with install-apollo.sh, TOUCH=1)' }
    if ($dev -and $dev.Status -ne 'OK') {
        $p = Get-PnpDeviceProperty -InstanceId $dev.InstanceId -KeyName DEVPKEY_Device_ProblemCode -ErrorAction SilentlyContinue
        if ($p) { Write-Host "problem code: $($p.Data)" }
    }
    if (Test-Path $Par) {
        $v = Get-ItemProperty $Par
        foreach ($k in 'Status', 'TrimId', 'Firmware', 'Reports') { Write-Host ("{0,-9} {1}" -f $k, $v.$k) }
    } else { Write-Host 'the driver has not reported anything yet (no Parameters key)' }
    $hid = Get-PnpDevice -Class HIDClass -ErrorAction SilentlyContinue | Where-Object InstanceId -like 'HID\*3675*'
    if ($hid) { $hid | Format-Table Status, FriendlyName -AutoSize | Out-Host }
}

if ($Status) { Show-Status; return }

if ($Remove) {
    $infs = pnputil /enum-drivers | Out-String
    foreach ($m in [regex]::Matches($infs, '(?ms)Published Name:\s+(oem\d+\.inf)\s+Original Name:\s+apollotouch\.inf')) {
        pnputil /delete-driver $m.Groups[1].Value /uninstall /force | Out-Host
    }
    if (Test-Path $FwDest) { Remove-Item $FwDest -Force }
    if (Test-Path $Par) { Remove-Item $Par -Recurse -Force }
    Write-Host 'removed.'
    return
}

$here = $PSScriptRoot
foreach ($f in "$Name.sys", "$Name.inf", "$Name.cat") {
    if (-not (Test-Path (Join-Path $here $f))) { throw "missing $f next to this script" }
}
if (-not $Firmware) { throw "Give -Firmware <$FwName.ihex or .bin> (Xiaomi apollo-q-oss firmware/)." }
if ((bcdedit /enum '{current}' | Select-String -Pattern 'testsigning\s+Yes') -eq $null) {
    Write-Warning 'testsigning does not look enabled for the running Windows; the driver may be refused.'
}

# 1. firmware
$fw = Read-Firmware $Firmware
Test-Firmware $fw
[IO.File]::WriteAllBytes($FwDest, $fw)
Write-Host "firmware -> $FwDest"

# 2. certificate, trusted by the machine
$cert = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert | Where-Object Subject -eq $Subject | Select-Object -First 1
if (-not $cert) {
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $Subject -CertStoreLocation Cert:\CurrentUser\My `
        -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(5)
}
$cer = Join-Path $env:TEMP 'renegade-apollo-test.cer'
Export-Certificate -Cert $cert -FilePath $cer | Out-Null
foreach ($store in 'Root', 'TrustedPublisher') {
    if (-not (Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object Thumbprint -eq $cert.Thumbprint)) {
        # certutil, not Import-Certificate: the latter is refused (E_ACCESSDENIED) for LocalMachine\Root on apollo
        certutil -addstore -f $store $cer | Out-Null
        if ($LASTEXITCODE) { throw "certutil -addstore $store failed ($LASTEXITCODE)" }
    }
}

# 3. sign a copy of the package (the catalog lists the driver's image hash, which the embedded signature does not change)
$pkg = Join-Path $env:TEMP "$Name-pkg"
Remove-Item $pkg -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory $pkg | Out-Null
foreach ($f in "$Name.sys", "$Name.inf", "$Name.cat") { Copy-Item (Join-Path $here $f) $pkg }
foreach ($f in "$Name.sys", "$Name.cat") {
    $p = Join-Path $pkg $f
    Set-AuthenticodeSignature -FilePath $p -Certificate $cert -HashAlgorithm SHA256 | Out-Null
    if ((Get-AuthenticodeSignature $p).SignerCertificate.Thumbprint -ne $cert.Thumbprint) { throw "signing $f failed" }
}

# 4. install on \_SB.TCH0
if (Test-Path $Par) { Remove-Item $Par -Recurse -Force }    # fresh diagnostics
pnputil /add-driver (Join-Path $pkg "$Name.inf") /install | Out-Host
if ($LASTEXITCODE -and $LASTEXITCODE -ne 3010) { throw "pnputil failed ($LASTEXITCODE)" }
Write-Host 'waiting 8 s for the touch firmware download...'
Start-Sleep 8
Show-Status

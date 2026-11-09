<#
Install and run apollogpu.sys (Adreno 650 bring-up, milestone 1) on the phone's running Windows.
Test-signing must be on (it is for apollo). Run from an Administrator PowerShell in the folder that holds
apollogpu.sys (the driver-builds files).

  powershell -ExecutionPolicy Bypass -File install-apollogpu.ps1 -Firmware <folder> -Run
  powershell -ExecutionPolicy Bypass -File install-apollogpu.ps1 -Run          (again, firmware already installed)
  powershell -ExecutionPolicy Bypass -File install-apollogpu.ps1 -Status       (what the last run reported)
  powershell -ExecutionPolicy Bypass -File install-apollogpu.ps1 -Remove
  optional with -Run: -Steps N (1..22, default 22), -Level N (GPU speed 1 = 305 MHz .. 6 = 587 MHz, default 1),
                      -Zap N (0..3, TrustZone ladder, default 3)

<folder> holds the phone's own GPU firmware, as ios-apollo's tools/pull-gpu-fw.sh takes it from /vendor/firmware
(research/apollo-gpu/): a650_gmu.bin, a650_sqe.fw, a650_zap.mdt, a650_zap.b02. They are checked and copied to
System32\drivers\apollogpu\.

The driver is a demand-start service: it runs only when -Run starts it, never at boot. It powers the GPU, starts its
GMU and command processor, gets it out of secure mode through TrustZone, has it write a test word and draw three bands
(orange, white, blue) that are copied to the screen, then powers it all down again. Every line goes to
HKLM\SYSTEM\CurrentControlSet\Services\apollogpu\Parameters\Log; the log is saved to disk before each step, so if the
phone freezes, reboot and run -Status: the last lines show where it stopped.
#>
param(
    [string]$Firmware,
    [string]$Sys,
    [switch]$Run,
    [int]$Steps = 22,
    [int]$Level = 1,
    [int]$Zap = 3,
    [switch]$Status,
    [switch]$Remove
)
$ErrorActionPreference = 'Stop'
$Name = 'apollogpu'
$Drivers = Join-Path $env:SystemRoot 'System32\drivers'
$Dest = Join-Path $Drivers "$Name.sys"
$FwDir = Join-Path $Drivers $Name
$Subject = 'CN=renegade-apollo test signing'
$Par = "HKLM:\SYSTEM\CurrentControlSet\Services\$Name\Parameters"

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run this from an elevated (Administrator) PowerShell.' }

function Show-Result([switch]$Full) {
    if (-not (Test-Path $Par)) { Write-Host 'apollogpu has not run yet (no Parameters key).'; return }
    $v = Get-ItemProperty $Par
    $log = if ($v.Log) { $v.Log -split "`n" | Where-Object { $_ -ne '' } } else { @() }
    if ($Full -or $log.Count -le 60) { $log | ForEach-Object { Write-Host $_ } }
    else { Write-Host "... ($($log.Count) lines; the whole log: $env:TEMP\apollogpu-log.txt)"; $log | Select-Object -Last 60 | ForEach-Object { Write-Host $_ } }
    if ($log.Count) { $log | Set-Content (Join-Path $env:TEMP 'apollogpu-log.txt') -Encoding UTF8 }
    Write-Host ''
    Write-Host "Last line: $($v.Status)"
    $text = $log -join "`n"
    if ($text -match 'A PICTURE DRAWN BY THE GPU IS ON THE PANEL') {
        Write-Host 'RESULT: the GPU works on Windows (milestone 1 passed: it drew the bands on the screen).' -ForegroundColor Green
    } elseif ($text -match 'SWEET GPU: done') {
        Write-Host 'RESULT: stopped before the end; the lines above say at which step and why.' -ForegroundColor Yellow
    } else {
        Write-Host 'RESULT: no "done" line: the run is still going, or the phone froze during the step shown last.' -ForegroundColor Yellow
    }
}

if ($Status) { Show-Result; return }

if ($Remove) {
    sc.exe stop $Name 2>$null | Out-Null
    sc.exe delete $Name 2>$null | Out-Null
    if (Test-Path $Dest) { Remove-Item $Dest -Force }
    if (Test-Path $FwDir) { Remove-Item $FwDir -Recurse -Force }
    Write-Host 'removed.'
    return
}

# 1. firmware (checked like the driver checks it)
if ($Firmware) {
    $need = @{ 'a650_gmu.bin' = @(64, 0x200000); 'a650_sqe.fw' = @(8, 0x40000); 'a650_zap.mdt' = @(116, 0x10000); 'a650_zap.b02' = @(1, 4096) }
    foreach ($f in $need.Keys) {
        $p = Join-Path $Firmware $f
        if (-not (Test-Path $p)) { throw "missing $p (ios-apollo tools/pull-gpu-fw.sh puts it in research/apollo-gpu/)" }
        $len = (Get-Item $p).Length
        if ($len -lt $need[$f][0] -or $len -gt $need[$f][1]) { throw "$f is $len bytes: not the expected file" }
    }
    $mdt = [IO.File]::ReadAllBytes((Join-Path $Firmware 'a650_zap.mdt'))
    if ($mdt[0] -ne 0x7f -or $mdt[1] -ne 0x45 -or $mdt[2] -ne 0x4c -or $mdt[3] -ne 0x46 -or $mdt[4] -ne 1) { throw 'a650_zap.mdt is not a 32-bit ELF' }
    $segments = [BitConverter]::ToUInt16($mdt, 44)
    $hashes = [BitConverter]::ToUInt32($mdt, 100)
    if ($segments -lt 2 -or (52 + 32 * $segments + $hashes) -ne $mdt.Length) {
        throw "a650_zap.mdt is not the ELF header + hashes alone ($segments segments, hashes $hashes, file $($mdt.Length)): TrustZone would refuse it"
    }
    New-Item -ItemType Directory -Force -Path $FwDir | Out-Null
    foreach ($f in $need.Keys) { Copy-Item (Join-Path $Firmware $f) (Join-Path $FwDir $f) -Force }
    Write-Host "firmware checked and copied to $FwDir"
}

# 2. the driver (test-signed), as a demand-start kernel service
if (-not $Sys) { $Sys = Join-Path $PSScriptRoot "$Name.sys" }
if (Test-Path $Sys) {
    if ((bcdedit /enum '{current}' | Select-String -Pattern 'testsigning\s+Yes') -eq $null) {
        Write-Warning 'testsigning does not look enabled for the running Windows; the driver may be refused.'
    }
    $cert = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert | Where-Object Subject -eq $Subject | Select-Object -First 1
    if (-not $cert) {
        $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $Subject -CertStoreLocation Cert:\CurrentUser\My `
            -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(5)
    }
    $work = Join-Path $env:TEMP 'renegade-apollo-gpu'
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    $tmp = Join-Path $work "$Name.sys"
    Copy-Item $Sys $tmp -Force
    Set-AuthenticodeSignature -FilePath $tmp -Certificate $cert -HashAlgorithm SHA256 | Out-Null
    if ((Get-AuthenticodeSignature $tmp).SignerCertificate.Thumbprint -ne $cert.Thumbprint) { throw 'Signing failed.' }
    sc.exe stop $Name 2>$null | Out-Null
    Copy-Item $tmp $Dest -Force
    if (-not (Get-Service $Name -ErrorAction SilentlyContinue)) {
        sc.exe create $Name type= kernel start= demand error= normal binPath= "System32\drivers\$Name.sys" DisplayName= 'apollo Adreno 650 bring-up' | Out-Host
        if ($LASTEXITCODE) { throw 'sc create failed' }
    }
    Write-Host "driver installed: $Dest (demand start: it runs only with -Run)"
} elseif (-not (Test-Path $Dest)) {
    throw "no $Name.sys next to this script (or give -Sys)"
}

# 3. run it
if ($Run) {
    foreach ($f in 'a650_gmu.bin', 'a650_sqe.fw', 'a650_zap.mdt', 'a650_zap.b02') {
        if (-not (Test-Path (Join-Path $FwDir $f))) { throw "no $f in ${FwDir}: install the firmware with -Firmware <folder>" }
    }
    if ($Steps -lt 1 -or $Steps -gt 22) { throw '-Steps is 1..22' }
    if ($Level -lt 1 -or $Level -gt 6) { throw '-Level is 1..6' }
    if ($Zap -lt 0 -or $Zap -gt 3) { throw '-Zap is 0..3' }
    New-Item -Path $Par -Force | Out-Null
    Set-ItemProperty -Path $Par -Name Steps -Value $Steps -Type DWord
    Set-ItemProperty -Path $Par -Name Level -Value $Level -Type DWord
    Set-ItemProperty -Path $Par -Name Zap -Value $Zap -Type DWord
    Remove-ItemProperty -Path $Par -Name Log, Status -ErrorAction SilentlyContinue
    sc.exe stop $Name 2>$null | Out-Null
    Start-Sleep -Seconds 1
    Write-Host "starting the bring-up (steps 1..$Steps, level $Level, zap $Zap); it takes up to about a minute ..."
    sc.exe start $Name | Out-Null
    if ($LASTEXITCODE) { throw "sc start failed ($LASTEXITCODE): the driver was not loaded (signature? see Event Viewer > System)" }
    for ($i = 0; $i -lt 180; $i++) {
        Start-Sleep -Seconds 1
        $log = (Get-ItemProperty $Par -ErrorAction SilentlyContinue).Log
        if ($log -match 'SWEET GPU: done') { break }
    }
    Show-Result
}

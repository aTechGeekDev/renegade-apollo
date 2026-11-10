<#
Install and run apollowifi.sys (PCIe0 + QCA6390 Wi-Fi bring-up, stage 1) on the phone's running Windows.
Test-signing must be on (it is for apollo). Run from an Administrator PowerShell in the folder that holds
apollowifi.sys (the driver-builds files). No firmware and no UEFI rebuild are needed for this stage.

  powershell -ExecutionPolicy Bypass -File install-apollowifi.ps1 -Run              (steps 1..2: power, link, IDs)
  powershell -ExecutionPolicy Bypass -File install-apollowifi.ps1 -Run -Steps 4     (also ECAM and the SMMU context)
  powershell -ExecutionPolicy Bypass -File install-apollowifi.ps1 -Status           (what the last run reported)
  powershell -ExecutionPolicy Bypass -File install-apollowifi.ps1 -Remove
  optional with -Run: -Steps N (1..4, default 2), -Deep (step 3 also reads bus 1 device 1), -Keep (leave the link up)

What the steps do (each is the next one plus everything before it):
  1  reads the PCIe clock and pin registers only
  2  powers the Wi-Fi chip, brings the PCIe link up and reads the chip's IDs (proven on this phone by ios-apollo)
  3  lays the PCI configuration space out the way Windows' own PCI driver expects it (ECAM) and reads it back
  4  gives the chip an IOMMU (SMMU) context in which memory addresses are the real ones, as Windows drivers use them
Afterwards the chip is put back in reset. The driver is a demand-start service: it runs only when -Run starts it.
Every line goes to HKLM\SYSTEM\CurrentControlSet\Services\apollowifi\Parameters\Log; the log is saved to disk before
each step and each risky read, so if the phone freezes or restarts, boot again and run -Status: the last lines
show where it stopped.
#>
param(
    [string]$Sys,
    [switch]$Run,
    [int]$Steps = 2,
    [switch]$Deep,
    [switch]$Keep,
    [switch]$Status,
    [switch]$Remove
)
$ErrorActionPreference = 'Stop'
$Name = 'apollowifi'
$Dest = Join-Path $env:SystemRoot "System32\drivers\$Name.sys"
$Subject = 'CN=renegade-apollo test signing'
$Par = "HKLM:\SYSTEM\CurrentControlSet\Services\$Name\Parameters"

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run this from an elevated (Administrator) PowerShell.' }

function Show-Result {
    if (-not (Test-Path $Par)) { Write-Host 'apollowifi has not run yet (no Parameters key).'; return }
    $v = Get-ItemProperty $Par
    $log = if ($v.Log) { $v.Log -split "`n" | Where-Object { $_ -ne '' } } else { @() }
    $log | ForEach-Object { Write-Host $_ }
    if ($log.Count) {
        $file = Join-Path $env:TEMP 'apollowifi-log.txt'
        $log | Set-Content $file -Encoding UTF8
        Write-Host ''
        Write-Host "(the whole log is also in $file)"
    }
    Write-Host "Last line: $($v.Status)"
    $text = $log -join "`n"
    if ($text -notmatch 'SWEET WIFI: done') {
        Write-Host 'RESULT: no "done" line: the run is still going, or the phone froze/restarted during the line shown last.' -ForegroundColor Yellow
        return
    }
    if ($text -match 'THE PCIe LINK IS UP AND THE QCA6390 ANSWERS') {
        Write-Host 'RESULT step 2: the Wi-Fi chip is powered, the PCIe link is up and the chip answers.' -ForegroundColor Green
        if ($text -match 'SUBSYS_([0-9A-F]{8})') { Write-Host "         hardware ID: PCI\VEN_17CB&DEV_1101&SUBSYS_$($Matches[1])" -ForegroundColor Green }
    } elseif ($text -match 'step 2') {
        Write-Host 'RESULT step 2: FAILED (no link to the Wi-Fi chip); the lines above say where.' -ForegroundColor Yellow
    }
    if ($text -match 'ECAM IS READY FOR pci.sys') { Write-Host 'RESULT step 3: the PCI configuration space is laid out as Windows expects.' -ForegroundColor Green }
    elseif ($text -match 'step 3') { Write-Host 'RESULT step 3: ECAM is NOT ready; the lines above say why.' -ForegroundColor Yellow }
    if ($text -match 'THE RADIO MAY USE PHYSICAL ADDRESSES') { Write-Host 'RESULT step 4: the IOMMU (SMMU) context for the chip was accepted.' -ForegroundColor Green }
    elseif ($text -match 'step 4') { Write-Host 'RESULT step 4: the SMMU context was NOT accepted; the lines above say why.' -ForegroundColor Yellow }
}

if ($Status) { Show-Result; return }

if ($Remove) {
    sc.exe stop $Name 2>$null | Out-Null
    sc.exe delete $Name 2>$null | Out-Null
    if (Test-Path $Dest) { Remove-Item $Dest -Force }
    Write-Host 'removed.'
    return
}

# 1. the driver (test-signed), as a demand-start kernel service
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
    $work = Join-Path $env:TEMP 'renegade-apollo-wifi'
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    $tmp = Join-Path $work "$Name.sys"
    Copy-Item $Sys $tmp -Force
    Set-AuthenticodeSignature -FilePath $tmp -Certificate $cert -HashAlgorithm SHA256 | Out-Null
    if ((Get-AuthenticodeSignature $tmp).SignerCertificate.Thumbprint -ne $cert.Thumbprint) { throw 'Signing failed.' }
    sc.exe stop $Name 2>$null | Out-Null
    Start-Sleep -Seconds 1
    Copy-Item $tmp $Dest -Force
    if (-not (Get-Service $Name -ErrorAction SilentlyContinue)) {
        sc.exe create $Name type= kernel start= demand error= normal binPath= "System32\drivers\$Name.sys" DisplayName= 'apollo PCIe / QCA6390 Wi-Fi bring-up' | Out-Host
        if ($LASTEXITCODE) { throw 'sc create failed' }
    }
    Write-Host "driver installed: $Dest (demand start: it runs only with -Run)"
} elseif (-not (Test-Path $Dest)) {
    throw "no $Name.sys next to this script (or give -Sys)"
}

# 2. run it
if ($Run) {
    if ($Steps -lt 1 -or $Steps -gt 4) { throw '-Steps is 1..4' }
    New-Item -Path $Par -Force | Out-Null
    Set-ItemProperty -Path $Par -Name Steps -Value $Steps -Type DWord
    Set-ItemProperty -Path $Par -Name Deep -Value ([int][bool]$Deep) -Type DWord
    Set-ItemProperty -Path $Par -Name Keep -Value ([int][bool]$Keep) -Type DWord
    Remove-ItemProperty -Path $Par -Name Log, Status -ErrorAction SilentlyContinue
    sc.exe stop $Name 2>$null | Out-Null
    Start-Sleep -Seconds 1
    Write-Host "starting the bring-up (steps 1..$Steps); it takes a few seconds ..."
    sc.exe start $Name | Out-Null
    if ($LASTEXITCODE) { throw "sc start failed ($LASTEXITCODE): the driver was not loaded (signature? see Event Viewer > System; after -Keep a restart is needed)" }
    for ($i = 0; $i -lt 60; $i++) {
        Start-Sleep -Seconds 1
        $log = (Get-ItemProperty $Par -ErrorAction SilentlyContinue).Log
        if ($log -match 'SWEET WIFI: done') { break }
    }
    Show-Result
}

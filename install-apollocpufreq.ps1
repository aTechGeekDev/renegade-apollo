<#
Install apollocpufreq.sys on the phone's running Windows (test-signing must be on, as it is for apollo).

  powershell -ExecutionPolicy Bypass -File install-apollocpufreq.ps1 -Sys .\apollocpufreq.sys [-MaxMHz 2400]
  powershell -ExecutionPolicy Bypass -File install-apollocpufreq.ps1 -Remove

Test-signs the driver with a self-made certificate, copies it to System32\drivers, registers it as a
system-start kernel service and starts it. -MaxMHz caps the chosen frequency (0 = top of each table).
The result is logged by the driver (DbgPrint) and visible as the CPU speed in Task Manager / benchmarks.
-Remove stops and deletes the service and the file; rebooting then returns the CPUs to the firmware's level.
#>
param(
    [string]$Sys,
    [int]$MaxMHz = 0,
    [switch]$Remove
)
$ErrorActionPreference = 'Stop'
$Name = 'apollocpufreq'
$Dest = Join-Path $env:SystemRoot "System32\drivers\$Name.sys"
$Subject = 'CN=renegade-apollo test signing'

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run this from an elevated (Administrator) PowerShell.' }

if ($Remove) {
    sc.exe stop $Name | Out-Null
    sc.exe delete $Name | Out-Null
    if (Test-Path $Dest) { Remove-Item $Dest -Force }
    Write-Host 'removed; reboot to return the CPUs to the firmware level.'
    return
}

if (-not $Sys -or -not (Test-Path $Sys)) { throw 'Give -Sys <apollocpufreq.sys>.' }
if ((bcdedit /enum '{current}' | Select-String -Pattern 'testsigning\s+Yes') -eq $null) {
    Write-Warning 'testsigning does not look enabled for the running Windows; the driver may be refused.'
}

$cert = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert | Where-Object Subject -eq $Subject | Select-Object -First 1
if (-not $cert) {
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $Subject -CertStoreLocation Cert:\CurrentUser\My `
        -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(5)
}
$tmp = Join-Path $env:TEMP "$Name.sys"
Copy-Item $Sys $tmp -Force
Set-AuthenticodeSignature -FilePath $tmp -Certificate $cert -HashAlgorithm SHA256 | Out-Null
if ((Get-AuthenticodeSignature $tmp).SignerCertificate.Thumbprint -ne $cert.Thumbprint) { throw 'Signing failed.' }

sc.exe stop $Name 2>$null | Out-Null
Copy-Item $tmp $Dest -Force
if (-not (Get-Service $Name -ErrorAction SilentlyContinue)) {
    sc.exe create $Name type= kernel start= system error= normal binPath= "System32\drivers\$Name.sys" DisplayName= 'apollo CPU frequency (EPSS)' | Out-Host
    if ($LASTEXITCODE) { throw 'sc create failed' }
}
$par = "HKLM:\SYSTEM\CurrentControlSet\Services\$Name\Parameters"
New-Item -Path $par -Force | Out-Null
Set-ItemProperty -Path $par -Name MaxMHz -Value $MaxMHz -Type DWord
sc.exe start $Name | Out-Host
Write-Host "installed (cap: $MaxMHz MHz, 0 = top). It runs again at every boot."

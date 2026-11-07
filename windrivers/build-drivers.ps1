# Build the ARM64 kernel drivers in windrivers\ with MSVC + the WDK NuGet packages (used by CI).
# Output: windrivers\out\<name>.sys
$ErrorActionPreference = 'Stop'
$Root = $PSScriptRoot
$Pkg = Join-Path $Root 'packages'
$Out = Join-Path $Root 'out'
New-Item -ItemType Directory -Force -Path $Out, (Join-Path $Root 'obj') | Out-Null

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = (Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name -Descending | Select-Object -First 1).FullName
$cl = "$msvc\bin\Hostx64\arm64\cl.exe"; $link = "$msvc\bin\Hostx64\arm64\link.exe"
if (-not (Test-Path $cl)) { throw "no ARM64 cross compiler under $msvc" }

$wdk = (Get-ChildItem "$Pkg\Microsoft.Windows.WDK.ARM64*" -Directory | Select-Object -First 1).FullName
$ntddk = Get-ChildItem $wdk -Recurse -Filter ntddk.h | Select-Object -First 1
$ntos = Get-ChildItem $wdk -Recurse -Filter ntoskrnl.lib | Where-Object FullName -match 'arm64' | Select-Object -First 1
$spec = Get-ChildItem $Pkg -Recurse -Filter specstrings.h | Select-Object -First 1
if (-not $ntddk -or -not $ntos -or -not $spec) { throw 'WDK/SDK headers or libs not found' }
$wdkInc = $ntddk.Directory.Parent.FullName; $sdkInc = $spec.Directory.Parent.FullName
Write-Host "MSVC $msvc`nWDK inc $wdkInc`nWDK lib $($ntos.DirectoryName)"

$inc = @("$wdkInc\km", "$wdkInc\km\crt", "$wdkInc\shared", "$sdkInc\shared", "$msvc\include") | ForEach-Object { "/I`"$_`"" }
$defs = '/D_ARM64_', '/DARM64', '/D_WIN64', '/D_WIN32_WINNT=0x0A00', '/DWINVER=0x0A00', '/DNTDDI_VERSION=0x0A00000C'

foreach ($name in 'apollocpufreq') {
    $obj = Join-Path $Root "obj\$name.obj"
    @('/nologo', '/c', '/kernel', '/GS-', '/W4', '/WX', '/O2', '/Zi') + $defs + $inc + "/Fo`"$obj`"" + "`"$(Join-Path $Root "$name\$name.c")`"" |
        Set-Content (Join-Path $Root "obj\$name.cl.rsp") -Encoding ASCII
    & $cl "@$(Join-Path $Root "obj\$name.cl.rsp")"; if ($LASTEXITCODE) { throw "cl $name" }
    @('/nologo', "/OUT:`"$Out\$name.sys`"", '/MACHINE:ARM64', '/SUBSYSTEM:NATIVE,10.00', '/DRIVER', '/ENTRY:DriverEntry',
      '/NODEFAULTLIB', '/RELEASE', '/DEBUG', "/PDB:`"$Out\$name.pdb`"", "/LIBPATH:`"$($ntos.DirectoryName)`"", 'ntoskrnl.lib', 'BufferOverflowFastFailK.lib', "`"$obj`"") |
        Set-Content (Join-Path $Root "obj\$name.link.rsp") -Encoding ASCII
    & $link "@$(Join-Path $Root "obj\$name.link.rsp")"; if ($LASTEXITCODE) { throw "link $name" }
    Get-Item "$Out\$name.sys" | Select-Object Name, Length
}

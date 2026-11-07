# Build the ARM64 kernel drivers in windrivers\ with MSVC + the WDK NuGet packages (used by CI).
# Output: windrivers\out\<name>.sys (+ .inf/.cat for PnP drivers)
#   apollocpufreq - legacy (non-PnP) WDM driver
#   apollotouch   - KMDF 1.15 PnP driver + Virtual HID Framework, with an INF and an Inf2Cat catalog
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
$vhfh = Get-ChildItem $Pkg -Recurse -Filter vhf.h | Where-Object FullName -match 'shared' | Select-Object -First 1
if (-not $ntddk -or -not $ntos -or -not $spec -or -not $vhfh) { throw 'WDK/SDK headers or libs not found' }
$wdkInc = $ntddk.Directory.Parent.FullName; $sdkInc = $spec.Directory.Parent.FullName
$kmLib = $ntos.DirectoryName
$kmdfVer = '1.15'
$kmdfInc = Get-ChildItem $wdk -Recurse -Directory | Where-Object { $_.FullName -match "\\wdf\\kmdf\\$([regex]::Escape($kmdfVer))$" -and $_.FullName -match '\\Include\\' } | Select-Object -First 1
$kmdfLib = Get-ChildItem $wdk -Recurse -Filter wdfdriverentry.lib | Where-Object FullName -match "\\ARM64\\$([regex]::Escape($kmdfVer))\\" | Select-Object -First 1
if (-not $kmdfInc -or -not $kmdfLib) { throw "KMDF $kmdfVer headers or libs not found" }
Write-Host "MSVC $msvc`nWDK inc $wdkInc`nWDK lib $kmLib`nKMDF $($kmdfInc.FullName) / $($kmdfLib.DirectoryName)`nvhf.h $($vhfh.FullName)"

$inc = @("$wdkInc\km", "$wdkInc\km\crt", "$wdkInc\shared", "$sdkInc\shared", "$msvc\include")
$defs = '/D_ARM64_', '/DARM64', '/D_WIN64', '/D_WIN32_WINNT=0x0A00', '/DWINVER=0x0A00', '/DNTDDI_VERSION=0x0A00000C'

$drivers = @(
    @{ Name = 'apollocpufreq'; Entry = 'DriverEntry'; Inc = @(); Defs = @(); Libs = @() },
    @{ Name = 'apollotouch'; Entry = 'FxDriverEntry'; Inc = @($kmdfInc.FullName)
       Defs = @('/wd4324', '/DKMDF_VERSION_MAJOR=1', "/DKMDF_VERSION_MINOR=$($kmdfVer.Split('.')[1])")
       Libs = @("/LIBPATH:`"$($kmdfLib.DirectoryName)`"", 'WdfDriverEntry.lib', 'WdfLdr.lib', 'vhfkm.lib', 'hal.lib', 'libcntpr.lib') }   # libcntpr: vhfkm.lib's _Interlocked* / __security_push_cookie
)

foreach ($d in $drivers) {
    $name = $d.Name
    $obj = Join-Path $Root "obj\$name.obj"
    @('/nologo', '/c', '/kernel', '/GS-', '/W4', '/WX', '/O2', '/Zi') + $defs + $d.Defs +
        (($d.Inc + $inc) | ForEach-Object { "/I`"$_`"" }) + "/Fo`"$obj`"" + "/Fd`"$Root\obj\$name.pdb`"" +
        "`"$(Join-Path $Root "$name\$name.c")`"" |
        Set-Content (Join-Path $Root "obj\$name.cl.rsp") -Encoding ASCII
    & $cl "@$(Join-Path $Root "obj\$name.cl.rsp")"; if ($LASTEXITCODE) { throw "cl $name" }
    @('/nologo', "/OUT:`"$Out\$name.sys`"", '/MACHINE:ARM64', '/SUBSYSTEM:NATIVE,10.00', '/DRIVER', "/ENTRY:$($d.Entry)",
      '/NODEFAULTLIB', '/RELEASE', '/DEBUG', "/PDB:`"$Out\$name.pdb`"", "/LIBPATH:`"$kmLib`"") + $d.Libs +
        @('ntoskrnl.lib', 'BufferOverflowFastFailK.lib', "`"$obj`"") |
        Set-Content (Join-Path $Root "obj\$name.link.rsp") -Encoding ASCII
    & $link "@$(Join-Path $Root "obj\$name.link.rsp")"; if ($LASTEXITCODE) { throw "link $name" }
    Get-Item "$Out\$name.sys" | Select-Object Name, Length | Out-Host

    $inf = Join-Path $Root "$name\$name.inf"
    if (Test-Path $inf) {
        # catalog: Inf2Cat over a folder holding just the package
        $pk = Join-Path $Root "obj\pkg-$name"
        Remove-Item $pk -Recurse -Force -ErrorAction SilentlyContinue
        New-Item -ItemType Directory $pk | Out-Null
        Copy-Item $inf, "$Out\$name.sys" $pk
        $inf2cat = Get-ChildItem $wdk -Recurse -Filter Inf2Cat.exe | Select-Object -First 1
        if (-not $inf2cat) { throw 'Inf2Cat.exe not found in the WDK package' }
        & $inf2cat.FullName "/driver:$pk" '/os:10_CO_ARM64,10_NI_ARM64,10_GE_ARM64' '/uselocaltime' | Out-Host
        if ($LASTEXITCODE) { throw "Inf2Cat $name" }
        Copy-Item $inf, "$pk\$name.cat" $Out
    }
}

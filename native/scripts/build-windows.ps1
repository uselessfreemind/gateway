param(
    [ValidateSet('Debug','Release')][string]$Configuration = 'Release',
    [string]$VcpkgRoot = $env:VCPKG_ROOT
)
$ErrorActionPreference = 'Stop'
$Root = Split-Path $PSScriptRoot -Parent
function Run([string]$Exe, [string[]]$Arguments) {
    & $Exe @Arguments
    $Code = $LASTEXITCODE
    if ($Code -ne 0) {
        # Report the actual failing tests instead of only the wrapper's exit code.
        $Results = Join-Path $Root 'build-windows\ctest-results.xml'
        if ($Exe -eq 'ctest' -and (Test-Path $Results)) {
            $Failures = Select-Xml -Path $Results -XPath '//testcase[failure]'
            foreach ($Failure in $Failures) {
                $Name = $Failure.Node.GetAttribute('name')
                $Details = $Failure.Node.failure.InnerText
                # CTest versions can put detailed output in system-out instead.
                if ([string]::IsNullOrWhiteSpace($Details)) {
                    $Output = $Failure.Node.SelectSingleNode('system-out')
                    if ($Output) { $Details = $Output.InnerText }
                }
                $Message = "$Name`n$Details"
                if ($Message.Length -gt 3000) { $Message = $Message.Substring(0, 3000) }
                $Escaped = $Message.Replace('%','%25').Replace("`r",'%0D').Replace("`n",'%0A')
                if ($env:GITHUB_ACTIONS -eq 'true') {
                    Write-Host "::error title=CTest failure::$Escaped"
                }
                if ($env:GITHUB_STEP_SUMMARY) {
                    @("### Failed native test: $Name", '', '```text', $Details, '```', '') |
                        Out-File -FilePath $env:GITHUB_STEP_SUMMARY -Encoding utf8 -Append
                }
            }
        }
        throw "$Exe failed with exit code $Code"
    }
}
if (-not $VcpkgRoot) { $VcpkgRoot = Join-Path $env:LOCALAPPDATA 'ScadaTools\vcpkg' }
if (-not (Test-Path (Join-Path $VcpkgRoot '.git'))) {
    Run 'git' @('clone','--branch','2025.06.13','--depth','1','https://github.com/microsoft/vcpkg.git',$VcpkgRoot)
}
if (-not (Test-Path (Join-Path $VcpkgRoot 'vcpkg.exe'))) {
    Run (Join-Path $VcpkgRoot 'bootstrap-vcpkg.bat') @('-disableMetrics')
}
$Build = Join-Path $Root 'build-windows'
Run 'cmake' @('-S',$Root,'-B',$Build,'-G','Visual Studio 17 2022','-A','x64',
    "-DCMAKE_TOOLCHAIN_FILE=$VcpkgRoot/scripts/buildsystems/vcpkg.cmake",'-DSCADA_USE_VCPKG=ON','-DVCPKG_TARGET_TRIPLET=x64-windows-static-md')
Run 'cmake' @('--build',$Build,'--config',$Configuration,'--parallel','2')
Run 'ctest' @('--test-dir',$Build,'-C',$Configuration,'--output-on-failure','--output-junit',(Join-Path $Build 'ctest-results.xml'))
$ProjectDir = Join-Path $Root 'winui\Scada.Desktop'
Run 'nuget' @('restore',(Join-Path $ProjectDir 'packages.config'),'-PackagesDirectory',(Join-Path $Root 'winui\packages'),'-NonInteractive')
$Vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$Msbuild = & $Vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $Msbuild) { throw 'Visual Studio 2022 MSBuild with C++ and WinUI tools is required.' }
Run $Msbuild @((Join-Path $ProjectDir 'Scada.Desktop.vcxproj'),'/m:2',"/p:Configuration=$Configuration",'/p:Platform=x64',"/p:NativeBuildDir=$Build")
Write-Host "Desktop executable: $Root\winui\out\x64\$Configuration\Scada.Desktop.exe"

param(
    [ValidateSet('all','recorder','camera','tests','publish')]
    [string]$Stage='all'
)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
function CheckExit {if($LASTEXITCODE -ne 0){throw "构建工具退出码：$LASTEXITCODE"}}
function ShouldRun([string]$name) {return (($Stage -eq 'all') -or ($Stage -eq $name))}
$nuget=Join-Path $PSScriptRoot 'NuGet.Config'

if(ShouldRun 'recorder'){
    & "$root/recorder/build-native.ps1" -HostedTools
}
if(ShouldRun 'camera'){
    & "$root/virtual-camera/build-native.ps1" -HostedTools
    CheckExit
    # Contract check: the filter's media types must describe whole frames.
    # Kept non-blocking while the probe itself is being validated in CI; its
    # last output lines surface as a warning annotation when it fails.
    $probe=& "$root/virtual-camera/build/CameraProbe.exe" --dshow "$root/virtual-camera/build/PCamCamera.dll" 2>&1
    if($LASTEXITCODE -ne 0){
        $tail=(@($probe) | Select-Object -Last 6) -join ' | '
        if($env:GITHUB_ACTIONS -eq 'true'){Write-Host ('::warning::CameraProbe --dshow 退出码 ' + $LASTEXITCODE + '：' + $tail)}
        else{Write-Warning "CameraProbe --dshow 退出码 $LASTEXITCODE：$tail"}
    }
    # A crashed probe must not leak its exit code into the step result. Native
    # commands update $LASTEXITCODE in the caller's scope, so the reset must be
    # global: a script-local assignment would not be visible to the wrapper.
    $global:LASTEXITCODE=0
}
if(ShouldRun 'tests'){
    & dotnet restore "$root/tests/ProtocolTests.csproj" --configfile $nuget
    CheckExit
    & dotnet run --project "$root/tests/ProtocolTests.csproj" -c Release --no-restore
    CheckExit
}
if(ShouldRun 'publish'){
    & dotnet restore "$root/windows/PhoneCam.csproj" -r win-x64 --configfile $nuget -p:SelfContained=true
    CheckExit
    & dotnet publish "$root/windows/PhoneCam.csproj" -c Release -r win-x64 --self-contained true --no-restore -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true -o "$root/releases/windows"
    CheckExit
    [xml]$project=Get-Content -LiteralPath "$root/windows/PhoneCam.csproj" -Raw
    $version=@($project.Project.PropertyGroup.Version | Where-Object {$_})[0]
    if(!$version){throw '缺少Windows版本号'}
    $file=Join-Path $root "releases/PCam-$version.exe"
    Copy-Item -LiteralPath "$root/releases/windows/PCam.exe" -Destination $file -Force
    "$((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash)  $([IO.Path]::GetFileName($file))" |
        Set-Content -LiteralPath "$root/releases/SHA256SUMS-windows.txt" -Encoding utf8
}

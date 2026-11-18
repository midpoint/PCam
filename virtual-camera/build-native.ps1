param([switch]$ProbeOnly, [switch]$HostedTools)
$ErrorActionPreference='Stop'
if($HostedTools){
    . "$PSScriptRoot/../ci/native-tools.ps1"
    $compiler64=$nativeCompiler
    $compiler32=$nativeCompilerX86
    $lib32=$nativeLibX86
}else{
$tools=Join-Path (Split-Path (Split-Path $PSScriptRoot)) 'tools'
$vs=Join-Path $tools 'vs-buildtools'
$compiler=(Get-ChildItem "$tools/portable-msvc/Contents/VC/Tools/MSVC" -Directory | Sort-Object Name -Descending | Select-Object -First 1).FullName
$sdk=Join-Path $tools 'microsoft.windows.sdk.cpp/c'
$sdkLib=Join-Path $tools 'microsoft.windows.sdk.cpp.x64/c'
$sdkVersion=(Get-ChildItem "$sdk/Include" -Directory | Sort-Object Name -Descending | Select-Object -First 1).Name
$env:PATH="$compiler/bin/Hostx64/x64;$env:PATH"
$env:INCLUDE="$compiler/include;$sdk/Include/$sdkVersion/ucrt;$sdk/Include/$sdkVersion/shared;$sdk/Include/$sdkVersion/um;$sdk/Include/$sdkVersion/winrt;$sdk/Include/$sdkVersion/cppwinrt;$tools/wil/include"
$env:LIB="$compiler/lib/x64;$sdkLib/ucrt/x64;$sdkLib/um/x64"
$compiler64=Join-Path $compiler 'bin/Hostx64/x64/cl.exe'
$compiler32=Join-Path $compiler 'bin/Hostx64/x86/cl.exe'
if(!(Test-Path -LiteralPath $compiler32)){$compiler32=Join-Path $compiler 'bin/Hostx86/x86/cl.exe'}
# The SDK libraries ship per architecture; use the x86 package when the local
# tools provide one, otherwise expect the x64 package to carry x86 too.
$sdkLibX86=Join-Path $tools 'microsoft.windows.sdk.cpp.x86/c'
if(!(Test-Path -LiteralPath "$sdkLibX86/um/x86")){$sdkLibX86=$sdkLib}
$lib32="$compiler/lib/x86;$sdkLibX86/ucrt/x86;$sdkLibX86/um/x86"
}
if(!(Test-Path -LiteralPath $compiler32)){throw '找不到32位MSVC编译器（需要Hostx64/x86）'}
foreach($library in ($lib32 -split ';')){if(!(Test-Path -LiteralPath $library)){throw "缺少32位库目录：$library"}}
New-Item -ItemType Directory -Force -Path "$PSScriptRoot/build" | Out-Null
Push-Location "$PSScriptRoot/build"
try{
    $names=@('dllmain','winrtCommon','SimpleMediaSource','SimpleMediaStream','SimpleFrameGenerator','VirtualCameraMediaSourceActivate','PcamBridge','PcamDirectShow')
    $sources=@($names | ForEach-Object {"$PSScriptRoot/$_.cpp"})
    # Builds the same filter for both registry views: 64-bit consumers load
    # PCamCamera.dll, 32-bit consumers (ManyCam and other older apps) load
    # PCamCamera32.dll. Only the library search path differs per target.
    function Build-Filter([string]$compiler,[string]$libraries,[string]$output){
        $saved=$env:LIB
        try{
            $env:LIB=$libraries
            & $compiler /nologo /std:c++17 /EHsc /O2 /MT /W3 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /LD /I"$PSScriptRoot" @sources /link /Brepro /NODEFAULTLIB:oldnames.lib "/DEF:$PSScriptRoot/VirtualCameraMediaSource.def" "/OUT:$output" ole32.lib advapi32.lib mfreadwrite.lib shlwapi.lib
            if($LASTEXITCODE -ne 0){throw "Native camera build failed: $output"}
        }finally{$env:LIB=$saved}
    }
    if(!$ProbeOnly){
        Build-Filter $compiler64 $env:LIB 'PCamCamera.dll'
        Build-Filter $compiler32 $lib32 'PCamCamera32.dll'
    }
    # Probes for both bitnesses: a 64-bit probe cannot load the 32-bit filter.
    function Build-Probe([string]$compiler,[string]$libraries,[string]$output){
        $saved=$env:LIB
        try{
            $env:LIB=$libraries
            & $compiler /nologo /std:c++17 /EHsc /O2 /MT /DUNICODE /D_UNICODE /DNOMINMAX "$PSScriptRoot/CameraProbe.cpp" /link /Brepro /NODEFAULTLIB:oldnames.lib "/OUT:$output"
            if($LASTEXITCODE -ne 0){throw "Camera probe build failed: $output"}
        }finally{$env:LIB=$saved}
    }
    Build-Probe $compiler64 $env:LIB 'CameraProbe.exe'
    Build-Probe $compiler32 $lib32 'CameraProbe32.exe'
}finally{Pop-Location}

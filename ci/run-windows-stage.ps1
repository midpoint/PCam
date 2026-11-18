param(
    [Parameter(Mandatory)]
    [ValidateSet('recorder','camera','tests','publish')]
    [string]$Stage
)
$ErrorActionPreference='Stop'
$PSNativeCommandUseErrorActionPreference=$false
$log=Join-Path ([IO.Path]::GetTempPath()) "pcam-build-$Stage.log"
$failure=$null
try{
    & "$PSScriptRoot/build-windows.ps1" -Stage $Stage *>&1 | Tee-Object -FilePath $log
}catch{
    $failure=$_
}
if($failure){
    Write-Host $failure.Exception.Message
    if($env:GITHUB_ACTIONS -eq 'true'){
        Write-Host ('::error::' + ($failure.Exception.Message -replace '%','%25' -replace "`r",'' -replace "`n",'%0A'))
        Get-Content -LiteralPath $log -ErrorAction SilentlyContinue |
            Select-String -Pattern 'error [A-Z]{1,2}\d{4}|fatal error|error LNK\d+|error MSB\d+|error NU\d+' |
            Select-Object -First 10 |
            ForEach-Object { Write-Host ('::error::' + ($_.Line.Trim() -replace '%','%25' -replace "`r",'' -replace "`n",'%0A')) }
    }
    exit 1
}

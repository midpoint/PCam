$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$repo = $env:GITHUB_REPOSITORY
$sha = $env:GITHUB_SHA
# This fork publishes the Windows installer it builds plus the already-signed
# Android package from the upstream release. The Android signing secrets stay
# with the upstream repository, so no Android build runs here.
if ($repo -ne 'midpoint/PCam' -or $env:GITHUB_REF -ne 'refs/heads/main' -or
    $sha -notmatch '^[0-9a-fA-F]{40}$' -or $env:GITHUB_RUN_ID -notmatch '^\d+$') {
    throw 'Only this fork''s main branch can publish releases.'
}
function InvokeGh([string[]]$Arguments) {
    $result = @(& gh @Arguments)
    if ($LASTEXITCODE -ne 0) { throw "GitHub CLI failed: $($Arguments[0])" }
    return $result
}
[xml]$project = Get-Content -LiteralPath "$root/windows/PhoneCam.csproj" -Raw
$version = @($project.Project.PropertyGroup.Version | Where-Object { $_ })[0]
$androidVersion = [regex]::Match((Get-Content -LiteralPath "$root/android/app/build.gradle" -Raw), "versionName\s+'([^']+)'").Groups[1].Value
foreach ($item in @($version, $androidVersion)) {
    if ($item -notmatch '^\d+\.\d+\.\d+$') { throw 'Invalid release version.' }
}
$tag = "v$version"
$exe = "PCam-$version.exe"
$apk = "PCam-$androidVersion.apk"
$sumsWindows = 'SHA256SUMS-windows.txt'
$sumsAndroid = 'SHA256SUMS-android.txt'
$names = @($exe, $apk, $sumsWindows, $sumsAndroid)
function GetRelease {
    # The authenticated releases list includes drafts, whose tags may not exist yet.
    $query = '.[] | select(.tag_name == "' + $tag + '")'
    $json = (InvokeGh -Arguments @('api', "repos/$repo/releases?per_page=100", '--paginate', '--jq', $query)) -join "`n"
    if ($json) { return ($json | ConvertFrom-Json) }
    return $null
}
$existing = GetRelease
if ($existing) {
    $tagCommit = if ($existing.draft) { $existing.target_commitish } else {
        (InvokeGh -Arguments @('api', "repos/$repo/commits/$tag", '--jq', '.sha')) -join ''
    }
    # A version identifies one source commit. A rerun must never replace a published installer.
    if ($tagCommit -ne $sha) { throw 'This version already belongs to another commit. Increase the version first.' }
    if (!$existing.draft) {
        foreach ($name in $names) {
            if ($name -notin @($existing.assets.name)) { throw "Published release is missing $name; it will not be overwritten." }
        }
        Write-Output "Already published: $($existing.html_url)"
        return
    }
}

$work = Join-Path $env:RUNNER_TEMP ('pcam-publish-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
InvokeGh -Arguments @('run', 'download', $env:GITHUB_RUN_ID, '--repo', $repo, '--name', 'PCam-Windows', '--dir', $work) | Out-Null
# The Android package is the signed build published by the upstream project;
# copy that exact file so both repositories offer the same installer.
$upstream = 'hyyh0144/PCam'
$query = '.[] | select((.draft | not) and (.prerelease | not) and (.tag_name | endswith("-android-v' + $androidVersion + '")))'
$found = (InvokeGh -Arguments @('api', "repos/$upstream/releases?per_page=100", '--paginate', '--jq', $query)) -join "`n"
$upstreamRelease = $found -split "`n" | Where-Object { $_.Trim() } | ForEach-Object { $_ | ConvertFrom-Json } | Select-Object -First 1
if (!$upstreamRelease) { throw "No upstream release provides Android $androidVersion." }
foreach ($name in @($apk, $sumsAndroid)) {
    $match = @($upstreamRelease.assets | Where-Object { $_.name -eq $name })
    if ($match.Count -ne 1) { throw "Upstream release $($upstreamRelease.tag_name) is missing $name." }
    Invoke-WebRequest -Uri $match[0].browser_download_url -OutFile (Join-Path $work $name)
}
foreach ($path in @(($names | ForEach-Object { Join-Path $work $_ }))) {
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Artifact is missing $([IO.Path]::GetFileName($path))." }
}
function AssertChecksum([string]$file, [string]$sums) {
    $line = (Get-Content -LiteralPath (Join-Path $work $sums) -Raw).Trim()
    $match = [regex]::Match($line, '^([0-9a-fA-F]{64})\s{2}([^\r\n]+)$')
    $path = Join-Path $work $file
    if (!$match.Success -or $match.Groups[2].Value -ne $file -or
        (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $match.Groups[1].Value) {
        throw "Artifact checksum mismatch: $file"
    }
}
AssertChecksum $exe $sumsWindows
AssertChecksum $apk $sumsAndroid
$notes = @"
让手机成为电脑的眼睛。

- Windows 电脑端：$exe
- Android 手机端：$apk（与上游项目发布的已签名安装包相同）
- 本仓库是上游 PCam 项目的 fork：Windows 安装包在此构建，Android 安装包复制自上游发布。
- Windows 10 通过 DirectShow 提供虚拟摄像头，Windows 11 使用系统虚拟摄像头接口；首次安装需要管理员权限。
- 自动构建记录：https://github.com/$repo/actions/runs/$env:GITHUB_RUN_ID
- SHA256SUMS 文件可用于核对下载完整性。
"@
$notesFile = Join-Path $work 'release-notes.md'
[IO.File]::WriteAllText($notesFile, $notes, [Text.UTF8Encoding]::new($false))
if (!$existing) {
    InvokeGh -Arguments @('release', 'create', $tag, '--repo', $repo, '--target', $sha,
        '--title', "PCam — Windows $version / Android $androidVersion", '--notes-file', $notesFile, '--draft') | Out-Null
}
# Upload into a draft first: failed uploads never expose a half-complete public release.
InvokeGh -Arguments (@('release', 'upload', $tag, '--repo', $repo, '--clobber') + @($names | ForEach-Object { Join-Path $work $_ })) | Out-Null
$uploaded = GetRelease
foreach ($name in $names) {
    $local = Get-Item -LiteralPath (Join-Path $work $name)
    $asset = @($uploaded.assets | Where-Object { $_.name -eq $local.Name })
    if ($asset.Count -ne 1 -or $asset[0].size -ne $local.Length) { throw "Incomplete upload: $($local.Name)" }
}
InvokeGh -Arguments @('release', 'edit', $tag, '--repo', $repo, '--draft=false', '--latest') | Out-Null
$url = "https://github.com/$repo/releases/tag/$tag"
if ($env:GITHUB_STEP_SUMMARY) { "Release: $url" | Add-Content -LiteralPath $env:GITHUB_STEP_SUMMARY -Encoding utf8 }
Write-Output $url

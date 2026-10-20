# Offline packaging/publication orchestration tests. gh/git below are in-process fakes.
param([Parameter(Mandatory = $true)][string]$BuildDir)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$BuildDir = (Resolve-Path $BuildDir).Path
$temp = Join-Path ([IO.Path]::GetTempPath()) ('p4vpatch-release-tests-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $temp | Out-Null
$package = Join-Path $root 'tools\package-release.ps1'
$publish = Join-Path $root 'tools\publish-release.ps1'
$version = (Get-Content (Join-Path $root 'VERSION') -Raw).Trim()
function Check([bool]$condition, [string]$name) {
    if (-not $condition) { throw "FAIL $name" }
    Write-Host "PASS $name"
}
function Expect-Failure([scriptblock]$operation, [string]$name) {
    $failed = $false
    try { & $operation | Out-Null } catch { $failed = $true }
    Check $failed $name
}
$script:ghCalls = New-Object 'Collections.Generic.List[string]'
$global:P4VPatchReleaseTestState = @{ Calls = $script:ghCalls; ExistingTag = $false; FailUpload = $false; Dirty = $false; Version = $version }
function git {
    $global:LASTEXITCODE = 0
    if ($args -contains 'rev-parse') { 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa' }
    elseif ($args -contains 'status') { if ($global:P4VPatchReleaseTestState.Dirty) { ' M VERSION' } }
    else { throw "Unexpected git command: $args" }
}
function gh {
    $global:LASTEXITCODE = 0
    $global:P4VPatchReleaseTestState.Calls.Add(($args -join ' '))
    if ($args[0] -eq 'api') {
        if ($args[1] -like '*/git/matching-refs/*') {
            if ($global:P4VPatchReleaseTestState.ExistingTag) { '[{"ref":"refs/tags/v' + $global:P4VPatchReleaseTestState.Version + '"}]' } else { '[]' }
        } elseif ($args[1] -notlike '*/commits/*') { throw "Unexpected gh API: $args" }
    } elseif ($args[0] -ne 'release') { throw "Unexpected gh command: $args" }
    elseif ($args[1] -eq 'create' -and $global:P4VPatchReleaseTestState.FailUpload) { $global:LASTEXITCODE = 1 }
}
try {
    foreach ($path in @($package, $publish, (Join-Path $root 'build.ps1'))) {
        $tokens = $null; $errors = $null
        [Management.Automation.Language.Parser]::ParseFile($path, [ref]$tokens, [ref]$errors) | Out-Null
        Check ($errors.Count -eq 0) "PowerShell syntax: $(Split-Path $path -Leaf)"
    }
    $out = & $package -BuildDir $BuildDir -OutputRoot (Join-Path $temp 'package') -SkipBuild
    Check ((@($out)).Count -eq 1) 'packaging returns only the asset directory'
    $manifest = Get-Content -LiteralPath (Join-Path $out 'p4vpatch-update.json') -Raw | ConvertFrom-Json
    $dll = Get-Item -LiteralPath (Join-Path $out 'p4vpatch.dll')
    Check ($manifest.version -eq $version -and $manifest.schema -eq 1) 'manifest version/schema'
    Check ($manifest.dll.size -eq $dll.Length -and $manifest.dll.sha256 -eq (Get-FileHash $dll.FullName -Algorithm SHA256).Hash.ToLowerInvariant()) 'manifest hash and size'
    Check ($manifest.dll.url -eq "https://github.com/librashuai/p4vpatch/releases/download/v$version/p4vpatch.dll") 'version-bound download URL'
    Check ((Get-ChildItem -LiteralPath $out -File).Count -eq 4) 'four release assets'
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead((Join-Path $out "p4vpatch-v$version-windows-x64.zip"))
    try {
        $names = @($archive.Entries | Where-Object { $_.Name } | ForEach-Object { $_.FullName.Replace('\', '/') })
        Check ($names.Count -eq 4 -and $names -contains 'p4vpatch-launcher.exe' -and $names -contains 'p4vpatch.dll' -and $names -contains 'docs/releases.md' -and @($names | Where-Object { $_ -match '^(Qt6|p4v\.exe)' }).Count -eq 0) 'complete ZIP without Qt/P4V runtime'
    } finally { $archive.Dispose() }
    foreach ($line in Get-Content (Join-Path $out 'SHA256SUMS.txt')) {
        $parts = $line -split '  ', 2
        Check ($parts[0] -eq (Get-FileHash (Join-Path $out $parts[1]) -Algorithm SHA256).Hash.ToLowerInvariant()) "checksum: $($parts[1])"
    }
    Expect-Failure { & $package -BuildDir $BuildDir -OutputRoot (Join-Path $temp 'package') -SkipBuild } 'refuse stale output directory'
    Expect-Failure { & $package -BuildDir $BuildDir -OutputRoot (Join-Path $temp 'bad-version') -MinimumLauncherVersion 'v1.2.3' -SkipBuild } 'reject invalid launcher version'
    Expect-Failure { & $package -BuildDir $BuildDir -OutputRoot (Join-Path $temp 'wrong-repo') -Repository 'other/repo' -SkipBuild } 'reject repository/build mismatch'
    $notes = Join-Path $temp 'notes.md'
    Set-Content -LiteralPath $notes -Value 'Test release notes'
    try { & $publish -NotesFile $notes -BuildDir $BuildDir -OutputRoot (Join-Path $temp 'draft') -SkipBuild }
    catch { Write-Host $_.ScriptStackTrace; throw }
    Check (@($script:ghCalls | Where-Object { $_ -like 'release create *' -and $_ -like '*--draft*' }).Count -eq 1) 'draft creation includes assets'
    Check (@($script:ghCalls | Where-Object { $_ -like 'release edit *' }).Count -eq 0) 'default never publishes'
    $script:ghCalls.Clear()
    & $publish -NotesFile $notes -BuildDir $BuildDir -OutputRoot (Join-Path $temp 'public') -SkipBuild -Publish
    $create = @($script:ghCalls | Where-Object { $_ -like 'release create *' })
    $edit = @($script:ghCalls | Where-Object { $_ -like 'release edit *' })
    Check ($create.Count -eq 1 -and $edit.Count -eq 1 -and $edit[0] -like '*--draft=false --latest*') 'explicit publish edits uploaded draft to latest'
    Check ($script:ghCalls.IndexOf($create[0]) -lt $script:ghCalls.IndexOf($edit[0])) 'upload before publication'
    $global:P4VPatchReleaseTestState.FailUpload = $true
    $script:ghCalls.Clear()
    Expect-Failure { & $publish -NotesFile $notes -BuildDir $BuildDir -OutputRoot (Join-Path $temp 'failed-upload') -SkipBuild -Publish } 'upload failure aborts publication'
    Check (@($script:ghCalls | Where-Object { $_ -like 'release edit *' }).Count -eq 0) 'incomplete upload never published'
    $global:P4VPatchReleaseTestState.FailUpload = $false
    $global:P4VPatchReleaseTestState.Dirty = $true
    $script:ghCalls.Clear()
    Expect-Failure { & $publish -NotesFile $notes -BuildDir $BuildDir -SkipBuild -Publish } 'dirty source rejected'
    Check ($script:ghCalls.Count -eq 0) 'dirty source never reaches GitHub'
    $global:P4VPatchReleaseTestState.Dirty = $false
    $global:P4VPatchReleaseTestState.ExistingTag = $true
    $script:ghCalls.Clear()
    Expect-Failure { & $publish -NotesFile $notes -BuildDir $BuildDir -OutputRoot (Join-Path $temp 'duplicate') -SkipBuild -Publish } 'existing tag rejected'
    Check (@($script:ghCalls | Where-Object { $_ -like 'release *' }).Count -eq 0) 'existing release never modified'
    Write-Host 'All release script tests passed. No GitHub commands were executed.'
} finally {
    Remove-Variable -Name P4VPatchReleaseTestState -Scope Global -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}

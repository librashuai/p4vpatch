# Explicit network operation: creates a draft by default; -Publish makes it public/latest.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$NotesFile,
    [string]$Repository = 'librashuai/p4vpatch',
    [string]$QtSdk = '',
    [string]$BuildDir = '',
    [string]$OutputRoot = '',
    [string]$MinimumLauncherVersion = '0.1.0',
    [switch]$Publish,
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $QtSdk) { $QtSdk = Join-Path $root '.sdk\6.8.3\msvc2022_64' }
if (-not $BuildDir) { $BuildDir = Join-Path $root 'build-release' }
if (-not (Get-Command gh -ErrorAction SilentlyContinue)) { throw 'Install GitHub CLI and run gh auth login first.' }
if (-not (Get-Command git -ErrorAction SilentlyContinue)) { throw 'Git is required.' }
$NotesFile = (Resolve-Path -LiteralPath $NotesFile).Path
if (-not (Get-Item -LiteralPath $NotesFile).Length) { throw 'Release notes cannot be empty.' }
$version = (Get-Content -LiteralPath (Join-Path $root 'VERSION') -Raw).Trim()
if ($version -notmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$') { throw 'Invalid VERSION.' }
if ($Repository -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$') { throw 'Repository must be owner/repository.' }
$status = & git -C $root status --porcelain
if ($LASTEXITCODE -ne 0 -or $status) { throw 'Commit all source/version/notes changes before publishing (clean worktree required).' }
$commit = (& git -C $root rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot determine commit.' }
& gh api "repos/$Repository/commits/$commit" --silent
if ($LASTEXITCODE -ne 0) { throw 'Push the current commit to the target GitHub repository before publishing.' }
# Never silently overwrite existing tags/assets; a published version is immutable.
$refsJson = & gh api "repos/$Repository/git/matching-refs/tags/v$version"
if ($LASTEXITCODE -ne 0) { throw 'Cannot query existing tags; publishing cancelled.' }
$refs = @($refsJson | ConvertFrom-Json)
if (@($refs | Where-Object { $_.ref -eq "refs/tags/v$version" }).Count) {
    throw "Tag v$version already exists. Increment VERSION for a new release."
}
$packageArgs = @{ QtSdk = $QtSdk; BuildDir = $BuildDir; OutputRoot = $OutputRoot; Repository = $Repository; MinimumLauncherVersion = $MinimumLauncherVersion; SkipBuild = $SkipBuild }
$assetsDir = & (Join-Path $PSScriptRoot 'package-release.ps1') @packageArgs
$assets = @(
    (Join-Path $assetsDir "p4vpatch-v$version-windows-x64.zip"),
    (Join-Path $assetsDir 'p4vpatch.dll'),
    (Join-Path $assetsDir 'p4vpatch-update.json'),
    (Join-Path $assetsDir 'SHA256SUMS.txt')
)
# Publish only after every asset is uploaded. A failed upload leaves a non-public draft.
& gh release create "v$version" @assets --repo $Repository --target $commit --title "P4V Patch v$version" --notes-file $NotesFile --draft
if ($LASTEXITCODE -ne 0) { throw 'Draft creation/upload failed. Inspect GitHub drafts before retrying; do not publish incomplete assets.' }
if ($Publish) {
    & gh release edit "v$version" --repo $Repository --draft=false --latest
    if ($LASTEXITCODE -ne 0) { throw 'Publishing failed; inspect the draft on GitHub.' }
    Write-Host "Published v$version as the latest stable release."
} else {
    Write-Host "Created draft v$version. Validate all assets, then publish it as the latest stable release on GitHub."
}

# Build a complete user ZIP and the two assets consumed by the launcher updater.
[CmdletBinding()]
param(
    [string]$QtSdk = '',
    [string]$BuildDir = '',
    [string]$OutputRoot = '',
    [string]$Repository = 'librashuai/p4vpatch',
    [string]$MinimumLauncherVersion = '0.1.0',
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $QtSdk) { $QtSdk = Join-Path $root '.sdk\6.8.3\msvc2022_64' }
if (-not $BuildDir) { $BuildDir = Join-Path $root 'build-release' }
if (-not $OutputRoot) { $OutputRoot = Join-Path $root 'dist' }
$version = (Get-Content -LiteralPath (Join-Path $root 'VERSION') -Raw).Trim()
function Assert-Version([string]$value) {
    if ($value -notmatch '^(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})$' -or
        @($value.Split('.') | Where-Object { [int]$_ -gt 65535 }).Count) { throw "Invalid stable version: $value" }
}
Assert-Version $version
Assert-Version $MinimumLauncherVersion
if ([version]$MinimumLauncherVersion -gt [version]$version) { throw 'Minimum launcher version cannot exceed release version.' }
if ($Repository -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$') { throw 'Repository must be owner/repository.' }
if (-not $SkipBuild) {
    & (Join-Path $root 'build.ps1') -QtSdk $QtSdk -BuildDir $BuildDir -Repository $Repository | Out-Host
}
$BuildDir = (Resolve-Path $BuildDir).Path
$header = Get-Content -LiteralPath (Join-Path $BuildDir 'generated\version.h') -Raw
if ($header -notmatch ('#define P4VPATCH_REPOSITORY "' + [regex]::Escape($Repository) + '"') -or
    $header -notmatch ('#define P4VPATCH_VERSION "' + [regex]::Escape($version) + '"')) {
    throw 'Build metadata does not match VERSION/repository; rebuild before packaging.'
}
$qt = [regex]::Match($header, '#define P4VPATCH_QT_VERSION "([^"]+)"').Groups[1].Value
$p4v = [regex]::Match($header, '#define P4VPATCH_P4V_VERSION "([^"]+)"').Groups[1].Value
if (-not $qt -or -not $p4v) { throw 'Missing compatibility metadata.' }
foreach ($name in @('p4vpatch.dll', 'p4vpatch-launcher.exe')) {
    $info = [Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $BuildDir $name))
    if ($info.FileVersion -ne $version) { throw "$name version $($info.FileVersion) does not match $version." }
}
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
$output = Join-Path ([IO.Path]::GetFullPath($OutputRoot)) "v$version"
if (Test-Path -LiteralPath $output) { throw "Output already exists: $output. Remove it explicitly before rebuilding this release." }
New-Item -ItemType Directory -Path $output | Out-Null
$stage = Join-Path $output 'package'
try {
    New-Item -ItemType Directory -Path $stage | Out-Null
    Copy-Item -LiteralPath (Join-Path $BuildDir 'p4vpatch.dll') -Destination $stage
    Copy-Item -LiteralPath (Join-Path $BuildDir 'p4vpatch-launcher.exe') -Destination $stage
    Copy-Item -LiteralPath (Join-Path $root 'README.md') -Destination $stage
    New-Item -ItemType Directory -Path (Join-Path $stage 'docs') | Out-Null
    Copy-Item -LiteralPath (Join-Path $root 'docs\releases.md') -Destination (Join-Path $stage 'docs\releases.md')
    Copy-Item -LiteralPath (Join-Path $BuildDir 'p4vpatch.dll') -Destination $output
    $dll = Get-Item -LiteralPath (Join-Path $output 'p4vpatch.dll')
    if ($dll.Length -gt 64MB) { throw 'DLL exceeds launcher download size limit.' }
    $manifest = [ordered]@{
        schema = 1
        version = $version
        minimum_launcher_version = $MinimumLauncherVersion
        p4v_version = $p4v
        qt_version = $qt
        dll = [ordered]@{
            url = "https://github.com/$Repository/releases/download/v$version/p4vpatch.dll"
            size = $dll.Length
            sha256 = (Get-FileHash -LiteralPath $dll.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
    $utf8 = New-Object Text.UTF8Encoding($false)
    [IO.File]::WriteAllText((Join-Path $output 'p4vpatch-update.json'), ($manifest | ConvertTo-Json -Depth 4), $utf8)
    $zip = Join-Path $output "p4vpatch-v$version-windows-x64.zip"
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal
    $hashes = @('p4vpatch.dll', 'p4vpatch-update.json', (Split-Path $zip -Leaf)) | ForEach-Object {
        $hash = (Get-FileHash -LiteralPath (Join-Path $output $_) -Algorithm SHA256).Hash.ToLowerInvariant()
        "$hash  $_"
    }
    [IO.File]::WriteAllLines((Join-Path $output 'SHA256SUMS.txt'), [string[]]$hashes, $utf8)
    Remove-Item -LiteralPath $stage -Recurse -Force
} catch {
    Remove-Item -LiteralPath $output -Recurse -Force -ErrorAction SilentlyContinue
    throw
}
Write-Host "Release assets: $output"
# A single output object lets publish-release.ps1 consume the package location.
$output

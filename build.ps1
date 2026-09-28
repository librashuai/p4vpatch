# Run from an x64 Visual Studio Developer PowerShell / Developer Command Prompt.
param(
    [string]$QtSdk = (Join-Path $PSScriptRoot '.sdk\6.8.3\msvc2022_64'),
    [string]$BuildDir = (Join-Path $PSScriptRoot 'build-msvc')
)

$ErrorActionPreference = 'Stop'
foreach ($tool in @('cmake', 'ninja', 'cl.exe')) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "Missing $tool. Run in an x64 Visual Studio Developer PowerShell (with CMake and Ninja on PATH)."
    }
}
if (-not (Test-Path (Join-Path $QtSdk 'lib\cmake\Qt6\Qt6Config.cmake'))) {
    throw "Qt 6.8.3 MSVC x64 SDK not found at $QtSdk. Pass -QtSdk <sdk-path>."
}
& cmake -S $PSScriptRoot -B $BuildDir -G Ninja '-DCMAKE_BUILD_TYPE=Release' "-DCMAKE_PREFIX_PATH=$QtSdk" '-DBUILD_PATCH=ON'
if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed ($LASTEXITCODE)" }
& cmake --build $BuildDir --config Release
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }
Write-Host "Built $BuildDir\p4vpatch-launcher.exe and $BuildDir\p4vpatch.dll"

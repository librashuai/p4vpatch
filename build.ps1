# Runs from a regular PowerShell; discovers Visual Studio and initializes MSVC x64 if needed.
param(
    [string]$QtSdk = (Join-Path $PSScriptRoot '.sdk\6.8.3\msvc2022_64'),
    [string]$BuildDir = (Join-Path $PSScriptRoot 'build-msvc')
)

$ErrorActionPreference = 'Stop'
$missingTools = @('cmake', 'ninja', 'cl.exe') | Where-Object { -not (Get-Command $_ -ErrorAction SilentlyContinue) }
if ($missingTools) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        throw "Missing $($missingTools -join ', '). Cannot find vswhere.exe at $vswhere; install Visual Studio with the C++ desktop workload."
    }
    $vsInstall = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $vsInstall) {
        throw 'No Visual Studio installation with MSVC x64 tools found. Install the Desktop development with C++ workload.'
    }
    $vsInstall = ($vsInstall | Select-Object -First 1).Trim()

    if ('cl.exe' -in $missingTools) {
        $vsDevCmd = Join-Path $vsInstall 'Common7\Tools\VsDevCmd.bat'
        if (-not (Test-Path $vsDevCmd)) { throw "Missing Visual Studio environment script: $vsDevCmd" }
        # A batch file cannot modify its parent PowerShell process; import its environment.
        $envScript = Join-Path $env:TEMP ("p4vpatch-vs-$([guid]::NewGuid()).cmd")
        try {
            Set-Content -LiteralPath $envScript -Encoding Ascii -Value @(
                '@echo off'
                "call `"$vsDevCmd`" -no_logo -arch=amd64 -host_arch=amd64 >nul"
                'if errorlevel 1 exit /b 1'
                'set'
            )
            $vsEnv = & $env:ComSpec /d /c "`"$envScript`""
            if ($LASTEXITCODE -ne 0) { throw "Failed to initialize the x64 MSVC environment via $vsDevCmd" }
            foreach ($line in $vsEnv) {
                if ($line -match '^([^=]+)=(.*)$') {
                    [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
                }
            }
        } finally {
            Remove-Item -LiteralPath $envScript -ErrorAction SilentlyContinue
        }
    }
    # VS ships CMake and Ninja, but VsDevCmd does not always add them to PATH.
    $cmakeTools = Join-Path $vsInstall 'Common7\IDE\CommonExtensions\Microsoft\CMake'
    foreach ($entry in @(@('cmake', 'CMake\bin'), @('ninja', 'Ninja'))) {
        if (-not (Get-Command $entry[0] -ErrorAction SilentlyContinue)) {
            $toolDir = Join-Path $cmakeTools $entry[1]
            if (Test-Path (Join-Path $toolDir "$($entry[0]).exe")) {
                $env:PATH = "$toolDir;$env:PATH"
            }
        }
    }
}
foreach ($tool in @('cmake', 'ninja', 'cl.exe')) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "Missing $tool. Install Visual Studio C++ tools and the CMake/Ninja components, or add $tool to PATH."
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

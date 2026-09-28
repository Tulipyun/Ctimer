param([switch]$SkipTests, [string]$ExecutableName = 'Ctimer.exe')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$version = (Get-Content -Raw -Encoding UTF8 -LiteralPath (Join-Path $projectRoot 'VERSION')).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'VERSION must contain major.minor.patch' }
$toolchainRoot = Join-Path $env:LOCALAPPDATA 'CtimerBuild\llvm-mingw-20260922-ucrt-x86_64'
$compiler = Join-Path $toolchainRoot 'bin\clang++.exe'
if (-not (Test-Path -LiteralPath $compiler)) { & (Join-Path $PSScriptRoot 'setup-toolchain.ps1') }
$resourceCompiler = Join-Path $toolchainRoot 'bin\llvm-windres.exe'
$buildDirectory = Join-Path $projectRoot 'build'
$distributionDirectory = Join-Path $projectRoot 'dist'
New-Item -ItemType Directory -Force -Path $buildDirectory,$distributionDirectory | Out-Null
$generatedDirectory = Join-Path $buildDirectory 'generated'
New-Item -ItemType Directory -Force -Path $generatedDirectory | Out-Null
$versionParts = $version.Split('.')
foreach ($template in @('version.h','app.manifest')) {
    $content = Get-Content -Raw -Encoding UTF8 -LiteralPath (Join-Path $projectRoot "resources/$template.in")
    $content = $content.Replace('@PROJECT_VERSION_MAJOR@',$versionParts[0]).Replace('@PROJECT_VERSION_MINOR@',$versionParts[1]).Replace('@PROJECT_VERSION_PATCH@',$versionParts[2]).Replace('@PROJECT_VERSION@',$version)
    [IO.File]::WriteAllText((Join-Path $generatedDirectory $template), $content, [Text.UTF8Encoding]::new($false))
}
Push-Location $projectRoot
try {
    & (Join-Path $PSScriptRoot 'make-icon.ps1')
    & $resourceCompiler -I build/generated -I resources -i resources/app.rc -O coff -o build/app-res.o
    if ($LASTEXITCODE -ne 0) { throw 'Resource compilation failed' }
    $common = @('-std=c++20','-O2','-DNDEBUG','-DUNICODE','-D_UNICODE','-DWIN32_LEAN_AND_MEAN','-DNOMINMAX','-D_WIN32_WINNT=0x0A00','-Wall','-Wextra','-Isrc','-Ibuild/generated','-Iresources')
    $objects = @()
    foreach ($unit in @('core','estimator','platform','ntp','engine')) {
        $objectFile = "build/$unit.o"
        & $compiler @common -c "src/$unit.cpp" -o $objectFile
        if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $unit" }
        $objects += $objectFile
    }
    & $compiler @common -c src/main.cpp -o build/main.o
    if ($LASTEXITCODE -ne 0) { throw 'GUI compilation failed' }
    & $compiler -static -municode -mwindows @objects build/main.o build/app-res.o -o "dist/$ExecutableName" -lws2_32 -luser32 -lshell32 -lcomctl32 -lgdi32 -lwtsapi32 -lwinmm -ladvapi32
    if ($LASTEXITCODE -ne 0) { throw 'GUI link failed' }
    $builtVersion = (Get-Item -LiteralPath (Join-Path $distributionDirectory $ExecutableName)).VersionInfo
    $expectedName = -join ([char[]]@(0x5b9a,0x65f6,0x70b9,0x51fb,0x5668))
    if ($builtVersion.FileDescription -ne $expectedName -or $builtVersion.ProductVersion -ne $version) { throw 'Application name or version resource verification failed' }
    & $compiler @common -static tests/tests.cpp tests/estimator_tests.cpp @objects -o build/CtimerTests.exe -lws2_32 -luser32 -lshell32 -ladvapi32
    if ($LASTEXITCODE -ne 0) { throw 'Test compilation failed' }
    if (-not (Test-Path -LiteralPath 'dist/Ctimer.ini')) { Copy-Item -LiteralPath 'config/Ctimer.ini' -Destination 'dist/Ctimer.ini' }
    if (-not $SkipTests) {
        & .\build\CtimerTests.exe --app (Join-Path $distributionDirectory $ExecutableName)
        if ($LASTEXITCODE -ne 0) { throw 'Core tests failed' }
    }
    Get-Item (Join-Path $distributionDirectory $ExecutableName) | Select-Object FullName,Length
} finally { Pop-Location }

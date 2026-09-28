$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$formatter = Join-Path $env:LOCALAPPDATA 'CtimerBuild\llvm-mingw-20260922-ucrt-x86_64\bin\clang-format.exe'
if (-not (Test-Path -LiteralPath $formatter)) { throw 'Run scripts/setup-toolchain.ps1 first.' }
$sourceFiles = Get-ChildItem -LiteralPath (Join-Path $projectRoot 'src'),(Join-Path $projectRoot 'tests') -File | Where-Object Extension -In '.cpp','.hpp','.h'
foreach ($sourceFile in $sourceFiles) {
    & $formatter --style=file -i $sourceFile.FullName
    if ($LASTEXITCODE -ne 0) { throw "Formatting failed: $($sourceFile.FullName)" }
}

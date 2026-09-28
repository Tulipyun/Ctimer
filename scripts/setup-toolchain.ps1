$ErrorActionPreference = 'Stop'
$toolchainVersion = '20260922'
$toolchainHash = 'e3ad77d117a4bea19a7a3b333341824d79a5a371004a10e25b8504e7b3047666'
$toolchainCache = Join-Path $env:LOCALAPPDATA 'CtimerBuild'
$toolchainName = "llvm-mingw-$toolchainVersion-ucrt-x86_64"
$toolchainRoot = Join-Path $toolchainCache $toolchainName
if (-not (Test-Path (Join-Path $toolchainRoot 'bin\clang++.exe'))) {
    New-Item -ItemType Directory -Force -Path $toolchainCache | Out-Null
    $toolchainArchive = Join-Path $toolchainCache "$toolchainName.zip"
    if (-not (Test-Path -LiteralPath $toolchainArchive)) {
        $toolchainUrl = "https://github.com/mstorsjo/llvm-mingw/releases/download/$toolchainVersion/$toolchainName.zip"
        Write-Host "Downloading $toolchainUrl"
        Invoke-WebRequest -Uri $toolchainUrl -OutFile $toolchainArchive
    }
    if ((Get-FileHash -LiteralPath $toolchainArchive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $toolchainHash) {
        throw 'Toolchain SHA256 mismatch; archive was not extracted.'
    }
    Write-Host 'SHA256 verified; extracting toolchain...'
    Expand-Archive -LiteralPath $toolchainArchive -DestinationPath $toolchainCache -Force
}
& (Join-Path $toolchainRoot 'bin\clang++.exe') --version
Write-Host "Toolchain: $toolchainRoot"

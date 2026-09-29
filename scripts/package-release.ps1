param([string]$ExecutablePath = 'dist/Ctimer.exe')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$version = (Get-Content -Raw -LiteralPath (Join-Path $projectRoot 'VERSION')).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'Invalid VERSION' }
if (-not [IO.Path]::IsPathRooted($ExecutablePath)) { $ExecutablePath = Join-Path $projectRoot $ExecutablePath }
$binary = Get-Item -LiteralPath $ExecutablePath
if ($binary.VersionInfo.ProductVersion -ne $version) { throw "Binary version does not match VERSION ($version)" }
$bytes = [IO.File]::ReadAllBytes($binary.FullName)
$peOffset = [BitConverter]::ToInt32($bytes, 0x3c)
if ([BitConverter]::ToUInt16($bytes, $peOffset + 4) -ne 0x8664) { throw 'Expected a Windows x64 executable' }
$outputDirectory = Join-Path $projectRoot 'dist'
$packageName = "Ctimer-v$version-win-x64"
$stagingDirectory = Join-Path $projectRoot ("build\packages\" + [Guid]::NewGuid().ToString('N') + "\$packageName")
New-Item -ItemType Directory -Force -Path $stagingDirectory,(Join-Path $stagingDirectory 'docs') | Out-Null
Copy-Item -LiteralPath $binary.FullName -Destination (Join-Path $stagingDirectory 'Ctimer.exe')
Copy-Item -LiteralPath (Join-Path $projectRoot 'config\Ctimer.ini') -Destination (Join-Path $stagingDirectory 'Ctimer.example.ini')
Copy-Item -LiteralPath (Join-Path $projectRoot 'README.md'),(Join-Path $projectRoot 'CHANGELOG.md'),(Join-Path $projectRoot 'THIRD_PARTY_NOTICES.md') -Destination $stagingDirectory
Copy-Item -LiteralPath (Join-Path $projectRoot 'docs\USAGE.md'),(Join-Path $projectRoot 'docs\BUILDING.md') -Destination (Join-Path $stagingDirectory 'docs')
Copy-Item -LiteralPath (Join-Path $projectRoot 'docs\v0.6-同步升级与测试.md') -Destination (Join-Path $stagingDirectory 'docs')
Copy-Item -LiteralPath (Join-Path $projectRoot 'docs\v0.6.1-时钟显示优化.md') -Destination (Join-Path $stagingDirectory 'docs')
Copy-Item -LiteralPath (Join-Path $projectRoot 'licenses') -Destination $stagingDirectory -Recurse
$releaseBinary = Join-Path $outputDirectory "Ctimer-v$version.exe"
if (-not [string]::Equals($binary.FullName, $releaseBinary, [StringComparison]::OrdinalIgnoreCase)) { Copy-Item -LiteralPath $binary.FullName -Destination $releaseBinary -Force }
$archive = Join-Path $outputDirectory "$packageName.zip"
Compress-Archive -LiteralPath $stagingDirectory -DestinationPath $archive -Force
$checksumLines = foreach ($file in @($releaseBinary,$archive)) {
    (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() + '  ' + [IO.Path]::GetFileName($file)
}
[IO.File]::WriteAllLines((Join-Path $outputDirectory "SHA256SUMS-v$version.txt"), $checksumLines, [Text.UTF8Encoding]::new($false))
Get-Item -LiteralPath $releaseBinary,$archive | Select-Object Name,Length

param(
    [Parameter(Mandatory = $true)]
    [string]$CaBundle
)
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$version = (Get-Content -LiteralPath (Join-Path $projectRoot 'VERSION') -Raw).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'Invalid release version.' }
$outputDirectory = Join-Path $projectRoot ('dist/v' + $version)
[IO.Directory]::CreateDirectory($outputDirectory) | Out-Null
$dol = Join-Path $projectRoot 'wiimc.dol'
$ca = (Resolve-Path -LiteralPath $CaBundle).Path
if (-not (Test-Path -LiteralPath $dol)) { throw 'Build wiimc.dol before packaging.' }
if (-not (Select-String -LiteralPath $ca -SimpleMatch '-----BEGIN CERTIFICATE-----' -Quiet)) {
    throw 'The CA bundle does not contain certificates.'
}

# The public archive is an explicit allowlist. Never package the prepared SD:
# it contains the owner's WebDAV credentials and private TLS seed.
$files = [ordered]@{
    'wiimc.dol' = $dol
    'VERSION' = (Join-Path $projectRoot 'VERSION')
    'README.md' = (Join-Path $projectRoot 'README.md')
    'CHANGELOG.md' = (Join-Path $projectRoot 'CHANGELOG.md')
    'HARDWARE_TEST.md' = (Join-Path $projectRoot 'HARDWARE_TEST.md')
    'PORTING.md' = (Join-Path $projectRoot 'PORTING.md')
    'LICENSE' = (Join-Path $projectRoot 'LICENSE')
    'LICENSE-mbedtls' = (Join-Path $projectRoot 'vendor/prefix/LICENSE-mbedtls')
    'apps/wiimc/ca.pem' = $ca
    'apps/wiimc/onlinemedia.xml' = (Join-Path $projectRoot 'examples/onlinemedia.xml')
    'apps/wiimc/webdav.conf.example' = (Join-Path $projectRoot 'examples/webdav.conf.example')
    'examples/onlinemedia.xml' = (Join-Path $projectRoot 'examples/onlinemedia.xml')
    'examples/webdav.conf.example' = (Join-Path $projectRoot 'examples/webdav.conf.example')
    'tools/prepare-sd.ps1' = (Join-Path $projectRoot 'tools/prepare-sd.ps1')
}
foreach ($source in $files.Values) {
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing release file: $source" }
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zipPath = Join-Path $outputDirectory ('WiiMC-GCN-Online-' + $version + '.zip')
$archive = [IO.Compression.ZipFile]::Open($zipPath + '.tmp', [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($entry in $files.GetEnumerator()) {
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $archive, $entry.Value, $entry.Key, [IO.Compression.CompressionLevel]::Optimal
        ) | Out-Null
    }
} finally { $archive.Dispose() }
Move-Item -LiteralPath ($zipPath + '.tmp') -Destination $zipPath -Force
Copy-Item -LiteralPath $dol -Destination (Join-Path $outputDirectory 'wiimc.dol') -Force
$checksums = foreach ($asset in @('wiimc.dol', [IO.Path]::GetFileName($zipPath))) {
    $hash = (Get-FileHash -LiteralPath (Join-Path $outputDirectory $asset) -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $asset"
}
[IO.File]::WriteAllLines((Join-Path $outputDirectory 'SHA256SUMS.txt'), $checksums, [Text.Encoding]::ASCII)
Get-ChildItem -LiteralPath $outputDirectory | Select-Object Name, Length

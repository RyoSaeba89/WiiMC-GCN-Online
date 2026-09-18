param(
    [string]$Destination = (Join-Path $PSScriptRoot '../sd-package'),
    [string]$CaBundle,
    [string]$WebDavConfig,
    [switch]$RefreshCA
)
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$targetRoot = [IO.Path]::GetFullPath($Destination)
$appDirectory = Join-Path $targetRoot 'apps/wiimc'
$dolPath = Join-Path $projectRoot 'wiimc.dol'
if (-not (Test-Path -LiteralPath $dolPath)) { throw 'Build wiimc.dol first with tools/build-gc.sh.' }
[IO.Directory]::CreateDirectory($appDirectory) | Out-Null
$caTarget = Join-Path $appDirectory 'ca.pem'
if ($CaBundle) {
    Copy-Item -LiteralPath $CaBundle -Destination $caTarget -Force
} elseif ($RefreshCA -or -not (Test-Path -LiteralPath $caTarget)) {
    # The CA bundle is Mozilla's trust store, converted by curl (MPL 2.0).
    $download = Join-Path $appDirectory 'ca.pem.download'
    Invoke-WebRequest -Uri 'https://curl.se/ca/cacert.pem' -OutFile $download
    $checksumResponse = Invoke-WebRequest -Uri 'https://curl.se/ca/cacert.pem.sha256'
    $checksumText = if ($checksumResponse.Content -is [byte[]]) {
        [Text.Encoding]::ASCII.GetString($checksumResponse.Content)
    } else {
        [string]$checksumResponse.Content
    }
    $expected = ($checksumText -split '\s+')[0]
    $actual = (Get-FileHash -LiteralPath $download -Algorithm SHA256).Hash
    if ($expected -notmatch '^[0-9a-fA-F]{64}$' -or $actual -ne $expected) { throw 'CA bundle checksum mismatch.' }
    Move-Item -LiteralPath $download -Destination $caTarget -Force
}
$seedPath = Join-Path $appDirectory 'tls-seed.bin'
if (-not (Test-Path -LiteralPath $seedPath)) {
    $seedBytes = [byte[]]::new(64)
    $generator = [Security.Cryptography.RandomNumberGenerator]::Create()
    try { $generator.GetBytes($seedBytes); [IO.File]::WriteAllBytes($seedPath, $seedBytes) }
    finally { $generator.Dispose(); [Array]::Clear($seedBytes, 0, $seedBytes.Length) }
}
$radioTarget = Join-Path $appDirectory 'onlinemedia.xml'
Copy-Item -LiteralPath (Join-Path $projectRoot 'examples/onlinemedia.xml') -Destination $radioTarget -Force
$webDavExampleTarget = Join-Path $appDirectory 'webdav.conf.example'
if (-not (Test-Path -LiteralPath $webDavExampleTarget)) {
    Copy-Item -LiteralPath (Join-Path $projectRoot 'examples/webdav.conf.example') -Destination $webDavExampleTarget
}
$privateWebDav = if ($WebDavConfig) {
    [IO.Path]::GetFullPath($WebDavConfig)
} else {
    Join-Path $projectRoot 'private/webdav.conf'
}
if (Test-Path -LiteralPath $privateWebDav) {
    Copy-Item -LiteralPath $privateWebDav -Destination (Join-Path $appDirectory 'webdav.conf') -Force
} elseif ($WebDavConfig) {
    throw "WebDAV configuration not found: $privateWebDav"
}
# Keep a recoverable copy of the previous binary before replacing it.
$cardDol = Join-Path $targetRoot 'wiimc.dol'
if (Test-Path -LiteralPath $cardDol) {
    $oldHash = (Get-FileHash -LiteralPath $cardDol -Algorithm SHA256).Hash
    if ($oldHash -ne (Get-FileHash -LiteralPath $dolPath -Algorithm SHA256).Hash) {
        $backup = Join-Path $targetRoot ('wiimc-before-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.dol')
        Copy-Item -LiteralPath $cardDol -Destination $backup
    }
}
Copy-Item -LiteralPath $dolPath -Destination $cardDol -Force
# Archive the matching ELF and incoming hardware logs inside the project.
$strings = Join-Path $projectRoot 'logs/build-online.log'
$elfPath = Join-Path $projectRoot 'wiimc.elf'
if (Test-Path -LiteralPath $elfPath) {
    $elfBytes = [IO.File]::ReadAllBytes($elfPath)
    $elfText = [Text.Encoding]::ASCII.GetString($elfBytes)
    $buildMatch = [regex]::Match($elfText, 'build   (\d{8}-\d{6})')
    if ($buildMatch.Success) {
        $archive = Join-Path $projectRoot 'deployed'
        [IO.Directory]::CreateDirectory($archive) | Out-Null
        Copy-Item -LiteralPath $elfPath -Destination (Join-Path $archive ('wiimc-' + $buildMatch.Groups[1].Value + '.elf')) -Force
    }
}
foreach ($name in @('wiimc.log','wiimc-prev.log','wiimc-crash.txt')) {
    $sourceLog = Join-Path $targetRoot $name
    if (Test-Path -LiteralPath $sourceLog) {
        [IO.Directory]::CreateDirectory((Join-Path $projectRoot 'logs')) | Out-Null
        $logHash = (Get-FileHash -LiteralPath $sourceLog -Algorithm SHA256).Hash.Substring(0,12)
        $logArchive = Join-Path $projectRoot ('logs/sd-' + $logHash + '-' + $name)
        Copy-Item -LiteralPath $sourceLog -Destination $logArchive -Force
    }
}
Get-Item -LiteralPath $cardDol, $caTarget, $seedPath | Select-Object FullName, Length
if (Test-Path -LiteralPath (Join-Path $appDirectory 'webdav.conf')) {
    Write-Output 'Radio list and private WebDAV configuration installed.'
} else {
    Write-Output 'Radio list installed. Configure webdav.conf to enable the WebDAV device.'
}

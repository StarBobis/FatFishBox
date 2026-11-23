[CmdletBinding()]
param(
    [string]$BuildDirectory = 'build'
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not [IO.Path]::IsPathRooted($BuildDirectory)) {
    $BuildDirectory = Join-Path $projectRoot $BuildDirectory
}
$buildPath = (Resolve-Path -LiteralPath $BuildDirectory).Path
$source = [IO.File]::ReadAllText((Join-Path $projectRoot 'CMakeLists.txt'))
$versionMatch = [regex]::Match($source, 'project\(FatFishBox\s+VERSION\s+(\d+\.\d+\.\d+)\b')
if (-not $versionMatch.Success) { throw 'Cannot read the three-part project version.' }
$version = $versionMatch.Groups[1].Value
$cache = [IO.File]::ReadAllText((Join-Path $buildPath 'CMakeCache.txt'))
$cacheVersion = [regex]::Match($cache, '(?m)^CMAKE_PROJECT_VERSION:STATIC=([^\r\n]+)')
if (-not $cacheVersion.Success -or $cacheVersion.Groups[1].Value -ne $version) {
    throw 'Build configuration is outdated. Build the current Release version before packaging.'
}
$cmakeMatch = [regex]::Match($cache, '(?m)^CMAKE_COMMAND:INTERNAL=([^\r\n]+)')
if (-not $cmakeMatch.Success -or -not (Test-Path -LiteralPath $cmakeMatch.Groups[1].Value -PathType Leaf)) {
    throw 'Cannot locate the CMake executable from the build cache.'
}
$releaseDirectory = Join-Path $buildPath 'releases'
New-Item -ItemType Directory -Path $releaseDirectory -Force | Out-Null

# Use a new install tree for every attempt; never package the live directory.
$packageRoot = Join-Path $buildPath ('share-package-' + $version + '-' + [guid]::NewGuid().ToString('N'))
$packageDirectory = Join-Path $packageRoot 'FatFishBox'
New-Item -ItemType Directory -Path $packageDirectory -Force | Out-Null
& $cmakeMatch.Groups[1].Value --install $buildPath --config Release --prefix $packageDirectory
if ($LASTEXITCODE -ne 0) { throw 'Release staging installation failed.' }
$executable = Join-Path $packageDirectory 'FatFishBox.exe'
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw 'Staged Release EXE is missing.' }
$executableHash = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash
$archivePath = Join-Path $releaseDirectory "FatFishBox-v$version-win-x64.zip"
$temporaryArchive = Join-Path $packageRoot ('package-' + [guid]::NewGuid().ToString('N') + '.zip')
Compress-Archive -LiteralPath $packageDirectory -DestinationPath $temporaryArchive -CompressionLevel Optimal

Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($temporaryArchive)
try {
    $entry = $archive.GetEntry('FatFishBox/FatFishBox.exe')
    if ($null -eq $entry) { throw 'ZIP does not contain the Release EXE at the expected path.' }
    foreach ($file in $archive.Entries) {
        if ($file.FullName -match '(?i)(^|/)(logs|evidence|team-workspaces)/|\.(log|dmp)$') {
            throw "Unexpected runtime data in package: $($file.FullName)"
        }
    }
    $stream = $entry.Open()
    $hasher = [Security.Cryptography.SHA256]::Create()
    try { $archivedHash = [BitConverter]::ToString($hasher.ComputeHash($stream)).Replace('-', '') }
    finally { $hasher.Dispose(); $stream.Dispose() }
    if ($archivedHash -ne $executableHash) { throw 'Archived EXE does not match the staged Release EXE.' }
}
finally { $archive.Dispose() }

# Only replace this exact version's archive; retain all previous versions.
Move-Item -LiteralPath $temporaryArchive -Destination $archivePath -Force
[pscustomobject]@{
    Version = $version
    Archive = $archivePath
    ArchiveSHA256 = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash
    ExecutableSHA256 = $executableHash
    Bytes = (Get-Item -LiteralPath $archivePath).Length
}

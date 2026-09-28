#Requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$AgentExe,
    [Parameter(Mandatory)][string]$DependencyRoot,
    [Parameter(Mandatory)][string]$RuviaRoot,
    [Parameter(Mandatory)][string]$VcpkgRoot,
    [Parameter(Mandatory)][string]$Output
)
$ErrorActionPreference = 'Stop'
$tar = Join-Path $env:SystemRoot 'System32\tar.exe'
if (-not (Test-Path $tar)) { throw 'Windows tar.exe is required for source packaging' }
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
$version = (& $AgentExe --version).Trim()
$product = ([xml](Get-Content (Join-Path $PSScriptRoot 'agent.wxs') -Raw)).Wix.Product
if ($version -ne [string]$product.Version) { throw 'Source and binary versions differ' }
& git -C $repository diff --quiet HEAD
if ($LASTEXITCODE -ne 0) { throw 'Refusing to publish sources from a modified working tree' }
$untracked = @(& git -C $repository ls-files --others --exclude-standard)
if ($LASTEXITCODE -ne 0 -or $untracked.Count) { throw 'Refusing to omit untracked project sources' }
$commit = (& git -C $repository rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot identify project commit' }
$baseline = (Get-Content (Join-Path $repository 'vcpkg.json') -Raw | ConvertFrom-Json).'builtin-baseline'
$ruvia = (& git -C $RuviaRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or -not (Get-Content (Join-Path $repository 'CMakeLists.txt') -Raw).Contains($ruvia)) { throw 'Ruvia source does not match the project pin' }
& git -C $RuviaRoot diff --quiet HEAD
if ($LASTEXITCODE -ne 0) { throw 'Ruvia source has uncommitted changes' }
$stage = Join-Path $repository ('build\agent-sources-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path (Join-Path $stage 'dependencies'),(Join-Path $stage 'metadata') -Force | Out-Null
$resolvedOutput = [IO.Path]::GetFullPath($Output)
New-Item -ItemType Directory -Path (Split-Path -Parent $resolvedOutput) -Force | Out-Null
try {
    foreach ($snapshot in @(
        @{ Root=$repository; Ref=$commit; Name='project' },
        @{ Root=$RuviaRoot; Ref=$ruvia; Name='ruvia' },
        @{ Root=$VcpkgRoot; Ref=$baseline; Name='vcpkg' }
    )) {
        & git -C $snapshot.Root archive --format=tar ('--prefix=' + $snapshot.Name + '/') ('--output=' + (Join-Path $stage ($snapshot.Name + '.tar'))) $snapshot.Ref
        if ($LASTEXITCODE -ne 0) { throw ('Cannot archive ' + $snapshot.Name) }
    }
    $resources = @()
    foreach ($package in Get-ChildItem (Join-Path $DependencyRoot 'share') -Directory) {
        $resourceFile = Join-Path $package.FullName 'vcpkg-spdx-resources.json'
        $metadata = Join-Path $stage ('metadata\' + $package.Name)
        New-Item -ItemType Directory -Path $metadata -Force | Out-Null
        foreach ($name in @('copyright','vcpkg.spdx.json','vcpkg-spdx-resources.json')) {
            $file = Join-Path $package.FullName $name
            if (Test-Path $file) { Copy-Item $file $metadata }
        }
        if (-not (Test-Path $resourceFile)) { continue }
        foreach ($resource in (Get-Content $resourceFile -Raw | ConvertFrom-Json).packages) {
            $location = [string]$resource.downloadLocation
            $checksum = @($resource.checksums | Where-Object { $_.algorithm -eq 'SHA512' })
            if ($checksum.Count -ne 1) { throw ('Missing source checksum: ' + $package.Name) }
            if ($location -match '^git\+https://github\.com/([^/]+)/([^@]+)@(.+)$') {
                $owner, $name, $ref = $Matches[1], $Matches[2], $Matches[3]
                $url = "https://github.com/$owner/$name/archive/$ref.tar.gz"
                $filename = "$owner-$name-$ref.tar.gz" -replace '[/:]', '_'
            } elseif ($location -match '^git\+(https://[^/]+/([^@]+))@(.+)$') {
                $repo, $path, $ref = $Matches[1], $Matches[2], $Matches[3]
                $name = ($path -split '/')[-1]
                $url = "$repo/-/archive/$ref/$name-$ref.tar.gz"
                $filename = (($path -replace '/', '-') + "-$ref.tar.gz") -replace '[/:]', '_'
            } elseif ($location -match '^https://') {
                $url = $location
                $filename = [IO.Path]::GetFileName(([Uri]$location).AbsolutePath)
            } else { throw ('Unsupported source location: ' + $package.Name) }
            $destination = Join-Path $stage ('dependencies\' + $filename)
            if (-not (Test-Path $destination)) {
                $cached = Join-Path $VcpkgRoot ('downloads\' + $filename)
                if ((Test-Path $cached) -and (Get-FileHash $cached -Algorithm SHA512).Hash -eq $checksum[0].checksumValue) {
                    Copy-Item $cached $destination
                } else {
                    # The GNU mirror selector may redirect HTTPS to an HTTP-only
                    # mirror. Use the canonical HTTPS origin, never weaken TLS.
                    if ($url.StartsWith('https://ftpmirror.gnu.org/gnu/')) {
                        $url = $url.Replace('https://ftpmirror.gnu.org/gnu/', 'https://ftp.gnu.org/gnu/')
                    }
                    Write-Output ('Downloading checked source: ' + $package.Name + '/' + $filename)
                    Invoke-WebRequest -Uri $url -OutFile $destination
                }
            }
            if ((Get-FileHash $destination -Algorithm SHA512).Hash -ne $checksum[0].checksumValue) { throw ('Source checksum mismatch: ' + $filename) }
            $resources += @{ package=$package.Name; file=$filename; origin=$location; sha512=$checksum[0].checksumValue }
        }
    }
    if (-not ($resources | Where-Object { $_.package -eq 'x264' })) { throw 'x264 corresponding source missing' }
    $manifest = @{ version=$version; project_commit=$commit; ruvia_commit=$ruvia; vcpkg_baseline=$baseline; agent_sha256=(Get-FileHash $AgentExe -Algorithm SHA256).Hash.ToLowerInvariant(); resources=$resources }
    $json = $manifest | ConvertTo-Json -Depth 8
    [IO.File]::WriteAllText((Join-Path $stage 'SOURCE_MANIFEST.json'), $json, (New-Object System.Text.UTF8Encoding($false)))
    @'
# Windows Agent corresponding source

Extract project.tar, ruvia.tar and vcpkg.tar. The project includes its CMake presets,
manifest, overlay patches, protocol schemas, tests and Windows packaging scripts.
Dependencies contains the original third-party source archives; metadata records
versions, original locations, licenses and cryptographic checksums. The vcpkg
snapshot contains the build recipes/patches used with those archives. General
purpose development tools (MSVC, CMake, NASM, LLVM, WiX) are not bundled here.

Follow project/README.md for the Windows static build. A normal build uses a git
checkout of the recorded vcpkg baseline (https://github.com/microsoft/vcpkg) and
VCPKG_ROOT pointing to it. Copy dependencies/* into its downloads directory to
reuse these checked archives. Configure with -DFETCHCONTENT_SOURCE_DIR_RUVIA
pointing to the extracted ruvia directory. Required tools may still be downloaded.
The vcpkg snapshot is provided as source as well; it is not a replacement for the
git history needed by vcpkg's versioned registry. No Agent/device credentials are
needed for building. The Agent remains GPL-3.0-or-later; see project/agent/LICENSE.md
and COPYING. Third-party code retains its original licenses.
'@ | Set-Content (Join-Path $stage 'README.md') -Encoding UTF8
    & $tar -czf $resolvedOutput -C $stage .
    if ($LASTEXITCODE -ne 0) { throw 'Cannot create source archive' }
    [IO.File]::WriteAllText(($resolvedOutput + '.json'), $json, (New-Object System.Text.UTF8Encoding($false)))
    Write-Output "Corresponding source: $resolvedOutput"
    Get-FileHash $resolvedOutput -Algorithm SHA256 | Select-Object Hash,Path
} finally {
    Remove-Item $stage -Recurse -Force
}

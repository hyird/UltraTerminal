#Requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$AgentExe,
    [Parameter(Mandatory)][string]$LoginDll,
    [Parameter(Mandatory)][string]$CaBundle,
    [Parameter(Mandatory)][string]$Output,
    [Parameter(Mandatory)][string]$DependencyRoot,
    [Parameter(Mandatory)][string]$RuviaRoot,
    [Parameter(Mandatory)][ValidatePattern('^[a-zA-Z0-9.-]+$')][string]$GatewayHost,
    [Parameter(Mandatory)][ValidatePattern('^[a-zA-Z0-9.-]+$')][string]$GatewayAddress,
    [ValidateRange(1,65535)][int]$GatewayPort = 7443
)
$ErrorActionPreference = 'Stop'
$product = ([xml](Get-Content -LiteralPath (Join-Path $PSScriptRoot 'agent.wxs') -Raw)).Wix.Package
if ((& $AgentExe --version).Trim() -ne [string]$product.Version) { throw 'Agent binary and MSI version differ' }
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
& git -C $repository diff --quiet HEAD
if ($LASTEXITCODE -ne 0) { throw 'Refusing to package an uncommitted source revision' }
$untracked = @(& git -C $repository ls-files --others --exclude-standard)
if ($LASTEXITCODE -ne 0 -or $untracked.Count) { throw 'Refusing to omit untracked project sources' }
$commit = (& git -C $repository rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $commit -notmatch '^[a-f0-9]{40}$') { throw 'Cannot identify project commit' }
$ruvia = (& git -C $RuviaRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or -not (Get-Content (Join-Path $repository 'CMakeLists.txt') -Raw).Contains($ruvia)) { throw 'Ruvia source does not match the project pin' }
& git -C $RuviaRoot diff --quiet HEAD
if ($LASTEXITCODE -ne 0) { throw 'Ruvia source has uncommitted changes' }
$baseline = (Get-Content (Join-Path $repository 'vcpkg.json') -Raw | ConvertFrom-Json).'builtin-baseline'
$sourceUrl = "https://github.com/hyird/UltraTerminal/tree/$commit"
$toolRoot = Join-Path $repository 'build\tools'
$wix = Join-Path $toolRoot 'wix7'
$wixExe = Join-Path $wix 'wix.exe'
if (-not (Test-Path -LiteralPath $wixExe)) {
    & dotnet tool install wix --version 7.0.1 --tool-path $wix
    if ($LASTEXITCODE -ne 0) { throw 'WiX 7.0.1 installation failed' }
}
if ((& $wixExe --version) -notmatch '^7\.0\.1(?:\+|$)') { throw 'WiX version does not match the pin' }
$stage = Join-Path $repository 'build\agent-msi'
$payload = Join-Path $stage 'payload'
New-Item -ItemType Directory -Path $payload -Force | Out-Null
Copy-Item -LiteralPath $AgentExe -Destination (Join-Path $payload 'agent.exe') -Force
Copy-Item -LiteralPath $LoginDll -Destination (Join-Path $payload 'ultraterimal_login.dll') -Force
Copy-Item -LiteralPath $CaBundle -Destination (Join-Path $payload 'ca.pem') -Force
Copy-Item -LiteralPath (Join-Path $repository 'agent\COPYING') -Destination $payload -Force
Copy-Item -LiteralPath (Join-Path $repository 'agent\LICENSE.md') -Destination (Join-Path $payload 'agent_license.md') -Force
$notices = New-Object System.Text.StringBuilder
$sourceResources = @()
[void]$notices.AppendLine('Ruvia (MIT)')
[void]$notices.AppendLine((Get-Content (Join-Path $RuviaRoot 'LICENSE') -Raw))
foreach ($directory in Get-ChildItem (Join-Path $DependencyRoot 'share') -Directory | Sort-Object Name) {
    $resourceFile = Join-Path $directory.FullName 'vcpkg-spdx-resources.json'
    if (Test-Path -LiteralPath $resourceFile) {
        $sourceResources += @{ package=$directory.Name; resources=(Get-Content -LiteralPath $resourceFile -Raw | ConvertFrom-Json).packages }
    }
    $copyright = Join-Path $directory.FullName 'copyright'
    if (Test-Path $copyright) {
        [void]$notices.AppendLine(("`n===== " + $directory.Name + " =====`n"))
        [void]$notices.AppendLine((Get-Content $copyright -Raw))
    }
}
[IO.File]::WriteAllText((Join-Path $payload 'third_party_notices.txt'), $notices.ToString(), (New-Object System.Text.UTF8Encoding($false)))
if (-not ($sourceResources | Where-Object { $_.package -eq 'openh264' -and $_.resources })) { throw 'OpenH264 source metadata missing' }
$sourceMetadata = @{ project_commit=$commit; ruvia_commit=$ruvia; vcpkg_baseline=$baseline; agent_sha256=(Get-FileHash $AgentExe -Algorithm SHA256).Hash.ToLowerInvariant(); dependencies=$sourceResources } | ConvertTo-Json -Depth 12
$sourceNotice = "Ultraterimal Agent $($product.Version) - GPL-3.0-or-later`nProject source and build instructions: $sourceUrl`nProject download: https://github.com/hyird/UltraTerminal/archive/$commit.tar.gz`nRuvia source location and exact revision: $sourceUrl/CMakeLists.txt`nDependency recipes: https://github.com/microsoft/vcpkg/tree/$baseline`nDependency source locations and checksums (SPDX):`n$sourceMetadata`n"
[IO.File]::WriteAllText((Join-Path $payload 'source_info.txt'), $sourceNotice, (New-Object System.Text.UTF8Encoding($false)))
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'install_agent.ps1') -Destination (Join-Path $payload 'install_agent.ps1') -Force
$resolvedOutput = [IO.Path]::GetFullPath($Output)
New-Item -ItemType Directory -Path (Split-Path -Parent $resolvedOutput) -Force | Out-Null
& $wixExe build -acceptEula wix7 -arch x64 -d "Payload=$payload" -out $resolvedOutput (Join-Path $PSScriptRoot 'agent.wxs')
if ($LASTEXITCODE -ne 0) { throw 'WiX build failed; WiX 7 requires the build owner to accept its OSMF EULA' }
$outputDirectory = Split-Path -Parent $resolvedOutput
$manifest = @{ source = $sourceUrl; source_commit = $commit; version = [string]$product.Version; file = '/assets/' + [IO.Path]::GetFileName($resolvedOutput); sha256 = (Get-FileHash -LiteralPath $resolvedOutput -Algorithm SHA256).Hash.ToLowerInvariant(); native_host = $GatewayHost; native_address = $GatewayAddress; native_port = $GatewayPort }
[IO.File]::WriteAllText((Join-Path $outputDirectory 'windows-agent.json'), ($manifest | ConvertTo-Json -Compress), (New-Object System.Text.UTF8Encoding($false)))
Write-Output "MSI: $resolvedOutput"
Write-Output "SHA256: $((Get-FileHash -LiteralPath $resolvedOutput -Algorithm SHA256).Hash)"

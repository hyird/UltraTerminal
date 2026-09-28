#Requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$AgentExe,
    [Parameter(Mandatory)][string]$LoginDll,
    [Parameter(Mandatory)][string]$CaBundle,
    [Parameter(Mandatory)][string]$Output,
    [Parameter(Mandatory)][ValidatePattern('^[a-zA-Z0-9.-]+$')][string]$GatewayHost,
    [Parameter(Mandatory)][ValidatePattern('^[a-zA-Z0-9.-]+$')][string]$GatewayAddress,
    [ValidateRange(1,65535)][int]$GatewayPort = 7443
)
$ErrorActionPreference = 'Stop'
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
$toolRoot = Join-Path $repository 'build\tools'
$archive = Join-Path $toolRoot 'wix3141-binaries.zip'
$wix = Join-Path $toolRoot 'wix3141'
$url = 'https://github.com/wixtoolset/wix3/releases/download/wix3141rtm/wix314-binaries.zip'
$sha256 = '6AC824E1642D6F7277D0ED7EA09411A508F6116BA6FAE0AA5F2C7DAA2FF43D31'
New-Item -ItemType Directory -Path $toolRoot -Force | Out-Null
if (-not (Test-Path -LiteralPath $archive)) { Invoke-WebRequest -Uri $url -OutFile $archive }
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $sha256) { throw 'WiX release archive checksum mismatch' }
if (-not (Test-Path -LiteralPath (Join-Path $wix 'candle.exe'))) {
    New-Item -ItemType Directory -Path $wix -Force | Out-Null
    Expand-Archive -LiteralPath $archive -DestinationPath $wix -Force
}
$stage = Join-Path $repository 'build\agent-msi'
$payload = Join-Path $stage 'payload'
New-Item -ItemType Directory -Path $payload -Force | Out-Null
Copy-Item -LiteralPath $AgentExe -Destination (Join-Path $payload 'agent.exe') -Force
Copy-Item -LiteralPath $LoginDll -Destination (Join-Path $payload 'ultraterimal_login.dll') -Force
Copy-Item -LiteralPath $CaBundle -Destination (Join-Path $payload 'ca.pem') -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'install_agent.ps1') -Destination (Join-Path $payload 'install_agent.ps1') -Force
$object = Join-Path $stage 'agent.wixobj'
$resolvedOutput = [IO.Path]::GetFullPath($Output)
New-Item -ItemType Directory -Path (Split-Path -Parent $resolvedOutput) -Force | Out-Null
& (Join-Path $wix 'candle.exe') -nologo -arch x64 "-dPayload=$payload" -out $object (Join-Path $PSScriptRoot 'agent.wxs')
if ($LASTEXITCODE -ne 0) { throw 'WiX candle failed' }
& (Join-Path $wix 'light.exe') -nologo -out $resolvedOutput $object
if ($LASTEXITCODE -ne 0) { throw 'WiX light failed' }
$outputDirectory = Split-Path -Parent $resolvedOutput
$manifest = @{ file = '/assets/' + [IO.Path]::GetFileName($resolvedOutput); sha256 = (Get-FileHash -LiteralPath $resolvedOutput -Algorithm SHA256).Hash; native_host = $GatewayHost; native_address = $GatewayAddress; native_port = $GatewayPort }
[IO.File]::WriteAllText((Join-Path $outputDirectory 'windows-agent.json'), ($manifest | ConvertTo-Json -Compress), (New-Object System.Text.UTF8Encoding($false)))
Write-Output "MSI: $resolvedOutput"
Write-Output "SHA256: $((Get-FileHash -LiteralPath $resolvedOutput -Algorithm SHA256).Hash)"

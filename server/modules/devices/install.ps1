# The device command supplies a single-use enrollment capability in the current
# PowerShell scope. Pass it to the installed script via stdin, never process args.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Open PowerShell as administrator and run the installation command again.' }
if (-not [Environment]::Is64BitOperatingSystem -or $env:PROCESSOR_ARCHITECTURE -ne 'AMD64') { throw 'This installer requires Windows x64 PowerShell.' }
if ($utOrigin -cnotmatch '^https://[a-zA-Z0-9.-]+(:[0-9]{1,5})?$' -or $utDevice -cnotmatch '^d-[0-9a-f]{64}$' -or ($utCode -ne '' -and $utCode -cnotmatch '^[0-9a-f]{64}$')) { throw 'Invalid installation command. Copy it again from device management.' }
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$manifest = Invoke-RestMethod -Uri ($utOrigin + '/windows-agent.json')
if ($manifest.file -cnotmatch '^/assets/ultraterimal-agent-[0-9]+\.[0-9]+\.[0-9]+\.msi$' -or $manifest.sha256 -cnotmatch '^[0-9A-Fa-f]{64}$') { throw 'Invalid installer manifest' }
if ($manifest.native_host -cnotmatch '^[a-zA-Z0-9.-]+$' -or $manifest.native_address -cnotmatch '^[a-zA-Z0-9.-]+$' -or [int]$manifest.native_port -lt 1 -or [int]$manifest.native_port -gt 65535) { throw 'Invalid Agent endpoint' }
$root = Join-Path $env:ProgramFiles 'Ultraterimal'
$existingConfig = Join-Path $root 'agent.json'
if (Test-Path -LiteralPath $existingConfig) {
    $existing = Get-Content -LiteralPath $existingConfig -Raw | ConvertFrom-Json
    if ($existing.device_id -cne $utDevice) { throw 'This computer is bound to another device. Copy the command from that device instead.' }
} elseif (-not $utCode) { throw 'This command repairs an enrolled device. Use the original computer or add a new device.' }
$downloadDirectory = Join-Path $env:TEMP ('ultraterimal-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $downloadDirectory | Out-Null
# Windows Installer remembers the original package name for maintenance mode.
$msi = Join-Path $downloadDirectory ([IO.Path]::GetFileName($manifest.file))
$log = Join-Path $env:TEMP ('ultraterimal-install-' + [guid]::NewGuid().ToString('N') + '.log')
try {
    Write-Host 'Downloading Ultraterimal Agent...'
    Invoke-WebRequest -UseBasicParsing -Uri ($utOrigin + $manifest.file) -OutFile $msi
    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        $stream = [IO.File]::OpenRead($msi)
        try { $downloadHash = ([BitConverter]::ToString($sha256.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
        finally { $stream.Dispose() }
    } finally { $sha256.Dispose() }
    if ($downloadHash -ne $manifest.sha256.ToLowerInvariant()) { throw 'MSI checksum mismatch' }
    Write-Host 'Installing Windows service...'
    $process = Start-Process -FilePath (Join-Path $env:SystemRoot 'System32\msiexec.exe') -ArgumentList @('/i', ('"' + $msi + '"'), '/qn', '/norestart', '/l*v', ('"' + $log + '"')) -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode -notin @(0,3010)) { throw "MSI installation failed: $($process.ExitCode). Log: $log" }
    $utCode | & (Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe') -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'install_agent.ps1') -MsiInstalled -EnrollmentFromStdin -ExpectedDeviceId $utDevice -GatewayHost $manifest.native_host -GatewayAddress $manifest.native_address -GatewayPort $manifest.native_port
    if ($LASTEXITCODE -ne 0) { throw 'Agent setup failed. Copy a new command from device management and retry.' }
    Write-Host 'Ultraterimal Agent is installed and running.'
    if ($process.ExitCode -eq 3010) { Write-Host 'Windows requires a restart to finish replacing files.' }
} finally {
    $utCode = $null
    Remove-Item -LiteralPath $msi -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $downloadDirectory -ErrorAction SilentlyContinue
}

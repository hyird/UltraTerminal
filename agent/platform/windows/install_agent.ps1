#Requires -RunAsAdministrator
[CmdletBinding()]
param([Parameter(Mandatory)][string]$GatewayHost, [string]$GatewayAddress = '', [ValidateRange(1,65535)][int]$GatewayPort = 7443, [switch]$MsiInstalled, [switch]$EnrollmentFromStdin, [string]$ExpectedDeviceId = '')
$ErrorActionPreference = 'Stop'
$installDirectory = Join-Path $env:ProgramFiles 'Ultraterimal'
if ($MsiInstalled) {
    if ([IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\') -cne [IO.Path]::GetFullPath($installDirectory).TrimEnd('\')) { throw 'Run the installer script from the MSI installation directory.' }
    $service = Get-CimInstance Win32_Service -Filter "Name='UltraterimalAgent'"
    $expectedExecutable = Join-Path $installDirectory 'agent.exe'
    $actualCommand = if ($service) { $service.PathName.Trim() } else { '' }
    if ($actualCommand -cne ('"' + $expectedExecutable + '" --service') -and $actualCommand -cne ($expectedExecutable + ' --service')) { throw 'The MSI Agent service is missing or belongs to another installation.' }
    $hasConfig = Test-Path -LiteralPath (Join-Path $installDirectory 'agent.json')
    $hasCredential = Test-Path -LiteralPath (Join-Path $installDirectory 'agent.credential')
    if ($hasConfig -ne $hasCredential) { throw 'Incomplete Agent identity; inspect the existing installation before retrying.' }
    if ($hasConfig) {
        $existing = Get-Content -LiteralPath (Join-Path $installDirectory 'agent.json') -Raw | ConvertFrom-Json
        if ($ExpectedDeviceId -and $existing.device_id -cne $ExpectedDeviceId) { throw 'This computer is already bound to a different device. Use its own installation command.' }
        Set-Service -Name UltraterimalAgent -StartupType Automatic
        if ((Get-Service -Name UltraterimalAgent).Status -ne 'Running') {
            & $expectedExecutable --start
            if ($LASTEXITCODE -ne 0) { throw 'Service start failed' }
        }
        & $expectedExecutable --status
        return
    }
} else {
    if (Get-Service UltraterimalAgent -ErrorAction SilentlyContinue) { throw 'Agent service already exists.' }
    if (Test-Path -LiteralPath $installDirectory) { throw 'Installation directory already exists; inspect it before retrying.' }
}
foreach ($file in @('agent.exe', 'ultraterimal_login.dll', 'ca.pem')) {
    if (-not (Test-Path -LiteralPath (Join-Path $PSScriptRoot $file))) { throw "Missing package file: $file" }
}
if (-not $MsiInstalled) {
    New-Item -ItemType Directory -Path $installDirectory | Out-Null
}
$acl = New-Object System.Security.AccessControl.DirectorySecurity
$acl.SetAccessRuleProtection($true, $false)
foreach ($sidText in @('S-1-5-18', 'S-1-5-32-544')) {
    $sid = New-Object System.Security.Principal.SecurityIdentifier($sidText)
    $rule = New-Object System.Security.AccessControl.FileSystemAccessRule($sid, 'FullControl', 'ContainerInherit,ObjectInherit', 'None', 'Allow')
    $acl.AddAccessRule($rule)
}
Set-Acl -LiteralPath $installDirectory -AclObject $acl
if (-not $MsiInstalled) {
    foreach ($file in @('agent.exe', 'ultraterimal_login.dll', 'ca.pem')) { Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination (Join-Path $installDirectory $file) }
}
$utf8 = New-Object System.Text.UTF8Encoding($false)
$bootstrapSettings = @{ host = $GatewayHost; port = [string]$GatewayPort; ca_file = 'ca.pem'; credential_file = 'agent.credential' }
if ($GatewayAddress) { $bootstrapSettings.address = $GatewayAddress }
$bootstrap = $bootstrapSettings | ConvertTo-Json -Compress
[IO.File]::WriteAllText((Join-Path $installDirectory 'bootstrap.json'), $bootstrap, $utf8)
$secureCode = if ($EnrollmentFromStdin) {
    $inputCode = [Console]::In.ReadLine()
    if ($inputCode -cnotmatch '^[0-9a-f]{64}$') { throw 'No valid enrollment code. Copy a new installation command from the pending device.' }
    ConvertTo-SecureString -String $inputCode -AsPlainText -Force
} else { Read-Host 'One-time enrollment code from the web console (hidden)' -AsSecureString }
$inputCode = $null
$codePointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secureCode)
try {
    $code = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($codePointer)
    if ($code -cnotmatch '^[0-9a-f]{64}$') { throw 'Invalid enrollment code' }
    $request = @{ code = $code; name = $env:COMPUTERNAME } | ConvertTo-Json -Compress
    [IO.File]::WriteAllText((Join-Path $installDirectory 'enroll.request'), $request, $utf8)
} finally {
    [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($codePointer)
    $code = $null
    $request = $null
    $secureCode.Dispose()
}
$runner = @'
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$result = 'failed'
try {
    $utf8 = New-Object System.Text.UTF8Encoding($false)
    $OutputEncoding = $utf8
    $request = [IO.File]::ReadAllText((Join-Path $root 'enroll.request'), $utf8)
    $connection = $request | & (Join-Path $root 'agent.exe') --enroll (Join-Path $root 'bootstrap.json') 2> (Join-Path $root 'enroll.error')
    $request = $null
    if ($LASTEXITCODE -ne 0) { throw 'Enrollment failed' }
    [IO.File]::WriteAllText((Join-Path $root 'agent.json'), ([string]$connection + [Environment]::NewLine), $utf8)
    $result = 'ok'
} finally {
    $request = $null
    Remove-Item -LiteralPath (Join-Path $root 'enroll.request') -Force -ErrorAction SilentlyContinue
    [IO.File]::WriteAllText((Join-Path $root 'enroll.result'), $result)
}
'@
$runnerPath = Join-Path $installDirectory 'enroll_system.ps1'
[IO.File]::WriteAllText($runnerPath, $runner, $utf8)
$taskName = 'UltraterimalEnroll-' + [Guid]::NewGuid().ToString('N')
try {
    $action = New-ScheduledTaskAction -Execute (Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe') -Argument ('-NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File "' + $runnerPath + '"') -WorkingDirectory $installDirectory
    $principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
    $settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 2)
    Register-ScheduledTask -TaskName $taskName -Action $action -Principal $principal -Settings $settings | Out-Null
    Start-ScheduledTask -TaskName $taskName
    $deadline = [DateTime]::UtcNow.AddSeconds(90)
    $resultPath = Join-Path $installDirectory 'enroll.result'
    while (-not (Test-Path -LiteralPath $resultPath)) {
        if ([DateTime]::UtcNow -gt $deadline) { throw 'Enrollment task timed out.' }
        Start-Sleep -Milliseconds 500
    }
    if ([IO.File]::ReadAllText($resultPath) -ne 'ok') { throw 'Enrollment failed; inspect the protected enroll.error file.' }
    $registered = Get-Content -LiteralPath (Join-Path $installDirectory 'agent.json') -Raw | ConvertFrom-Json
    if ($ExpectedDeviceId -and $registered.device_id -cne $ExpectedDeviceId) { throw 'Enrollment returned an unexpected device identity' }
    if (-not $MsiInstalled) {
        & (Join-Path $installDirectory 'agent.exe') --install
        if ($LASTEXITCODE -ne 0) { throw 'Service installation failed' }
    } else {
        Set-Service -Name UltraterimalAgent -StartupType Automatic
    }
    & (Join-Path $installDirectory 'agent.exe') --start
    if ($LASTEXITCODE -ne 0) { throw 'Service start failed' }
    & (Join-Path $installDirectory 'agent.exe') --status
} finally {
    if (Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue) {
        Stop-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
        Unregister-ScheduledTask -TaskName $taskName -Confirm:$false
    }
    foreach ($file in @('enroll.request', 'enroll_system.ps1', 'bootstrap.json', 'enroll.result')) {
        Remove-Item -LiteralPath (Join-Path $installDirectory $file) -Force -ErrorAction SilentlyContinue
    }
}

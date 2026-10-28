# Restart only the dedicated AI COLUMN local test instance recorded by this task.
param([string]$FreshDirectoryName = '', [ValidateSet('WARN', 'WDIAG', 'INFO')][string]$LogLevel = 'WARN')
$ErrorActionPreference = 'Stop'
$columnRepo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$columnWorkspace = Split-Path $columnRepo
$columnManifest = Join-Path $columnWorkspace '.codex-plan/ai-column-runtime-process.json'
$columnRun = Get-Content -LiteralPath $columnManifest -Raw | ConvertFrom-Json
$expectedExe = Join-Path $columnWorkspace 'ai-column-bin/seekdb.exe'
$expectedBase = [IO.Path]::GetFullPath($columnRun.base)
if ((Split-Path $expectedBase) -ne $columnWorkspace -or
    (Split-Path $expectedBase -Leaf) -notmatch '^ai-column-runtime-v[0-9]+$') {
    throw 'Runtime directory is outside the dedicated local test namespace'
}
if ([IO.Path]::GetFullPath($columnRun.exe) -ne [IO.Path]::GetFullPath($expectedExe) -or
    [IO.Path]::GetFullPath($columnRun.base) -ne [IO.Path]::GetFullPath($expectedBase) -or
    $columnRun.port -ne 28751) {
    throw 'Manifest does not identify the dedicated AI COLUMN test instance'
}
$columnProcess = Get-Process -Id $columnRun.pid -ErrorAction SilentlyContinue
if ($columnProcess) {
    if ($columnProcess.Path -ne $expectedExe) { throw 'PID belongs to another executable' }
    Stop-Process -InputObject $columnProcess
    if (-not $columnProcess.WaitForExit(10000)) { throw 'Test process did not exit' }
}
$probe = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Any, [int]$columnRun.port)
try { $probe.Start() } finally { $probe.Stop() }
Copy-Item -LiteralPath (Join-Path $columnRepo 'build_ai_column/src/observer/seekdb.exe') -Destination $expectedExe
if ($FreshDirectoryName) {
    if ($FreshDirectoryName -notmatch '^ai-column-runtime-v[0-9]+$') { throw 'Invalid fresh test directory name' }
    $expectedBase = Join-Path $columnWorkspace $FreshDirectoryName
    if (Test-Path -LiteralPath $expectedBase) { throw 'Fresh runtime directory already exists' }
    New-Item -ItemType Directory -Path $expectedBase | Out-Null
}
$columnProcess = Start-Process -FilePath $expectedExe -ArgumentList @(
    '--nodaemon', '--port=28751', "--base-dir=$expectedBase", "--log-level=$LogLevel",
    '--parameter', 'log_disk_size=2G', '--parameter', 'datafile_size=1G', '--parameter', 'datafile_maxsize=2G'
) -WorkingDirectory $expectedBase -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput (Join-Path $expectedBase 'stdout.log') `
    -RedirectStandardError (Join-Path $expectedBase 'stderr.log')
$columnRun.pid = $columnProcess.Id
$columnRun.base = $expectedBase
$columnRun | ConvertTo-Json | Set-Content -LiteralPath $columnManifest
$columnReady = $false
$columnReadyDeadline = [DateTime]::UtcNow.AddSeconds(30)
while ([DateTime]::UtcNow -lt $columnReadyDeadline -and -not $columnReady) {
    $columnProcess.Refresh()
    if ($columnProcess.HasExited) { throw 'Dedicated test instance exited during startup; inspect its stderr and service log' }
    $columnClient = [Net.Sockets.TcpClient]::new()
    try {
        $columnConnect = $columnClient.ConnectAsync('127.0.0.1', [int]$columnRun.port)
        if ($columnConnect.Wait(200) -and $columnClient.Connected) { $columnReady = $true }
    } catch { } finally { $columnClient.Dispose() }
    if (-not $columnReady) { Start-Sleep -Milliseconds 100 }
}
if (-not $columnReady) { throw 'Dedicated test instance did not open its SQL port within 30 seconds' }
$columnProcess | Select-Object Id, HasExited

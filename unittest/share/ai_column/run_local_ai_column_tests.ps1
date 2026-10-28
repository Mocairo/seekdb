# Run real seekdb SQL regression suites sequentially against a local instance.
param(
    [string]$PythonExecutable = 'python',
    [string]$PyMySqlDirectory = '',
    [int]$SqlPort = 28751,
    [switch]$IncludeRecovery
)
$ErrorActionPreference = 'Stop'
$columnRepo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$columnOutput = Join-Path $columnRepo 'build_ai_column_tests/regression'
New-Item -ItemType Directory -Path $columnOutput -Force | Out-Null
$columnSuites = @(
    @{ Name = 'ddl'; Script = 'test_ai_column_ddl_live.py'; Extra = @('--extended') },
    @{ Name = 'transaction'; Script = 'test_ai_column_t1_live.py'; Extra = @() },
    @{ Name = 'worker'; Script = 'test_ai_column_worker_live.py'; Extra = @() },
    @{ Name = 'retry-strict'; Script = 'test_ai_column_retry_strict_live.py'; Extra = @() },
    @{ Name = 'backfill'; Script = 'test_ai_column_backfill_live.py'; Extra = @() },
    @{ Name = 'vector'; Script = 'test_ai_column_vector_live.py'; Extra = @() },
    @{ Name = 'edges'; Script = 'test_ai_column_edges_live.py'; Extra = @() }
)
if ($IncludeRecovery) {
    if ($SqlPort -ne 28751) { throw 'Recovery is restricted to the dedicated port 28751 instance' }
    $columnSuites += @{ Name = 'recovery'; Script = 'test_ai_column_recovery_live.py'; Extra = @() }
}
$columnPreviousPythonPath = $env:PYTHONPATH
$columnResults = [Collections.Generic.List[object]]::new()
try {
    if ($PyMySqlDirectory) {
        $env:PYTHONPATH = if ($columnPreviousPythonPath) { $PyMySqlDirectory + [IO.Path]::PathSeparator + $columnPreviousPythonPath } else { $PyMySqlDirectory }
    }
    foreach ($columnSuite in $columnSuites) {
        $columnLog = Join-Path $columnOutput ($columnSuite.Name + '.log')
        $columnArguments = @('-u', (Join-Path $columnRepo ('unittest/sql/resolver/' + $columnSuite.Script)), '--port', "$SqlPort") + $columnSuite.Extra
        $columnTimer = [Diagnostics.Stopwatch]::StartNew()
        & $PythonExecutable @columnArguments *> $columnLog
        $columnExit = $LASTEXITCODE
        $columnTimer.Stop()
        $columnResults.Add([pscustomobject]@{
            suite = $columnSuite.Name; passed = ($columnExit -eq 0)
            elapsed_seconds = [math]::Round($columnTimer.Elapsed.TotalSeconds, 3)
            exit_code = $columnExit; log = $columnLog
        })
        $columnResults | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 (Join-Path $columnOutput 'results.json')
        if ($columnExit -ne 0) {
            Get-Content -LiteralPath $columnLog -Tail 18
            throw "AI COLUMN suite failed: $($columnSuite.Name)"
        }
        Write-Output "PASS $($columnSuite.Name) ($([math]::Round($columnTimer.Elapsed.TotalSeconds, 2))s)"
    }
    Write-Output "Results: $(Join-Path $columnOutput 'results.json')"
} finally {
    $env:PYTHONPATH = $columnPreviousPythonPath
}

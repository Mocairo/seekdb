param([string]$CxxCompiler = "", [string]$CMakeExecutable = "")
$ErrorActionPreference = 'Stop'
$repoPath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
if (-not $CMakeExecutable) {
    $bundledCMake = Join-Path $repoPath 'deps/3rd/tools/cmake/bin/cmake.exe'
    if (Test-Path -LiteralPath $bundledCMake) { $CMakeExecutable = $bundledCMake }
    else { $CMakeExecutable = (Get-Command cmake -ErrorAction Stop).Source }
}
$stateBuildPath = Join-Path $repoPath 'build_ai_column_state'
$stateTempPath = Join-Path $stateBuildPath 'tmp'
New-Item -ItemType Directory -Path $stateTempPath -Force | Out-Null
$previousTemp = $env:TEMP
$previousTmp = $env:TMP
try {
    $env:TEMP = $stateTempPath
    $env:TMP = $stateTempPath
    $configureArgs = @('--fresh', '-S', $PSScriptRoot, '-B', $stateBuildPath, '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY')
    $bundledNinja = Join-Path $repoPath 'deps/3rd/tools/ninja/ninja.exe'
    if (Test-Path -LiteralPath $bundledNinja) {
        $configureArgs += @('-G', 'Ninja', "-DCMAKE_MAKE_PROGRAM=$bundledNinja")
    }
    if ($CxxCompiler) {
        $configureArgs += "-DCMAKE_CXX_COMPILER=$CxxCompiler"
        $compilerDirectory = Split-Path $CxxCompiler
        $resourceCompiler = (Join-Path $compilerDirectory 'llvm-rc.exe').Replace('\', '/')
        $manifestTool = (Join-Path $compilerDirectory 'llvm-mt.exe').Replace('\', '/')
        if (Test-Path -LiteralPath $resourceCompiler) { $configureArgs += "-DCMAKE_RC_COMPILER:FILEPATH=$resourceCompiler" }
        if (Test-Path -LiteralPath $manifestTool) { $configureArgs += "-DCMAKE_MT:FILEPATH=$manifestTool" }
    }
    & $CMakeExecutable @configureArgs
    if ($LASTEXITCODE -ne 0) { throw 'State test configuration failed' }
    & $CMakeExecutable --build $stateBuildPath --config Release
    if ($LASTEXITCODE -ne 0) { throw 'State test compilation failed' }
    $ctestPath = Join-Path (Split-Path $CMakeExecutable) 'ctest.exe'
    if (-not (Test-Path -LiteralPath $ctestPath)) { $ctestPath = (Get-Command ctest -ErrorAction Stop).Source }
    & $ctestPath --test-dir $stateBuildPath -C Release --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'State tests failed' }
} finally {
    $env:TEMP = $previousTemp
    $env:TMP = $previousTmp
}

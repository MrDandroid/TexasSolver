param(
    [bool]$ExportBin = $true,
    [switch]$UpdateBaseline,
    [int]$Threads = 8,
    [int]$MaxIteration = 200,
    [int]$PrintInterval = 10,
    [double]$Accuracy = 0.5,
    [int]$DumpRounds = 2,
    [switch]$ProfileHotspots,
    [switch]$DisableLightRiverCombs,
    [switch]$DisableShowdownFastFields,
    [switch]$DisableTerminalSameCardCache,
    [switch]$DisableActionStrategyBuffer,
    [switch]$DisableUpdateRegretsInline,
    [switch]$DisableChanceReachBufferReuse,
    [switch]$DisableFastCardAccessors,
    [switch]$DisableActionRegretDirectUpdate,
    [switch]$OptimizeO2,
    [string]$OutputDir = ""
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = "Stop"

function Convert-KeyValueLine {
    param([string]$Line)
    $result = [ordered]@{}
    foreach ($match in [regex]::Matches($Line, "([A-Za-z_][A-Za-z0-9_]*)=([^ ]+)")) {
        $key = $match.Groups[1].Value
        $value = $match.Groups[2].Value
        if ($value -match "^-?\d+$") {
            $result[$key] = [int64]$value
        } elseif ($value -match "^-?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?$") {
            $result[$key] = [double]$value
        } else {
            $result[$key] = $value
        }
    }
    return $result
}

function Get-LastRegexGroup {
    param(
        [string]$Text,
        [string]$Pattern,
        [int]$Group = 1
    )
    $matches = [regex]::Matches($Text, $Pattern)
    if ($matches.Count -eq 0) {
        return $null
    }
    return $matches[$matches.Count - 1].Groups[$Group].Value
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = Join-Path $PSScriptRoot "benchmark_outputs\default_solver_bench"
}
$OutputDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDir)
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

$qtRoot = if ($env:QT_ROOT) { $env:QT_ROOT } else { "D:\Qt\5.15.2\mingw81_64" }
$mingwRoot = if ($env:MINGW_ROOT) { $env:MINGW_ROOT } else { "D:\Qt\Tools\mingw810_64" }
$env:PATH = "$mingwRoot\bin;$qtRoot\bin;$env:PATH"

$qmake = Join-Path $qtRoot "bin\qmake.exe"
$make = Join-Path $mingwRoot "bin\mingw32-make.exe"
$gxx = Join-Path $mingwRoot "bin\g++.exe"
$optimizationLevel = if ($OptimizeO2) { "-O2" } else { "-O3" }
$optLightRiverCombs = if ($DisableLightRiverCombs) { 0 } else { 1 }
$optShowdownFastFields = if ($DisableShowdownFastFields) { 0 } else { 1 }
$optTerminalSameCardCache = if ($DisableTerminalSameCardCache) { 0 } else { 1 }
$optActionStrategyBuffer = if ($DisableActionStrategyBuffer) { 0 } else { 1 }
$optUpdateRegretsInline = if ($DisableUpdateRegretsInline) { 0 } else { 1 }
$optChanceReachBufferReuse = if ($DisableChanceReachBufferReuse) { 0 } else { 1 }
$optFastCardAccessors = if ($DisableFastCardAccessors) { 0 } else { 1 }
$optActionRegretDirectUpdate = if ($DisableActionRegretDirectUpdate) { 0 } else { 1 }
$optimizationDefines = @(
    "TEXASSOLVER_OPT_LIGHT_RIVER_COMBS=$optLightRiverCombs",
    "TEXASSOLVER_OPT_SHOWDOWN_FAST_FIELDS=$optShowdownFastFields",
    "TEXASSOLVER_OPT_TERMINAL_SAME_CARD_CACHE=$optTerminalSameCardCache",
    "TEXASSOLVER_OPT_ACTION_STRATEGY_BUFFER=$optActionStrategyBuffer",
    "TEXASSOLVER_OPT_UPDATE_REGRETS_INLINE=$optUpdateRegretsInline",
    "TEXASSOLVER_OPT_CHANCE_REACH_BUFFER_REUSE=$optChanceReachBufferReuse",
    "TEXASSOLVER_OPT_FAST_CARD_ACCESSORS=$optFastCardAccessors",
    "TEXASSOLVER_OPT_ACTION_REGRET_DIRECT_UPDATE=$optActionRegretDirectUpdate"
)

$buildName = "Desktop_Qt_5_15_2_MinGW_64_bit-release"
if ($optimizationLevel -eq "-O3") {
    $buildName += "-O3"
}
if ($ProfileHotspots) {
    $buildName += "-hotspots"
}
if ($DisableLightRiverCombs) {
    $buildName += "-no-light-river-combs"
}
if ($DisableShowdownFastFields) {
    $buildName += "-no-showdown-fast-fields"
}
if ($DisableTerminalSameCardCache) {
    $buildName += "-no-terminal-same-card-cache"
}
if ($DisableActionStrategyBuffer) {
    $buildName += "-no-action-strategy-buffer"
}
if ($DisableUpdateRegretsInline) {
    $buildName += "-no-update-regrets-inline"
}
if ($DisableChanceReachBufferReuse) {
    $buildName += "-no-chance-reach-buffer-reuse"
}
if ($DisableFastCardAccessors) {
    $buildName += "-no-fast-card-accessors"
}
if ($DisableActionRegretDirectUpdate) {
    $buildName += "-no-action-regret-direct-update"
}
$buildDir = Join-Path $repoRoot "build\$buildName"
$releaseDir = Join-Path $buildDir "release"
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

if (!(Test-Path (Join-Path $buildDir "Makefile.Release"))) {
    Push-Location $buildDir
    try {
        $qmakeArgs = @("..\..\TexasSolverGui.pro", "-spec", "win32-g++", "CONFIG+=release", "QMAKE_CXXFLAGS_RELEASE+=$optimizationLevel")
        if ($ProfileHotspots) {
            $qmakeArgs += "DEFINES+=TEXASSOLVER_HOTSPOT_PROFILING"
        }
        foreach ($define in $optimizationDefines) {
            $qmakeArgs += "DEFINES+=$define"
        }
        & $qmake @qmakeArgs
        if ($LASTEXITCODE -ne 0) {
            throw "qmake failed with exit code $LASTEXITCODE"
        }
    } finally {
        Pop-Location
    }
}

Push-Location $buildDir
try {
    & $make -f Makefile.Release
    if ($LASTEXITCODE -ne 0) {
        throw "mingw32-make failed with exit code $LASTEXITCODE"
    }
} finally {
    Pop-Location
}

$source = Join-Path $PSScriptRoot "default_solver_bench.cpp"
$exe = Join-Path $OutputDir "default_solver_bench.exe"
$objects = Get-ChildItem $releaseDir -Filter "*.o" |
    Where-Object { $_.Name -ne "main.o" } |
    ForEach-Object { $_.FullName }
$benchmarkDefines = $optimizationDefines | ForEach-Object { "-D$_" }

$compileArgs = @(
    "-std=gnu++17",
    $optimizationLevel,
    "-fopenmp",
    "-mconsole"
) + $benchmarkDefines + @(
    "-I$repoRoot",
    "-I$(Join-Path $repoRoot 'include')",
    "-I$(Join-Path $repoRoot 'src\3rdparty\sqlite')",
    "-I$(Join-Path $repoRoot 'zsdt')",
    "-I$qtRoot\include",
    "-I$qtRoot\include\QtCore",
    "-I$qtRoot\include\QtGui",
    "-I$qtRoot\include\QtWidgets",
    "-I$qtRoot\include\QtSql",
    $source
) + $objects + @(
    "-L$qtRoot\lib",
    "-L$(Join-Path $repoRoot 'zsdt')",
    "-l:lzstd.lib",
    "-lQt5Widgets",
    "-lQt5Gui",
    "-lQt5Sql",
    "-lQt5Core",
    "-lz",
    "-lopengl32",
    "-lgdi32",
    "-luser32",
    "-lkernel32",
    "-lshell32",
    "-lole32",
    "-luuid",
    "-lwinmm",
    "-lws2_32",
    "-ladvapi32",
    "-lcomdlg32",
    "-limm32",
    "-o",
    $exe
)

& $gxx @compileArgs
if ($LASTEXITCODE -ne 0) {
    throw "benchmark compile failed with exit code $LASTEXITCODE"
}

$stdoutLog = Join-Path $OutputDir "latest_stdout.log"
$stderrLog = Join-Path $OutputDir "latest_stderr.log"
$binPath = Join-Path $OutputDir "default_strategy.bin"
$binBase = [System.IO.Path]::Combine(
    [System.IO.Path]::GetDirectoryName($binPath),
    [System.IO.Path]::GetFileNameWithoutExtension($binPath)
)

Remove-Item -LiteralPath $stdoutLog, $stderrLog -ErrorAction SilentlyContinue
if ($ExportBin) {
    Remove-Item -LiteralPath "$binBase.tsb", "$binBase.tsx", "$binBase.tsm" -ErrorAction SilentlyContinue
}

$runnerArgs = @(
    "--resource-dir", (Join-Path $repoRoot "resources"),
    "--threads", "$Threads",
    "--max-iteration", "$MaxIteration",
    "--print-interval", "$PrintInterval",
    "--accuracy", "$Accuracy",
    "--dump-rounds", "$DumpRounds"
)
if ($ExportBin) {
    $runnerArgs += @("--export-bin", $binPath)
}
if ($ProfileHotspots) {
    $runnerArgs += @("--profile-hotspots")
}

$process = Start-Process -FilePath $exe `
    -ArgumentList $runnerArgs `
    -WorkingDirectory $repoRoot `
    -RedirectStandardOutput $stdoutLog `
    -RedirectStandardError $stderrLog `
    -WindowStyle Hidden `
    -Wait `
    -PassThru

if ($process.ExitCode -ne 0) {
    Get-Content -LiteralPath $stderrLog -Tail 80
    throw "default_solver_bench.exe failed with exit code $($process.ExitCode)"
}

$stdout = if (Test-Path $stdoutLog) { Get-Content -LiteralPath $stdoutLog -Raw } else { "" }
$stderr = if (Test-Path $stderrLog) { Get-Content -LiteralPath $stderrLog -Raw } else { "" }
$resultLine = Get-LastRegexGroup $stdout "(BENCH_DEFAULT_RESULT[^\r\n]*)"
$configLine = Get-LastRegexGroup $stdout "(BENCH_DEFAULT_CONFIG[^\r\n]*)"
$timing = if ($resultLine) { Convert-KeyValueLine $resultLine } else { [ordered]@{} }
$config = if ($configLine) { Convert-KeyValueLine $configLine } else { [ordered]@{} }

$hotspots = @()
foreach ($line in ($stdout -split '\r?\n')) {
    if ($line -like "BENCH_HOTSPOT*") {
        $kv = Convert-KeyValueLine $line
        $hotspots += [ordered]@{
            name = $kv["name"]
            count = $kv["count"]
            inclusive_ms = $kv["inclusive_ms"]
            self_ms = $kv["self_ms"]
        }
    }
}

$finalIterText = Get-LastRegexGroup $stderr "Iter:\s+(\d+)"
$finalExploitText = Get-LastRegexGroup $stderr "Total exploitability\s+([-+0-9.]+)\s+precent"
$finalSolverTimeText = Get-LastRegexGroup $stderr "time used:\s+([-+0-9.]+)\s+second"

$hashes = @()
if ($ExportBin) {
    foreach ($ext in @("tsb", "tsx", "tsm")) {
        $path = "$binBase.$ext"
        if (Test-Path $path) {
            $file = Get-Item -LiteralPath $path
            $hashes += [ordered]@{
                file = $file.Name
                length = $file.Length
                sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
            }
        }
    }
}

$summary = [ordered]@{
    timestamp = (Get-Date).ToString("s")
    repo_root = $repoRoot
    runner = $source
    config = $config
    compiler_optimization = $optimizationLevel
    optimization_switches = [ordered]@{
        light_river_combs = [bool]$optLightRiverCombs
        showdown_fast_fields = [bool]$optShowdownFastFields
        terminal_same_card_cache = [bool]$optTerminalSameCardCache
        action_strategy_buffer = [bool]$optActionStrategyBuffer
        update_regrets_inline = [bool]$optUpdateRegretsInline
        chance_reach_buffer_reuse = [bool]$optChanceReachBufferReuse
        fast_card_accessors = [bool]$optFastCardAccessors
        action_regret_direct_update = [bool]$optActionRegretDirectUpdate
    }
    timing_ms = $timing
    final_iteration = if ($finalIterText) { [int]$finalIterText } else { $null }
    final_exploitability_percent = if ($finalExploitText) { [double]$finalExploitText } else { $null }
    final_solver_log_time_seconds = if ($finalSolverTimeText) { [double]$finalSolverTimeText } else { $null }
    hotspots = $hotspots
    output_hashes = $hashes
    stdout_log = $stdoutLog
    stderr_log = $stderrLog
}

$latestJson = Join-Path $OutputDir "latest_result.json"
$baselineJson = Join-Path $OutputDir "baseline_result.json"
$comparison = [ordered]@{
    status = "not_checked"
    hash_mismatches = @()
    exploitability_delta = $null
}

if (!(Test-Path $baselineJson) -or $UpdateBaseline) {
    $comparison.status = if ($UpdateBaseline) { "baseline_updated" } else { "baseline_created" }
} else {
    $baseline = Get-Content -LiteralPath $baselineJson -Raw | ConvertFrom-Json
    $baseMap = @{}
    foreach ($entry in $baseline.output_hashes) {
        $baseMap[$entry.file] = $entry.sha256
    }

    $hashMismatches = @()
    foreach ($entry in $hashes) {
        if (!$baseMap.ContainsKey($entry.file)) {
            $hashMismatches += "$($entry.file):missing_in_baseline"
        } elseif ($baseMap[$entry.file] -ne $entry.sha256) {
            $hashMismatches += "$($entry.file):sha256_changed"
        }
    }

    if ($null -ne $baseline.final_exploitability_percent -and $null -ne $summary.final_exploitability_percent) {
        $comparison.exploitability_delta = [math]::Abs(
            [double]$baseline.final_exploitability_percent - [double]$summary.final_exploitability_percent
        )
    }
    $comparison.hash_mismatches = $hashMismatches
    $comparison.status = if ($hashMismatches.Count -eq 0) { "match" } else { "mismatch" }
}

$summary["comparison_to_baseline"] = $comparison
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $latestJson -Encoding UTF8

$runsDir = Join-Path $OutputDir "runs"
New-Item -ItemType Directory -Force -Path $runsDir | Out-Null
$runStamp = (Get-Date).ToString("yyyyMMdd_HHmmss")
$runJson = Join-Path $runsDir "$runStamp.json"
Copy-Item -LiteralPath $latestJson -Destination $runJson -Force

$bestJson = Join-Path $OutputDir "best_result.json"
$isDefaultBenchShape = $ExportBin -and
    $MaxIteration -eq 200 -and
    $PrintInterval -eq 10 -and
    [math]::Abs($Accuracy - 0.5) -lt 0.0000001 -and
    $DumpRounds -eq 2
$canUpdateBest = $isDefaultBenchShape -and (
    $comparison.status -eq "match" -or
    $comparison.status -eq "baseline_created" -or
    $comparison.status -eq "baseline_updated"
)
if ($canUpdateBest -and $null -ne $timing.solve_ms) {
    $updateBest = $true
    if (Test-Path $bestJson) {
        $best = Get-Content -LiteralPath $bestJson -Raw | ConvertFrom-Json
        if ($null -ne $best.timing_ms -and $null -ne $best.timing_ms.solve_ms) {
            $updateBest = [int64]$timing.solve_ms -lt [int64]$best.timing_ms.solve_ms
        }
    }
    if ($updateBest) {
        Copy-Item -LiteralPath $latestJson -Destination $bestJson -Force
    }
}

$hotspotJson = Join-Path $OutputDir "latest_hotspots.json"
if ($ProfileHotspots) {
    Copy-Item -LiteralPath $latestJson -Destination $hotspotJson -Force
}

if ($comparison.status -eq "baseline_created" -or $comparison.status -eq "baseline_updated") {
    Copy-Item -LiteralPath $latestJson -Destination $baselineJson -Force
}

Write-Host "BENCH_SUMMARY $latestJson"
Write-Host "BENCH_RUN $runJson"
if (Test-Path $bestJson) {
    $bestForPrint = Get-Content -LiteralPath $bestJson -Raw | ConvertFrom-Json
    Write-Host "BENCH_BEST $bestJson"
    Write-Host "BENCH_BEST_SOLVE_MS $($bestForPrint.timing_ms.solve_ms)"
}
if ($ProfileHotspots) {
    Write-Host "BENCH_HOTSPOTS $hotspotJson"
}
Write-Host "BENCH_BASELINE $baselineJson"
Write-Host "BENCH_COMPARE $($comparison.status)"
Write-Host "BENCH_COMPILER_OPT $optimizationLevel"
Write-Host "BENCH_OPT_SWITCHES light_river_combs=$optLightRiverCombs showdown_fast_fields=$optShowdownFastFields terminal_same_card_cache=$optTerminalSameCardCache action_strategy_buffer=$optActionStrategyBuffer update_regrets_inline=$optUpdateRegretsInline chance_reach_buffer_reuse=$optChanceReachBufferReuse fast_card_accessors=$optFastCardAccessors action_regret_direct_update=$optActionRegretDirectUpdate"
Write-Host "BENCH_SOLVE_MS $($timing.solve_ms)"
Write-Host "BENCH_EXPORT_BIN_MS $($timing.export_bin_ms)"
Write-Host "BENCH_FINAL_ITER $($summary.final_iteration)"
Write-Host "BENCH_FINAL_EXPLOITABILITY_PERCENT $($summary.final_exploitability_percent)"
foreach ($entry in $hashes) {
    Write-Host "BENCH_HASH $($entry.file) $($entry.length) $($entry.sha256)"
}
if ($hotspots.Count -gt 0) {
    Write-Host "BENCH_HOTSPOT_TOP"
    foreach ($entry in ($hotspots | Sort-Object -Property { [double]$_["self_ms"] } -Descending | Select-Object -First 12)) {
        Write-Host ("  {0} self_ms={1} inclusive_ms={2} count={3}" -f $entry["name"], $entry["self_ms"], $entry["inclusive_ms"], $entry["count"])
    }
}

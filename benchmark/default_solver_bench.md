# Default Solver Benchmark

This benchmark reproduces the GUI default Hold'em setup:

- board: `Qs,Jh,2h`
- pot: `50`
- effective stack: `200`
- iterations: `200`
- exploitability stop: `0.5%`
- log interval: `10`
- threads: `8`
- dump rounds: `2`
- bin export: TSB2 `.tsb/.tsx/.tsm`

Run from the repository root:

```powershell
.\benchmark\run_default_solver_bench.ps1
```

The script writes outputs under:

```text
benchmark\benchmark_outputs\default_solver_bench
```

Important files:

- `baseline_result.json`: saved baseline for future comparisons.
- `latest_result.json`: latest run summary and comparison status.
- `latest_stdout.log`: benchmark runner timing markers.
- `latest_stderr.log`: solver iteration/exploitability log.
- `latest_hotspots.json`: latest hotspot profile when `-ProfileHotspots` is used.
- `default_strategy.tsb/.tsx/.tsm`: exported TSB2 strategy files.

To collect the hotspot map:

```powershell
.\benchmark\run_default_solver_bench.ps1 -ProfileHotspots
```

Hotspot timings are instrumentation timings. Use them to compare relative cost inside one profiled run, not as the clean wall-clock speed baseline.

To replace the baseline after an intentional algorithm/export change:

```powershell
.\benchmark\run_default_solver_bench.ps1 -UpdateBaseline
```

Optional environment overrides:

- `QT_ROOT`, default `D:\Qt\5.15.2\mingw81_64`
- `MINGW_ROOT`, default `D:\Qt\Tools\mingw810_64`

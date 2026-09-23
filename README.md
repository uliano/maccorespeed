# maccorespeed

A small CPU benchmark for Apple Silicon Macs that measures:

1. **Single-core** speed: one thread, which the scheduler places on a fastest core.
2. **Multi-core burst**: all cores for about 5 seconds, started with a cool CPU. This is the
   speed before any thermal throttling.
3. **Multi-core sustained**: the same multi-core test run continuously. The measurement
   starts only once performance and temperature have stopped drifting (thermal steady
   state). On a fanless MacBook Air this shows how much throttling costs. On a Mac with a
   fan the result should stay close to the burst score.

It was written for a fun comparison between a MacBook Air M5 and an M2 Pro. It is not a
replacement for Geekbench or Cinebench.

## Usage

```sh
./run.sh            # build for this Mac, run everything (~5-20 min), log to results/
./run.sh --quick    # 1-minute smoke test
./run.sh --help     # options (phase selection, steady-state tolerances, ...)
./compare.sh results/Apple-M5-*.txt results/Apple-M2-Pro-*.txt   # side by side
```

`run.sh` calls `build.sh`. That script compiles with Apple clang (`/usr/bin/clang`, from
Xcode or the Command Line Tools, installed with `xcode-select --install`) and picks the
most specific `-mcpu` that the compiler supports for the chip, e.g. `apple-m5` or
`apple-m2`. To run on a Mac that has no compiler, build the binary on another Mac and copy
the whole folder:

```sh
./build.sh --cpu apple-m2        # creates bin/maccorespeed-apple-m2
# on the target Mac, if the binary was copied via AirDrop / download:
xattr -dr com.apple.quarantine .
./run.sh
```

For meaningful numbers: plug the Mac into power, turn Low Power Mode off, close other apps,
and wait for background jobs (Spotlight indexing, builds, backups) to finish. Keep the Mac
on a hard surface and don't touch it while it runs. The program warns you if other
processes use CPU while it runs. You can press Ctrl-C during the sustained phase: it stops
and prints a summary of what it has measured so far.

## Results

| Mac | Single-core | Multi-core burst | Multi-core sustained | Sustained / burst |
|-----|------------:|-----------------:|---------------------:|------------------:|
| MacBook Air (15-inch, M5): 4 Super + 6 Efficiency cores, no fan | 1001 | 7371 | 5945 | 80.7% |
| M2 Pro | *coming soon* | | | |

### MacBook Air (15-inch, M5)

macOS 27.0, Apple clang 21.0.0 `-O3 -mcpu=apple-m5`, AC power, 2026-09-23. Full output:
[log](results/Apple-M5-20260923-093810.txt), sustained time series:
[CSV](results/Apple-M5-20260923-093810.csv).

| | single-core | multi-core burst | multi-core sustained | sustained / burst |
|---|---:|---:|---:|---:|
| sort (Melem/s) | 30.6 | 246.1 | 205.8 | 84% |
| lz (MB/s) | 1097 | 8016 | 6375 | 80% |
| nbody (Msteps/s) | 28.3 | 221.7 | 187.7 | 85% |
| sgemm (GFLOPS) | 116.8 | 744.6 | 559.9 | 75% |
| Super cores clock | 4.33-4.46 GHz | 4.04 GHz | 2.63 GHz | |
| Efficiency cores clock | | 2.96 GHz | 2.89 GHz | |
| CPU power | 4.3-7.9 W | 22.9 W | 10.3 W | |

- The multi-core burst is 7.36x the single-core score.
- Throttling starts 5-6 s after a cold start. The sustained phase reached steady state
  after 5:32, at 19% below the burst score.
- Only the Super cores slow down, from about 4.0 to 2.6 GHz. The Efficiency cores stay at
  about 2.9 GHz, and macOS never moves the threads off the Super cores.
- The hottest core sensor reaches about 100 °C in about 15 s. It then falls to about 87 °C
  while CPU power keeps dropping from 23 to 10 W, so what limits the chip in the long run
  is the chassis temperature, not the die.

```
  sustained score over time, % of burst:
   105% ┤
        │█▁
    95% ┤██▅▂▁
        │██████▇▆▅▄▆▆▆▆▆▅▄▄▄▄▃▂
    85% ┤██████████████████████▇▄▇█▇▅▄▃▁                     ▁▂▁▂ ▂ ▁▂▂▂▂
        │█████████████████████████████████▄ ▄▆▃▂▅▅▅▇█▁   █▃▆▆██████▄█████
    75% ┤██████████████████████████████████ ███████████▄▇████████████████
        │██████████████████████████████████▇█████████████████████████████
    65% └────────────────────────────────────────────────────────────────
         0:04                                                       6:35
```

## What it runs

Four small kernels that fit in L1/L2, so they measure the cores and not memory:

| kernel | workload | stresses |
|--------|----------|----------|
| `sort`  | quicksort of 32K random 32-bit keys | branchy integer code, branch mispredictions |
| `lz`    | LZ77 (LZ4-like) compression of 64 KB of synthetic text | integer ops, byte loads, hash lookups |
| `nbody` | the Benchmarks Game 5-body simulation (double) | scalar FP, sqrt/div latency |
| `sgemm` | 192x192 fp32 matrix multiply, NEON 8x8 FMA micro-kernel | SIMD FMA throughput |

Each score is `1000 x geomean(rate_k / ref_k)`. The reference rates are the single-core
results of the MacBook Air M5, so that machine scores about 1000 single-core. Every
multi-core score uses the same references, so the multi-core burst score divided by the
single-core score gives the multi-core speedup. Before measuring, a self-test checks that
each kernel computes correct results.

The sustained phase runs rounds of the four kernels, 1 s each, back to back. Steady state
is declared when, over the last 120 s, the average score of the second minute differs from
the first by less than 1 %/min and the temperature by less than 1 °C/min. The sustained
score is then the average over the next 60 s. The time series is saved as CSV next to the
log.

Temperature alone is not a good steady-state signal on a fanless Mac. On a MacBook Air M5
under full load, the hottest core sensor reaches about 100 °C in about 15 s and stays
there. Performance drops only slightly while that happens: it starts to fall after about
5 s. After that the chip keeps lowering the power (and clock) of the fast cores for minutes
as the chassis heats up, while the temperature reading stays flat. So the score has to
stop drifting too. The burst phase lasts less than 5 s for the same reason.

## Where the numbers come from (no sudo needed)

- **Clock (GHz), IPC and CPU power**: `proc_pid_rusage(RUSAGE_INFO_V6)`. On Apple Silicon
  the kernel keeps per-process cycles, instructions and energy, split by core type (e.g.
  Super/Performance vs Efficiency). "GHz" is cycles / CPU time on that core type. "CPU W"
  is the CPU energy billed to the benchmark per second. "busy S/E" is the average number of
  cores of each type running benchmark threads. A drop there means macOS moved threads off
  the fast cores.
- **Temperature**: the hottest SMC CPU core sensor (`Tp*`/`Te*`/`Tf*` keys), or IOHID die
  sensors as a fallback (`--list-sensors` shows them all). These are the same private
  interfaces used by tools like Stats and macmon.
- **Fans** (on Macs that have them): SMC `F*Ac` keys.
- **Thermal pressure**: the public `com.apple.system.thermalpressurelevel` notification
  (nominal / moderate / heavy / trapping / sleeping).

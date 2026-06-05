# CLI Tool For Relative Benchmarking

## Problem Statement

Many C programs are compared only by theory, such as `O(n)` or `O(n^2)`.
Real systems also depend on CPU cycles, instruction count, branch prediction,
cache locality, memory allocation overhead, and thermal conditions. This
project provides a Linux command-line tool that compares two compiled binaries
using real performance counters from `perf`.

## Objectives

- Run two executable programs under the same benchmarking tool.
- Collect elapsed time, CPU cycles, instructions, and branch misses.
- Calculate IPC, which means instructions per cycle.
- Show percentage differences relative to the first binary.
- Give a careful verdict without forcing a winner when results are mixed.
- Report CPU temperature and frequency as fairness context only.

## Linux Requirements

This project is intended for Linux.

Install the common tools on Ubuntu/Debian:

```sh
sudo apt update
sudo apt install gcc make linux-tools-common linux-tools-generic
```

If `perf` cannot access hardware counters, run with `sudo` or check:

```sh
cat /proc/sys/kernel/perf_event_paranoid
```

## Build

The final source of truth is `perfcmp.c`. Older files such as `G1.c` to
`G5.c` are development snapshots and should not be used for final evaluation.
Run `make` before the demo so the executable matches the final source.

```sh
make clean
make
```

The main executable is:

```sh
./perfcmp
```

The testcase binaries are built inside `testcases/`.

## Usage

```sh
./perfcmp
./perfcmp --demo
./perfcmp <binary1> <binary2>
./perfcmp --duration 10 <binary1> <binary2>
./perfcmp --help
```

Running `./perfcmp` opens a menu of built-in testcase pairs. Use the full
binary-path form only when you want to benchmark your own new programs.

Example demo commands:

```sh
./perfcmp
./perfcmp --demo
./perfcmp --duration 10 ./testcases/01_row_major ./testcases/02_col_major
./perfcmp --duration 10 ./testcases/03_bubble_sort ./testcases/04_selection_sort
./perfcmp --duration 10 ./testcases/05_without_prefetch ./testcases/06_with_prefetch
./perfcmp --duration 10 ./testcases/07_malloc_alloc ./testcases/08_stack_alloc
```

## Testcase Pairs

| Pair | Concept | Expected Discussion |
| --- | --- | --- |
| Row-major vs column-major | Cache locality | Row-major access is usually faster in C because memory is stored row by row. |
| Bubble sort vs selection sort | Algorithm behavior | Both are `O(n^2)`, but bubble sort performs many more swaps. |
| Without prefetch vs with prefetch | Memory access hinting | Software prefetch can help in some patterns, but results are hardware-dependent. |
| Heap vs stack allocation | Allocation overhead | Stack allocation avoids repeated `malloc` and `free` bookkeeping. |

## Metrics Explained

- **Time Elapsed:** Wall-clock runtime. Lower is usually better.
- **CPU Cycles:** Processor cycles spent during execution. Lower is usually better.
- **Instructions:** Number of executed instructions. Lower can indicate a smaller work path.
- **IPC:** Instructions per cycle. Higher often indicates better CPU pipeline utilization.
- **Branch Misses:** Failed branch predictions. Lower usually means less wasted speculative work.
- **Temperature/Frequency:** Fairness indicators only. They are not used to normalize the final score because CPU thermal behavior is non-linear and hardware-dependent.

## Why Temperature Is Not Used For Normalization

Temperature is partly an effect of the workload itself, not just an external
condition. CPU frequency also depends on turbo boost, power limits, cooling,
governor settings, and throttling thresholds. Because this relationship is not
linear, the tool reports temperature and frequency as context instead of using a
misleading formula.

## Sample Output Shape

```text
CLI Tool For Relative Benchmarking
Select a built-in benchmark pair:

  1. Row-major vs column-major
     Concept: cache locality in C arrays

Enter choice [1-4]: 1
Duration in seconds [10]: 10

[1/2] Profiling ./testcases/01_row_major for up to 10 seconds...
[2/2] Profiling ./testcases/02_col_major for up to 10 seconds...

=========================================================================
                       BENCHMARK PERFORMANCE COMPARISON
=========================================================================
Metric               | 01_row_major       | 02_col_major       | Diff % vs 1st
-------------------------------------------------------------------------
Time Elapsed (s)     | 0.1200             | 0.3500             | +191.7%
CPU Cycles           | 400000000          | 1100000000         | +175.0%
Instructions         | 90000000           | 90000000           | +0.0%
IPC                  | 0.23               | 0.08               | -65.2%
Branch Misses        | 2000               | 2500               | +25.0%
=========================================================================
```

Actual numbers depend on the CPU, OS, cooling, and background processes.

## Rubric Mapping

- **Clarity:** The project has a clear Linux CLI goal and measurable output.
- **Correctness:** The tool validates perf output, handles missing binaries, and reports failures clearly.
- **C Constructs:** Uses structs, functions, strings, file I/O, command-line arguments, arrays, pointers, and dynamic allocation.
- **Organization:** Final source file, Makefile, cleaned command guide, and separate testcases.
- **Demonstration:** Four ready comparison pairs plus metric explanations for viva questions.

## Limitations

- Requires Linux and `perf`; it is not intended as a Windows-native tool.
- Hardware counters may need permission changes or `sudo`.
- Benchmark values vary across machines, CPU governors, and background load.
- Thermal/frequency values may show `N/A` if the Linux system does not expose them.

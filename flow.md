# Command Flow For Demo

## Build Everything

Ubuntu/Debian dependencies:

```sh
sudo apt update
sudo apt install gcc make linux-tools-common linux-tools-generic util-linux
```

Fedora dependencies:

```sh
sudo dnf install gcc make perf util-linux
```

Build:

```sh
make clean
make
```

## Show Help

```sh
./perfcmp --help
```

## Run Demonstration Pairs

Interactive menu:

```sh
./perfcmp
```

Or explicitly open demo mode:

```sh
./perfcmp --demo
```

Direct commands, useful when you want to show exact pair paths:

```sh
./perfcmp --duration 10 ./testcases/01_row_major ./testcases/02_col_major
./perfcmp --duration 10 ./testcases/03_bubble_sort ./testcases/04_selection_sort
./perfcmp --duration 10 ./testcases/05_without_prefetch ./testcases/06_with_prefetch
./perfcmp --duration 10 ./testcases/07_malloc_alloc ./testcases/08_stack_alloc
```

More stable benchmark mode:

```sh
./perfcmp --duration 10 --runs 5 --pin-core 0 ./testcases/01_row_major ./testcases/02_col_major
```

Cooldown examples:

```sh
./perfcmp --runs 3 --cooldown 10 ./testcases/03_bubble_sort ./testcases/04_selection_sort
./perfcmp --runs 3 --auto-cooldown ./testcases/01_row_major ./testcases/02_col_major
```

Auto-cooldown polls the CPU temperature sensor and waits until the CPU
cools back to baseline before each measurement run.

## If perf Permission Fails

```sh
sudo ./perfcmp --duration 10 ./testcases/01_row_major ./testcases/02_col_major
```

Explain that `perf` may require elevated permissions depending on the Linux
kernel's `perf_event_paranoid` setting.

## Run Scaling Analysis

With default sizes per pair:

```sh
./perfcmp --scaling ./testcases/03_bubble_sort ./testcases/04_selection_sort
```

With custom sizes:

```sh
./perfcmp --scaling --sizes 1000,2000,4000,8000 ./testcases/01_row_major ./testcases/02_col_major
```

This runs each binary at multiple input sizes and estimates the empirical
Big O complexity using log-log regression.


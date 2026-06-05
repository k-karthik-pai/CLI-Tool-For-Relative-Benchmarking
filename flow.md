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

## If perf Permission Fails

```sh
sudo ./perfcmp --duration 10 ./testcases/01_row_major ./testcases/02_col_major
```

Explain that `perf` may require elevated permissions depending on the Linux
kernel's `perf_event_paranoid` setting.

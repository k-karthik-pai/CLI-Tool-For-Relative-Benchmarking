/*
 * perfcmp.c — A Linux CLI tool for relative benchmarking of two binaries.
 *
 * Uses `perf stat` to collect hardware performance counters (cycles,
 * instructions, branch-misses) and wall-clock time for each binary,
 * then prints a side-by-side comparison table with percentage deltas.
 *
 * Features:
 *   - Warm-up pass to prime caches before measurement.
 *   - Configurable run count with averaged results.
 *   - Fixed or temperature-based auto-cooldown between runs.
 *   - Optional CPU core pinning via taskset.
 *   - Built-in demo pairs for common performance experiments.
 *   - Algorithmic scaling analysis (--scaling) with O() estimation.
 *
 * Requirements: Linux, gcc, make, GNU timeout, perf.
 */

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/wait.h>
#endif
#include <unistd.h>

#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_RESET   "\x1b[0m"

/* Global flag: 1 = emit ANSI color escape codes, 0 = plain text output. */
static int g_use_color = 1;

/*
 * Signal-safe temporary file cleanup.
 *
 * g_cleanup_logs holds up to 3 temp log file paths that should be deleted
 * if the process is interrupted (e.g. SIGINT, SIGTERM).  cleanup_on_signal
 * is registered as the signal handler; it unlinks any non-empty paths and
 * exits with 128 + signal number (standard UNIX convention).
 *
 * Only async-signal-safe functions (unlink, _exit) are called here.
 */
static char g_cleanup_logs[3][128];

static void cleanup_on_signal(int sig) {
    int i;

    for (i = 0; i < 3; i++) {
        if (g_cleanup_logs[i][0] != '\0') {
            unlink(g_cleanup_logs[i]);
        }
    }

    _exit(128 + sig);
}

#define DEFAULT_DURATION_SECONDS 10
#define DEFAULT_RUN_COUNT 1
#define DEFAULT_COOLDOWN_SECONDS 5
#define MIN_DURATION_SECONDS 1
#define MAX_DURATION_SECONDS 3600
#define MIN_RUN_COUNT 1
#define MAX_RUN_COUNT 10
#define MIN_COOLDOWN_SECONDS 0
#define MAX_COOLDOWN_SECONDS 120
#define MIN_CORE_ID 0
#define MAX_CORE_ID 255
#define TEMP_WARNING_DELTA_C 5.0
#define FREQ_WARNING_DELTA_MHZ 150.0
#define LOG_PATH_SIZE 128
#define CMD_SIZE (PATH_MAX * 2 + 512)
#define METRIC_TABLE_LINE "=============================================================================="
#define CONTEXT_TABLE_LINE "=============================================================================================="
#define MAX_SCALE_POINTS 10
#define DEFAULT_SCALE_POINTS 5
#define SCALING_TIMEOUT_SECONDS 120

/* Auto-cooldown settings */
#define AUTO_COOLDOWN_TEMP_TOLERANCE_C  1.0   /* degrees: close enough to baseline */
#define AUTO_COOLDOWN_POLL_INTERVAL_S   5     /* seconds between temp polls          */
#define AUTO_COOLDOWN_TIMEOUT_SECONDS   300   /* give up after this long             */

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* ══════════════════════════════════════════════════════════════════════════
 *  Data structures
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * PerfMetrics — Raw hardware counter values extracted from a perf stat log.
 * Each field has a companion has_* flag that is set to 1 when the value was
 * successfully parsed.  IPC is derived (instructions / cycles).
 */
typedef struct {
    unsigned long long cycles;
    unsigned long long instructions;
    unsigned long long branch_misses;
    double seconds;
    double ipc;
    int has_cycles;
    int has_instructions;
    int has_branch_misses;
    int has_seconds;
    int has_ipc;
} PerfMetrics;

/*
 * SystemContext — Snapshot of CPU temperature (°C) and average frequency
 * (MHz) taken before and after a benchmark run to detect thermal throttling
 * or frequency scaling that could skew results.
 */
typedef struct {
    double temperature_c;
    double frequency_mhz;
    int has_temperature;
    int has_frequency;
} SystemContext;

/*
 * BenchmarkRun — Groups everything related to one binary's benchmark:
 * the path to the binary, a short display name, the temp log path used by
 * perf stat, the averaged metrics, and before/after system context snapshots.
 */
typedef struct {
    const char *binary_path;
    const char *display_name;
    const char *log_path;
    PerfMetrics metrics;
    SystemContext before;
    SystemContext after;
} BenchmarkRun;

/*
 * ComplexityClass — Enumeration of algorithmic complexity classes used by
 * the --scaling analysis to classify observed growth rates.
 */
typedef enum {
    COMPLEXITY_CONSTANT,
    COMPLEXITY_LINEAR,
    COMPLEXITY_NLOGN,
    COMPLEXITY_QUADRATIC,
    COMPLEXITY_CUBIC,
    COMPLEXITY_UNKNOWN
} ComplexityClass;

/*
 * ScalingData — Stores elapsed times for both binaries at each input size
 * during a --scaling analysis run.
 */
typedef struct {
    int sizes[MAX_SCALE_POINTS];
    double times1[MAX_SCALE_POINTS];
    double times2[MAX_SCALE_POINTS];
    int count;
} ScalingData;

/*
 * ScalingResult — Outcome of fitting a complexity model to observed scaling
 * data: the best-fit class, the estimated power-law exponent, and R².
 */
typedef struct {
    ComplexityClass best_fit;
    double exponent;
    double r_squared;
} ScalingResult;

/*
 * DemoPair — Describes one built-in demo benchmark pair: display title,
 * paths to both binaries, the concept being demonstrated, the expected
 * complexity notation, and default input sizes for scaling analysis.
 */
typedef struct {
    const char *title;
    const char *binary1;
    const char *binary2;
    const char *concept;
    const char *expected_complexity;
    int default_sizes[DEFAULT_SCALE_POINTS];
} DemoPair;

/*
 * Built-in demo pairs: pre-configured benchmark experiments shipped with
 * the tool.  Each entry contains two binaries that illustrate a specific
 * performance concept (cache locality, sorting cost, prefetch hints, etc.).
 */
static const DemoPair DEMO_PAIRS[] = {
    {
        "Row-major vs column-major",
        "./testcases/01_row_major",
        "./testcases/02_col_major",
        "cache locality in C arrays",
        "O(N^2) vs O(N^2)",
        {512, 1024, 2048, 4096, 6144}
    },
    {
        "Bubble sort vs selection sort",
        "./testcases/03_bubble_sort",
        "./testcases/04_selection_sort",
        "algorithm behavior and swap count",
        "O(N^2) vs O(N^2)",
        {2000, 4000, 8000, 12000, 16000}
    },
    {
        "Without prefetch vs with prefetch",
        "./testcases/05_without_prefetch",
        "./testcases/06_with_prefetch",
        "software prefetch hints",
        "O(N) vs O(N)",
        {1000000, 2000000, 4000000, 8000000, 16000000}
    },
    {
        "Heap allocation vs stack allocation",
        "./testcases/07_malloc_alloc",
        "./testcases/08_stack_alloc",
        "allocation overhead",
        "O(N) vs O(N)",
        {100000, 200000, 500000, 1000000, 2000000}
    }
};

/* ══════════════════════════════════════════════════════════════════════════
 *  CLI usage / help
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * print_usage — Prints full help text including usage patterns, available
 * options, built-in demo pairs, examples, and system requirements.
 *
 * @param program_name  argv[0], used to build example command lines.
 */
static void print_usage(const char *program_name) {
    size_t i;

    printf("CLI Tool For Relative Benchmarking\n\n");
    printf("Usage:\n");
    printf("  %s\n", program_name);
    printf("  %s --demo\n", program_name);
    printf("  %s <binary1> <binary2>\n", program_name);
    printf("  %s --duration <seconds> <binary1> <binary2>\n", program_name);
    printf("  %s --runs <count> --pin-core <core> <binary1> <binary2>\n", program_name);
    printf("  %s --cooldown <seconds> <binary1> <binary2>\n", program_name);
    printf("  %s --auto-cooldown <binary1> <binary2>\n", program_name);
    printf("  %s --scaling <binary1> <binary2>\n", program_name);
    printf("  %s --scaling --sizes <n1,n2,...> <binary1> <binary2>\n", program_name);
    printf("  %s --help\n\n", program_name);
    printf("Demo mode:\n");
    printf("  Running without binary paths opens a menu of built-in testcase pairs.\n");
    printf("  Use full binary paths only when benchmarking your own new programs.\n\n");
    printf("Built-in pairs:\n");
    for (i = 0; i < sizeof(DEMO_PAIRS) / sizeof(DEMO_PAIRS[0]); i++) {
        printf("  %zu. %s (%s)\n", i + 1, DEMO_PAIRS[i].title, DEMO_PAIRS[i].concept);
    }
    printf("\n");
    printf("Options:\n");
    printf("  --duration <s>   Wall-clock budget per run in seconds (default: %d, max: %d).\n",
           DEFAULT_DURATION_SECONDS, MAX_DURATION_SECONDS);
    printf("  --runs <n>       Number of measurement runs to average (default: %d, max: %d).\n",
           DEFAULT_RUN_COUNT, MAX_RUN_COUNT);
    printf("  --cooldown <s>   Seconds to sleep after the warm-up pass and between consecutive\n");
    printf("                   measurement runs (default: %d, 0 to disable).\n",
           DEFAULT_COOLDOWN_SECONDS);
    printf("  --auto-cooldown  After warm-up, record baseline CPU temperature. Before each\n");
    printf("                   subsequent run, poll temperature every %ds and wait until it\n",
           AUTO_COOLDOWN_POLL_INTERVAL_S);
    printf("                   drops back within %.1f C of the baseline (timeout: %ds).\n",
           AUTO_COOLDOWN_TEMP_TOLERANCE_C, AUTO_COOLDOWN_TIMEOUT_SECONDS);
    printf("                   Falls back to --cooldown default if no temperature sensor found.\n");
    printf("                   Mutually exclusive with --cooldown.\n");
    printf("  --pin-core <id>  Bind execution to a specific CPU core (requires taskset).\n");
    printf("  --scaling        Measure elapsed time at multiple input sizes and estimate O().\n");
    printf("  --sizes <list>   Comma-separated input sizes for --scaling (e.g. 1000,2000,4000).\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s\n", program_name);
    printf("  %s --demo\n", program_name);
    printf("  %s ./testcases/01_row_major ./testcases/02_col_major\n", program_name);
    printf("  %s --duration 10 ./testcases/03_bubble_sort ./testcases/04_selection_sort\n", program_name);
    printf("  %s --runs 5 --cooldown 10 --pin-core 0 ./testcases/01_row_major ./testcases/02_col_major\n", program_name);
    printf("  %s --runs 3 --auto-cooldown ./testcases/03_bubble_sort ./testcases/04_selection_sort\n", program_name);
    printf("  %s --scaling ./testcases/03_bubble_sort ./testcases/04_selection_sort\n", program_name);
    printf("  %s --scaling --sizes 1000,2000,4000 ./testcases/01_row_major ./testcases/02_col_major\n\n", program_name);
    printf("Requirements:\n");
    printf("  Linux, gcc, make, GNU timeout, perf, and taskset for --pin-core.\n");
}

/* ══════════════════════════════════════════════════════════════════════════
 *  String / parsing utilities
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * base_name — Extracts the filename component from a full path.
 * Handles both forward-slash and backslash separators.
 *
 * @param path  A file path string (e.g. "/usr/bin/ls" or "C:\\bin\\ls").
 * @return      Pointer into `path` just past the last separator, or `path`
 *              itself if no separator is found.
 */
static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    const char *last_sep = slash;

    if (backslash != NULL && (last_sep == NULL || backslash > last_sep)) {
        last_sep = backslash;
    }

    return (last_sep != NULL) ? last_sep + 1 : path;
}

/*
 * remove_commas — Strips all comma characters from a string in place.
 * Used to normalise perf stat output numbers (e.g. "1,234,567" -> "1234567")
 * before parsing them with sscanf.
 *
 * @param str  The string to modify (modified in place).
 */
static void remove_commas(char *str) {
    size_t read_index = 0;
    size_t write_index = 0;

    while (str[read_index] != '\0') {
        if (str[read_index] != ',') {
            str[write_index++] = str[read_index];
        }
        read_index++;
    }

    str[write_index] = '\0';
}

/*
 * parse_int_in_range — Safely converts a string to an integer and checks
 * that the result lies within [min_value, max_value].  Uses strtol for
 * robust error detection (overflow, trailing characters, empty input).
 *
 * @param text       Null-terminated decimal string to parse.
 * @param min_value  Lower bound (inclusive).
 * @param max_value  Upper bound (inclusive).
 * @param value      Output: the parsed integer on success.
 * @return           1 on success, 0 on any parse error or out-of-range.
 */
static int parse_int_in_range(const char *text, int min_value, int max_value, int *value) {
    char *end = NULL;
    long parsed;

    errno = 0;
    parsed = strtol(text, &end, 10);

    if (errno != 0 || end == text || *end != '\0') {
        return 0;
    }

    if (parsed < min_value || parsed > max_value) {
        return 0;
    }

    *value = (int)parsed;
    return 1;
}

/*
 * parse_duration_seconds — Convenience wrapper: parses a duration value
 * and validates it against MIN/MAX_DURATION_SECONDS.
 *
 * @return  1 on success, 0 on error.
 */
static int parse_duration_seconds(const char *text, int *value) {
    return parse_int_in_range(text, MIN_DURATION_SECONDS, MAX_DURATION_SECONDS, value);
}

/*
 * command_exit_code — Extracts a conventional exit code from the raw
 * status value returned by system().  On POSIX the macros WIFEXITED /
 * WEXITSTATUS / WIFSIGNALED / WTERMSIG are used; on Windows the status
 * value is returned directly.
 *
 * @param status  Raw return value from system().
 * @return        Exit code (0–255), or 128+signal if killed, or -1 on error.
 */
static int command_exit_code(int status) {
    if (status == -1) {
        return -1;
    }

#ifdef _WIN32
    return status;
#else
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }

    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }

    return -1;
#endif
}

/*
 * shell_command_succeeds — Runs a shell command and returns 1 if it exits
 * with code 0, 0 otherwise.  Used to test for tool availability.
 *
 * @param command  Shell command string to execute.
 * @return         1 if exit code == 0, 0 otherwise.
 */
static int shell_command_succeeds(const char *command) {
    int status = system(command);
    return command_exit_code(status) == 0;
}

/*
 * quote_shell_arg — Wraps `input` in POSIX single quotes, escaping any
 * embedded single-quote characters with the '\'' idiom.  This prevents
 * shell injection when the string is interpolated into a system() call.
 *
 * @param input        Raw string to quote.
 * @param output       Buffer to receive the quoted result.
 * @param output_size  Size of `output` buffer.
 * @return             1 on success, 0 if `output` is too small.
 */
static int quote_shell_arg(const char *input, char *output, size_t output_size) {
    size_t pos = 0;
    size_t i;

    if (output_size < 3) {
        return 0;
    }

    output[pos++] = '\'';

    for (i = 0; input[i] != '\0'; i++) {
        if (input[i] == '\'') {
            const char *escaped = "'\\''";
            size_t j;

            for (j = 0; escaped[j] != '\0'; j++) {
                if (pos + 1 >= output_size) {
                    return 0;
                }
                output[pos++] = escaped[j];
            }
        } else {
            if (pos + 1 >= output_size) {
                return 0;
            }
            output[pos++] = input[i];
        }
    }

    if (pos + 1 >= output_size) {
        return 0;
    }

    output[pos++] = '\'';
    output[pos] = '\0';
    return 1;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  System monitoring helpers
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * read_temperature_c — Reads the average CPU temperature from the Linux
 * sysfs thermal zone interface (/sys/class/thermal/thermal_zone[0-5]/temp).
 * Iterates over zones 0–5, averaging all valid readings.  Values reported
 * in millidegrees (>1000) are converted to degrees Celsius.
 *
 * @param temperature_c  Output: average temperature in °C.
 * @return               1 if at least one valid reading was found, 0 otherwise.
 */
static int read_temperature_c(double *temperature_c) {
    const char *paths[] = {
        "/sys/class/thermal/thermal_zone0/temp",
        "/sys/class/thermal/thermal_zone1/temp",
        "/sys/class/thermal/thermal_zone2/temp",
        "/sys/class/thermal/thermal_zone3/temp",
        "/sys/class/thermal/thermal_zone4/temp",
        "/sys/class/thermal/thermal_zone5/temp"
    };
    double total = 0.0;
    int count = 0;
    size_t i;

    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        FILE *fp = fopen(paths[i], "r");
        long raw_temp;

        if (fp == NULL) {
            continue;
        }

        if (fscanf(fp, "%ld", &raw_temp) == 1) {
            double temp = raw_temp > 1000 ? raw_temp / 1000.0 : (double)raw_temp;

            if (temp > 0.0 && temp < 150.0) {
                total += temp;
                count++;
            }
        }

        fclose(fp);
    }

    if (count == 0) {
        return 0;
    }

    *temperature_c = total / count;
    return 1;
}

/*
 * read_average_frequency_mhz — Reads average CPU frequency by parsing
 * "cpu MHz" lines from /proc/cpuinfo.  Averages all cores' frequencies.
 *
 * @param frequency_mhz  Output: average frequency in MHz.
 * @return               1 on success, 0 if /proc/cpuinfo is unavailable
 *                       or contains no frequency entries.
 */
static int read_average_frequency_mhz(double *frequency_mhz) {
    FILE *fp = fopen("/proc/cpuinfo", "r");
    char line[256];
    double total = 0.0;
    int count = 0;

    if (fp == NULL) {
        return 0;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        double mhz;

        if (sscanf(line, "cpu MHz : %lf", &mhz) == 1) {
            if (mhz > 0.0) {
                total += mhz;
                count++;
            }
        }
    }

    fclose(fp);

    if (count == 0) {
        return 0;
    }

    *frequency_mhz = total / count;
    return 1;
}

/*
 * capture_system_context — Takes a point-in-time snapshot of CPU temperature
 * and frequency.  Called before and after benchmark runs so the comparison
 * table can flag thermal throttling or frequency drift.
 *
 * @return  A SystemContext struct with the readings and validity flags.
 */
static SystemContext capture_system_context(void) {
    SystemContext context;

    context.temperature_c = 0.0;
    context.frequency_mhz = 0.0;
    context.has_temperature = read_temperature_c(&context.temperature_c);
    context.has_frequency = read_average_frequency_mhz(&context.frequency_mhz);

    return context;
}

/*
 * wait_for_temp_baseline: polls CPU temperature every AUTO_COOLDOWN_POLL_INTERVAL_S
 * seconds until the reading drops to within AUTO_COOLDOWN_TEMP_TOLERANCE_C of
 * baseline_temp_c, or until AUTO_COOLDOWN_TIMEOUT_SECONDS elapses.
 *
 * Returns 1 if temperature recovered (or sensor unavailable — caller falls back).
 * Returns 0 only on hard timeout; in that case the caller should still proceed
 * but may want to note the warning already printed here.
 *
 * binary_number / run_number are used only for the progress line prefix so the
 * output style is consistent with the rest of the tool.
 */
static int wait_for_temp_baseline(double baseline_temp_c,
                                   int binary_number,
                                   int run_number,
                                   int run_count) {
    int elapsed = 0;
    double current_temp;

    /* Sensor unavailable: caller falls back to fixed cooldown. */
    if (!read_temperature_c(&current_temp)) {
        return 1;
    }

    if (current_temp <= baseline_temp_c + AUTO_COOLDOWN_TEMP_TOLERANCE_C) {
        return 1;   /* already cool enough, no wait needed */
    }

    printf("[%d/2 run %d/%d] Auto-cooldown: waiting for temp to drop from %.1f C "
           "back to baseline %.1f C (tolerance +/-%.1f C, timeout %ds) ...\n",
           binary_number, run_number, run_count,
           current_temp, baseline_temp_c,
           AUTO_COOLDOWN_TEMP_TOLERANCE_C, AUTO_COOLDOWN_TIMEOUT_SECONDS);

    while (elapsed < AUTO_COOLDOWN_TIMEOUT_SECONDS) {
        sleep((unsigned int)AUTO_COOLDOWN_POLL_INTERVAL_S);
        elapsed += AUTO_COOLDOWN_POLL_INTERVAL_S;

        if (!read_temperature_c(&current_temp)) {
            /* Sensor disappeared mid-wait; give up waiting. */
            return 1;
        }

        printf("[%d/2 run %d/%d] Auto-cooldown: %.1f C  (target <= %.1f C, waited %ds)\n",
               binary_number, run_number, run_count,
               current_temp,
               baseline_temp_c + AUTO_COOLDOWN_TEMP_TOLERANCE_C,
               elapsed);

        if (current_temp <= baseline_temp_c + AUTO_COOLDOWN_TEMP_TOLERANCE_C) {
            printf("[%d/2 run %d/%d] Temperature recovered to %.1f C. Proceeding.\n",
                   binary_number, run_number, run_count, current_temp);
            return 1;
        }
    }

    printf("%sWarning: auto-cooldown timed out after %ds; temperature is still %.1f C "
           "(baseline %.1f C). Proceeding anyway.\n%s",
           g_use_color ? ANSI_COLOR_YELLOW : "",
           AUTO_COOLDOWN_TIMEOUT_SECONDS, current_temp, baseline_temp_c,
           g_use_color ? ANSI_COLOR_RESET : "");
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Perf stat integration
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * parse_perf_file — Opens a perf stat log file and extracts hardware
 * counter values (cycles, instructions, branch-misses) and elapsed time.
 * Commas in numeric output are stripped before parsing.  If both cycles
 * and instructions are present, IPC is computed as instructions/cycles.
 *
 * @param filename  Path to the perf stat output file.
 * @param metrics   Output: populated PerfMetrics struct.
 * @return          1 on success, 0 if the file cannot be opened or the
 *                  elapsed-time field is missing.
 */
static int parse_perf_file(const char *filename, PerfMetrics *metrics) {
    FILE *fp = fopen(filename, "r");
    char line[512];

    if (fp == NULL) {
        fprintf(stderr, "Error: Could not open performance log file %s\n", filename);
        return 0;
    }

    memset(metrics, 0, sizeof(*metrics));

    while (fgets(line, sizeof(line), fp) != NULL) {
        unsigned long long value_ull;
        double value_double;

        remove_commas(line);

        if (strstr(line, "branch-misses") != NULL) {
            if (sscanf(line, " %llu", &value_ull) == 1) {
                metrics->branch_misses = value_ull;
                metrics->has_branch_misses = 1;
            }
        } else if (strstr(line, "instructions") != NULL) {
            if (sscanf(line, " %llu", &value_ull) == 1) {
                metrics->instructions = value_ull;
                metrics->has_instructions = 1;
            }
        } else if (strstr(line, "cycles") != NULL) {
            if (sscanf(line, " %llu", &value_ull) == 1) {
                metrics->cycles = value_ull;
                metrics->has_cycles = 1;
            }
        } else if (strstr(line, "seconds time elapsed") != NULL) {
            if (sscanf(line, " %lf", &value_double) == 1) {
                metrics->seconds = value_double;
                metrics->has_seconds = 1;
            }
        }
    }

    fclose(fp);

    if (metrics->has_cycles && metrics->has_instructions && metrics->cycles > 0) {
        metrics->ipc = (double)metrics->instructions / (double)metrics->cycles;
        metrics->has_ipc = 1;
    }

    if (!metrics->has_seconds) {
        fprintf(stderr, "Error: perf log %s did not contain elapsed time.\n", filename);
        return 0;
    }

    return 1;
}

/*
 * print_common_perf_failure_help — Prints a checklist of common reasons
 * why perf stat might fail (missing packages, permissions, missing tools,
 * bad binary).  Called after a perf command returns a non-zero exit code.
 */
static void print_common_perf_failure_help(void) {
    fprintf(stderr, "\nCommon causes:\n");
    fprintf(stderr, "  - perf is not installed. Ubuntu: sudo apt install linux-tools-common linux-tools-generic\n");
    fprintf(stderr, "  - perf is not installed. Fedora: sudo dnf install perf\n");
    fprintf(stderr, "  - perf_event permissions are restricted. Try running with sudo or adjusting perf_event_paranoid.\n");
    fprintf(stderr, "  - GNU timeout is missing.\n");
    fprintf(stderr, "  - taskset is missing, if --pin-core is enabled.\n");
    fprintf(stderr, "  - The benchmark binary crashed or does not have execute permission.\n");
}

/*
 * run_perf_stat — Constructs and executes a `perf stat` command line that
 * profiles the given binary for up to `duration_seconds`, optionally
 * pinned to a specific CPU core.  Output is written to `log_path`.
 * Exit code 124 (GNU timeout) is treated as success (binary ran for the
 * full duration and was killed).
 *
 * @param binary_path       Path to the benchmark binary.
 * @param log_path          Temp file path for perf stat output.
 * @param duration_seconds  Wall-clock budget via GNU `timeout`.
 * @param pin_core          CPU core ID for taskset, or -1 to skip pinning.
 * @param extra_arg         Optional extra argument appended to the binary
 *                          invocation (e.g. an input size), or NULL.
 * @return                  1 on success, 0 on failure.
 */
static int run_perf_stat(const char *binary_path,
                         const char *log_path,
                         int duration_seconds,
                         int pin_core,
                         const char *extra_arg) {
    char quoted_binary[PATH_MAX + 128];
    char quoted_log[PATH_MAX + 128];
    char command[CMD_SIZE];
    char arg_suffix[64];
    int status;
    int exit_code;

    if (!quote_shell_arg(binary_path, quoted_binary, sizeof(quoted_binary)) ||
        !quote_shell_arg(log_path, quoted_log, sizeof(quoted_log))) {
        fprintf(stderr, "Error: path is too long to quote safely.\n");
        return 0;
    }

    arg_suffix[0] = '\0';
    if (extra_arg != NULL && extra_arg[0] != '\0') {
        snprintf(arg_suffix, sizeof(arg_suffix), " %s", extra_arg);
    }

    if (pin_core >= 0) {
        if (snprintf(command, sizeof(command),
                     "perf stat -e cycles,instructions,branch-misses -o %s "
                     "timeout %ds taskset -c %d %s%s > /dev/null 2>&1",
                     quoted_log, duration_seconds, pin_core,
                     quoted_binary, arg_suffix) >= (int)sizeof(command)) {
            fprintf(stderr, "Error: generated perf command is too long.\n");
            return 0;
        }
    } else {
        if (snprintf(command, sizeof(command),
                     "perf stat -e cycles,instructions,branch-misses -o %s "
                     "timeout %ds %s%s > /dev/null 2>&1",
                     quoted_log, duration_seconds,
                     quoted_binary, arg_suffix) >= (int)sizeof(command)) {
            fprintf(stderr, "Error: generated perf command is too long.\n");
            return 0;
        }
    }

    status = system(command);
    exit_code = command_exit_code(status);

    if (exit_code == 0 || exit_code == 124) {
        return 1;
    }

    fprintf(stderr, "Error: perf command failed for %s (exit code %d).\n", binary_path, exit_code);
    print_common_perf_failure_help();
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Multi-run metric accumulation
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * add_metric_sample — Accumulates one benchmark run's metrics into running
 * totals.  Each counter is only added when its has_* flag is set, and the
 * corresponding per-counter count is incremented so that
 * finalize_average_metrics can compute correct averages.
 *
 * @param total               Running totals (modified in place).
 * @param sample              Metrics from the latest single run.
 * @param cycles_count        Number of runs that contributed cycle data.
 * @param instructions_count  Number of runs that contributed instruction data.
 * @param branch_misses_count Number of runs that contributed branch-miss data.
 * @param seconds_count       Number of runs that contributed elapsed time.
 */
static void add_metric_sample(PerfMetrics *total,
                              const PerfMetrics *sample,
                              int *cycles_count,
                              int *instructions_count,
                              int *branch_misses_count,
                              int *seconds_count) {
    if (sample->has_cycles) {
        total->cycles += sample->cycles;
        (*cycles_count)++;
    }

    if (sample->has_instructions) {
        total->instructions += sample->instructions;
        (*instructions_count)++;
    }

    if (sample->has_branch_misses) {
        total->branch_misses += sample->branch_misses;
        (*branch_misses_count)++;
    }

    if (sample->has_seconds) {
        total->seconds += sample->seconds;
        (*seconds_count)++;
    }
}

/*
 * finalize_average_metrics — Divides the accumulated metric totals by their
 * respective run counts to produce averaged results.  Uses rounded integer
 * division (adding count/2 before dividing) for the unsigned-long-long
 * counters to avoid systematic truncation bias.  Also recomputes IPC from
 * the averaged cycle and instruction counts.
 *
 * @param metrics             In/out: accumulated totals on entry, averages
 *                            on exit.
 * @param cycles_count        Runs contributing cycles.
 * @param instructions_count  Runs contributing instructions.
 * @param branch_misses_count Runs contributing branch misses.
 * @param seconds_count       Runs contributing elapsed time.
 * @param run_count           Expected total number of runs.
 * @return                    1 on success, 0 if seconds_count != run_count
 *                            (indicates a missing elapsed-time measurement).
 */
static int finalize_average_metrics(PerfMetrics *metrics,
                                    int cycles_count,
                                    int instructions_count,
                                    int branch_misses_count,
                                    int seconds_count,
                                    int run_count) {
    if (seconds_count != run_count) {
        fprintf(stderr, "Error: elapsed time was missing from one or more benchmark runs.\n");
        return 0;
    }

    metrics->has_seconds = seconds_count > 0;
    metrics->has_cycles = cycles_count > 0;
    metrics->has_instructions = instructions_count > 0;
    metrics->has_branch_misses = branch_misses_count > 0;

    if (metrics->has_cycles) {
        metrics->cycles = (metrics->cycles + (unsigned long long)(cycles_count / 2)) /
                          (unsigned long long)cycles_count;
    }

    if (metrics->has_instructions) {
        metrics->instructions = (metrics->instructions + (unsigned long long)(instructions_count / 2)) /
                                (unsigned long long)instructions_count;
    }

    if (metrics->has_branch_misses) {
        metrics->branch_misses = (metrics->branch_misses + (unsigned long long)(branch_misses_count / 2)) /
                                 (unsigned long long)branch_misses_count;
    }

    if (metrics->has_seconds) {
        metrics->seconds /= seconds_count;
    }

    if (metrics->has_cycles && metrics->has_instructions && metrics->cycles > 0) {
        metrics->ipc = (double)metrics->instructions / (double)metrics->cycles;
        metrics->has_ipc = 1;
    }

    return 1;
}

/*
 * run_benchmark: runs one binary through a warm-up pass followed by run_count
 * measured passes, with cooldown between runs.
 *
 * Cooldown modes (mutually exclusive, controlled by auto_cooldown flag):
 *
 *   Fixed cooldown (auto_cooldown == 0, cooldown_seconds > 0):
 *     Sleeps for cooldown_seconds after warm-up and between each pair of
 *     consecutive measured runs.
 *
 *   Auto-cooldown (auto_cooldown != 0):
 *     After the warm-up, records the CPU temperature as a baseline (x).
 *     Before each subsequent run the tool polls the temperature every
 *     AUTO_COOLDOWN_POLL_INTERVAL_S seconds and only proceeds when the
 *     temperature has dropped back to within AUTO_COOLDOWN_TEMP_TOLERANCE_C
 *     of x.  If no temperature sensor is available it falls back to a fixed
 *     cooldown of DEFAULT_COOLDOWN_SECONDS and emits a warning.  If the
 *     temperature does not recover within AUTO_COOLDOWN_TIMEOUT_SECONDS the
 *     tool proceeds with a yellow warning.
 *
 * System context (before / after):
 *   Captured after the warm-up cooldown (fixed) or after the first
 *   auto-cooldown wait, so the "before" snapshot reflects the thermal
 *   steady-state at the actual start of measurement.
 */
static int run_benchmark(BenchmarkRun *run,
                         int duration_seconds,
                         int binary_number,
                         int run_count,
                         int pin_core,
                         int cooldown_seconds,
                         int auto_cooldown) {
    PerfMetrics totals;
    int cycles_count = 0;
    int instructions_count = 0;
    int branch_misses_count = 0;
    int seconds_count = 0;
    double baseline_temp_c = 0.0;
    int has_baseline_temp = 0;
    int i;

    memset(&totals, 0, sizeof(totals));

    /* ── Warm-up pass ──────────────────────────────────────────────────────── */
    printf("[%d/2 warmup] Warming up %s for up to %d seconds ...\n",
           binary_number, run->binary_path, duration_seconds);
    unlink(run->log_path);
    if (!run_perf_stat(run->binary_path, run->log_path, duration_seconds, pin_core, NULL)) {
        return 0;
    }
    unlink(run->log_path);  /* result intentionally discarded */

    /* ── Post-warmup cooldown ──────────────────────────────────────────────── */
    if (auto_cooldown) {
        /*
         * Record baseline temperature immediately after warm-up finishes.
         * This is the "x" that subsequent runs will cool back down to.
         */
        has_baseline_temp = read_temperature_c(&baseline_temp_c);

        if (!has_baseline_temp) {
            printf("%sWarning: --auto-cooldown requested but no CPU temperature sensor found. "
                   "Falling back to fixed %ds cooldown.\n%s",
                   g_use_color ? ANSI_COLOR_YELLOW : "",
                   DEFAULT_COOLDOWN_SECONDS,
                   g_use_color ? ANSI_COLOR_RESET : "");
            /* Fall back: treat exactly like fixed cooldown. */
            printf("[%d/2 warmup] Cooling down for %d s before measurement ...\n",
                   binary_number, DEFAULT_COOLDOWN_SECONDS);
            sleep((unsigned int)DEFAULT_COOLDOWN_SECONDS);
        } else {
            printf("[%d/2 warmup] Auto-cooldown baseline captured: %.1f C. "
                   "Waiting for temperature to settle ...\n",
                   binary_number, baseline_temp_c);
            /*
             * Wait until the core is back at the post-warmup baseline before
             * the first measurement run.  This handles CPUs that briefly spike
             * higher during the warmup pass itself.
             */
            wait_for_temp_baseline(baseline_temp_c, binary_number, 1, run_count);
        }
    } else if (cooldown_seconds > 0) {
        printf("[%d/2 warmup] Cooling down for %d s before measurement ...\n",
               binary_number, cooldown_seconds);
        sleep((unsigned int)cooldown_seconds);
    }

    /* Capture the "before" snapshot after the warm-up cooldown. */
    run->before = capture_system_context();

    /* ── Measurement passes ────────────────────────────────────────────────── */
    for (i = 1; i <= run_count; i++) {
        PerfMetrics sample;

        printf("[%d/2 run %d/%d] Profiling %s for up to %d seconds",
               binary_number, i, run_count, run->binary_path, duration_seconds);
        if (pin_core >= 0) {
            printf(" on CPU core %d", pin_core);
        }
        printf(" ...\n");

        unlink(run->log_path);

        if (!run_perf_stat(run->binary_path, run->log_path, duration_seconds, pin_core, NULL)) {
            return 0;
        }

        if (!parse_perf_file(run->log_path, &sample)) {
            return 0;
        }

        add_metric_sample(&totals, &sample, &cycles_count, &instructions_count,
                          &branch_misses_count, &seconds_count);

        /* Cooldown between runs (skip after the last one). */
        if (i < run_count) {
            if (auto_cooldown) {
                if (has_baseline_temp) {
                    wait_for_temp_baseline(baseline_temp_c,
                                           binary_number, i + 1, run_count);
                } else {
                    /* No sensor: fixed fallback. */
                    printf("[%d/2 run %d/%d] Cooling down for %d s before next run ...\n",
                           binary_number, i, run_count, DEFAULT_COOLDOWN_SECONDS);
                    sleep((unsigned int)DEFAULT_COOLDOWN_SECONDS);
                }
            } else if (cooldown_seconds > 0) {
                printf("[%d/2 run %d/%d] Cooling down for %d s before next run ...\n",
                       binary_number, i, run_count, cooldown_seconds);
                sleep((unsigned int)cooldown_seconds);
            }
        }
    }

    run->after = capture_system_context();
    run->metrics = totals;

    return finalize_average_metrics(&run->metrics, cycles_count, instructions_count,
                                    branch_misses_count, seconds_count, run_count);
}

/*
 * format_ull_metric: formats an unsigned long long metric value into a
 * display string.  If the metric was not collected (available == 0), the
 * buffer is filled with "N/A".
 *
 *   available    - whether the metric was successfully measured
 *   value        - the raw metric value
 *   buffer       - output character buffer for the formatted string
 *   buffer_size  - size of the output buffer in bytes
 */
static void format_ull_metric(int available, unsigned long long value, char *buffer, size_t buffer_size) {
    if (!available) {
        snprintf(buffer, buffer_size, "N/A");
    } else {
        snprintf(buffer, buffer_size, "%llu", value);
    }
}

/*
 * format_double_metric: formats a double-precision metric value into a
 * display string with a given number of decimal places.  If the metric
 * was not collected (available == 0), the buffer is filled with "N/A".
 *
 *   available    - whether the metric was successfully measured
 *   value        - the raw metric value
 *   precision    - number of digits after the decimal point
 *   buffer       - output character buffer for the formatted string
 *   buffer_size  - size of the output buffer in bytes
 */
static void format_double_metric(int available, double value, int precision, char *buffer, size_t buffer_size) {
    if (!available) {
        snprintf(buffer, buffer_size, "N/A");
    } else {
        snprintf(buffer, buffer_size, "%.*f", precision, value);
    }
}

/*
 * print_metric_row: prints a single row of the benchmark comparison table.
 * Computes the percentage difference between the two raw values and
 * color-codes the output: green if the second binary is better, red if
 * worse.  When values are unavailable or the first value is zero, the
 * diff column shows "N/A".
 *
 *   metric_name      - label for this row (e.g. "CPU Cycles")
 *   value1           - pre-formatted string for binary 1
 *   value2           - pre-formatted string for binary 2
 *   values_available - 1 if both metrics were collected, 0 otherwise
 *   raw1, raw2       - raw numeric values used to compute percentage diff
 *   lower_is_better  - 1 if a lower value indicates better performance
 */
static void print_metric_row(const char *metric_name,
                             const char *value1,
                             const char *value2,
                             int values_available,
                             double raw1,
                             double raw2,
                             int lower_is_better) {
    double pct_diff = 0.0;

    printf("%-20s | %-18s | ", metric_name, value1);

    if (!values_available || raw1 == 0.0) {
        printf("%-18s | %-12s\n", value2, "N/A");
        return;
    }

    pct_diff = ((raw2 - raw1) / raw1) * 100.0;

    if (raw1 == raw2) {
        printf("%-18s | %-+11.1f%%\n", value2, pct_diff);
    } else {
        int second_is_better = lower_is_better ? (raw2 < raw1) : (raw2 > raw1);

        if (second_is_better) {
            printf("%s%-18s%s | %s%-+11.1f%%%s\n",
                   g_use_color ? ANSI_COLOR_GREEN : "", value2,
                   g_use_color ? ANSI_COLOR_RESET : "",
                   g_use_color ? ANSI_COLOR_GREEN : "", pct_diff,
                   g_use_color ? ANSI_COLOR_RESET : "");
        } else {
            printf("%s%-18s%s | %s%-+11.1f%%%s\n",
                   g_use_color ? ANSI_COLOR_RED : "", value2,
                   g_use_color ? ANSI_COLOR_RESET : "",
                   g_use_color ? ANSI_COLOR_RED : "", pct_diff,
                   g_use_color ? ANSI_COLOR_RESET : "");
        }
    }
}

/*
 * print_metric_table: prints the full benchmark performance comparison
 * table.  Each row shows a perf metric (time, cycles, instructions, IPC,
 * branch misses) for both binaries side-by-side with a percentage diff
 * column.  Uses format_ull_metric / format_double_metric for formatting
 * and print_metric_row for color-coded output.
 *
 *   run1 - results from the first binary
 *   run2 - results from the second binary
 */
static void print_metric_table(const BenchmarkRun *run1, const BenchmarkRun *run2) {
    char value1[64];
    char value2[64];

    printf("\n%s\n", METRIC_TABLE_LINE);
    printf("                         BENCHMARK PERFORMANCE COMPARISON                 \n");
    printf("%s\n", METRIC_TABLE_LINE);
    printf("%-20s | %-18.18s | %-18.18s | %-13s\n",
           "Metric", run1->display_name, run2->display_name, "Diff % vs 1st");
    printf("%s\n", METRIC_TABLE_LINE);

    format_double_metric(run1->metrics.has_seconds, run1->metrics.seconds, 4, value1, sizeof(value1));
    format_double_metric(run2->metrics.has_seconds, run2->metrics.seconds, 4, value2, sizeof(value2));
    print_metric_row("Time Elapsed (s)", value1, value2,
                     run1->metrics.has_seconds && run2->metrics.has_seconds,
                     run1->metrics.seconds, run2->metrics.seconds, 1);

    format_ull_metric(run1->metrics.has_cycles, run1->metrics.cycles, value1, sizeof(value1));
    format_ull_metric(run2->metrics.has_cycles, run2->metrics.cycles, value2, sizeof(value2));
    print_metric_row("CPU Cycles", value1, value2,
                     run1->metrics.has_cycles && run2->metrics.has_cycles,
                     (double)run1->metrics.cycles, (double)run2->metrics.cycles, 1);

    format_ull_metric(run1->metrics.has_instructions, run1->metrics.instructions, value1, sizeof(value1));
    format_ull_metric(run2->metrics.has_instructions, run2->metrics.instructions, value2, sizeof(value2));
    print_metric_row("Instructions", value1, value2,
                     run1->metrics.has_instructions && run2->metrics.has_instructions,
                     (double)run1->metrics.instructions, (double)run2->metrics.instructions, 1);

    format_double_metric(run1->metrics.has_ipc, run1->metrics.ipc, 2, value1, sizeof(value1));
    format_double_metric(run2->metrics.has_ipc, run2->metrics.ipc, 2, value2, sizeof(value2));
    print_metric_row("IPC", value1, value2,
                     run1->metrics.has_ipc && run2->metrics.has_ipc,
                     run1->metrics.ipc, run2->metrics.ipc, 0);

    format_ull_metric(run1->metrics.has_branch_misses, run1->metrics.branch_misses, value1, sizeof(value1));
    format_ull_metric(run2->metrics.has_branch_misses, run2->metrics.branch_misses, value2, sizeof(value2));
    print_metric_row("Branch Misses", value1, value2,
                     run1->metrics.has_branch_misses && run2->metrics.has_branch_misses,
                     (double)run1->metrics.branch_misses, (double)run2->metrics.branch_misses, 1);

    printf("%s\n", METRIC_TABLE_LINE);
}

/*
 * format_context_value: formats a system-context value (e.g. CPU
 * temperature or frequency) into a display string with a unit suffix.
 * Writes "N/A" when the reading is unavailable.
 *
 *   available    - whether the sensor reading was captured
 *   value        - the numeric sensor reading
 *   suffix       - unit string appended after the value (e.g. "C", "MHz")
 *   buffer       - output character buffer
 *   buffer_size  - size of the output buffer in bytes
 */
static void format_context_value(int available,
                                 double value,
                                 const char *suffix,
                                 char *buffer,
                                 size_t buffer_size) {
    if (available) {
        snprintf(buffer, buffer_size, "%.1f %s", value, suffix);
    } else {
        snprintf(buffer, buffer_size, "N/A");
    }
}

/*
 * print_system_context_table: prints a table showing the CPU temperature
 * and frequency captured before and after each binary's measured runs.
 * These values are fairness indicators — they are NOT used to normalize
 * scores, but help the user judge whether thermal throttling or frequency
 * scaling may have biased the comparison.  Emits yellow warnings if the
 * temperature or frequency delta between the two binaries exceeds the
 * configured thresholds.
 *
 *   run1 - results (including before/after context) for binary 1
 *   run2 - results (including before/after context) for binary 2
 */
static void print_system_context_table(const BenchmarkRun *run1, const BenchmarkRun *run2) {
    char temp_before[32];
    char temp_after[32];
    char freq_before[32];
    char freq_after[32];

    printf("\nSystem Context (fairness indicators, not score normalization)\n");
    printf("%s\n", CONTEXT_TABLE_LINE);
    printf("%-18s | %-16s | %-16s | %-16s | %-16s\n",
           "Binary", "Temp Before", "Temp After", "Freq Before", "Freq After");
    printf("%s\n", CONTEXT_TABLE_LINE);

    format_context_value(run1->before.has_temperature, run1->before.temperature_c,
                         "C", temp_before, sizeof(temp_before));
    format_context_value(run1->after.has_temperature, run1->after.temperature_c,
                         "C", temp_after, sizeof(temp_after));
    format_context_value(run1->before.has_frequency, run1->before.frequency_mhz,
                         "MHz", freq_before, sizeof(freq_before));
    format_context_value(run1->after.has_frequency, run1->after.frequency_mhz,
                         "MHz", freq_after, sizeof(freq_after));
    printf("%-18.18s | %-16s | %-16s | %-16s | %-16s\n",
           run1->display_name, temp_before, temp_after, freq_before, freq_after);

    format_context_value(run2->before.has_temperature, run2->before.temperature_c,
                         "C", temp_before, sizeof(temp_before));
    format_context_value(run2->after.has_temperature, run2->after.temperature_c,
                         "C", temp_after, sizeof(temp_after));
    format_context_value(run2->before.has_frequency, run2->before.frequency_mhz,
                         "MHz", freq_before, sizeof(freq_before));
    format_context_value(run2->after.has_frequency, run2->after.frequency_mhz,
                         "MHz", freq_after, sizeof(freq_after));
    printf("%-18.18s | %-16s | %-16s | %-16s | %-16s\n",
           run2->display_name, temp_before, temp_after, freq_before, freq_after);

    printf("%s\n", CONTEXT_TABLE_LINE);

    if (run1->before.has_temperature && run2->before.has_temperature) {
        double temp_delta = run2->before.temperature_c - run1->before.temperature_c;

        if (temp_delta > TEMP_WARNING_DELTA_C) {
            printf("%sWarning: second run started %.1f C hotter; results may have thermal bias.\n%s",
                   g_use_color ? ANSI_COLOR_YELLOW : "",
                   temp_delta,
                   g_use_color ? ANSI_COLOR_RESET : "");
        } else if (temp_delta < -TEMP_WARNING_DELTA_C) {
            printf("%sWarning: first run started %.1f C hotter; results may have thermal bias.\n%s",
                   g_use_color ? ANSI_COLOR_YELLOW : "",
                   -temp_delta,
                   g_use_color ? ANSI_COLOR_RESET : "");
        }
    }

    if (run1->before.has_frequency && run1->after.has_frequency &&
        run2->before.has_frequency && run2->after.has_frequency) {
        double avg_freq1 = (run1->before.frequency_mhz + run1->after.frequency_mhz) / 2.0;
        double avg_freq2 = (run2->before.frequency_mhz + run2->after.frequency_mhz) / 2.0;
        double freq_delta = avg_freq2 - avg_freq1;

        if (freq_delta > FREQ_WARNING_DELTA_MHZ) {
            printf("%sWarning: %s ran with %.1f MHz higher average observed CPU frequency; timing may be biased.\n%s",
                   g_use_color ? ANSI_COLOR_YELLOW : "",
                   run2->display_name, freq_delta,
                   g_use_color ? ANSI_COLOR_RESET : "");
        } else if (freq_delta < -FREQ_WARNING_DELTA_MHZ) {
            printf("%sWarning: %s ran with %.1f MHz higher average observed CPU frequency; timing may be biased.\n%s",
                   g_use_color ? ANSI_COLOR_YELLOW : "",
                   run1->display_name, -freq_delta,
                   g_use_color ? ANSI_COLOR_RESET : "");
        }
    }

    if (!run1->before.has_temperature || !run1->after.has_temperature ||
        !run2->before.has_temperature || !run2->after.has_temperature) {
        printf("Note: CPU temperature was unavailable on this Linux system.\n");
    }

    if (!run1->before.has_frequency || !run1->after.has_frequency ||
        !run2->before.has_frequency || !run2->after.has_frequency) {
        printf("Note: CPU frequency was unavailable on this Linux system.\n");
    }
}

/*
 * relative_difference: computes the relative (percentage-style) difference
 * between two non-negative values.  Returns (larger - smaller) / larger,
 * a value in [0, 1].  Returns 0 when both values are zero.
 *
 *   a, b - the two values to compare
 *
 * Returns: the relative difference as a fraction.
 */
static double relative_difference(double a, double b) {
    double larger = a > b ? a : b;
    double smaller = a > b ? b : a;

    if (larger == 0.0) {
        return 0.0;
    }

    return (larger - smaller) / larger;
}

/*
 * add_metric_vote: adds a weighted vote to the verdict scoring system for
 * one performance metric.  If the metric is available for both binaries
 * and the relative difference exceeds the given threshold, the winning
 * binary's score is incremented by 'weight'.  Ties and differences below
 * the threshold are ignored to avoid noise in the verdict.
 *
 *   score1, score2     - running score accumulators for each binary
 *   available1/2       - whether the metric was collected for each binary
 *   value1, value2     - raw metric values
 *   weight             - points to award for this metric category
 *   threshold          - minimum relative difference to count as a win
 *   lower_is_better    - 1 if a lower value means better performance
 */
static void add_metric_vote(double *score1,
                            double *score2,
                            int available1,
                            int available2,
                            double value1,
                            double value2,
                            double weight,
                            double threshold,
                            int lower_is_better) {
    if (!available1 || !available2) {
        return;
    }

    if (relative_difference(value1, value2) < threshold) {
        return;
    }

    if (lower_is_better) {
        if (value1 < value2) {
            *score1 += weight;
        } else if (value2 < value1) {
            *score2 += weight;
        }
    } else {
        if (value1 > value2) {
            *score1 += weight;
        } else if (value2 > value1) {
            *score2 += weight;
        }
    }
}

/*
 * print_metric_explanations: prints a brief guide explaining what each
 * performance metric means and whether lower or higher is better.  This
 * helps users who are unfamiliar with perf counters interpret the table.
 */
static void print_metric_explanations(void) {
    printf("\nMetric Guide\n");
    printf("  Time Elapsed : Real wall-clock runtime. Lower is better.\n");
    printf("  CPU Cycles   : Number of processor cycles spent. Lower is better.\n");
    printf("  Instructions : Number of executed instructions. Lower can indicate a smaller work path.\n");
    printf("  IPC          : Instructions per cycle. Higher often means better CPU pipeline use.\n");
    printf("  Branch Misses: Failed branch predictions. Lower usually means less wasted work.\n");
    printf("  Temp/Freq    : Fairness context only; temperature/frequency do not normalize scores.\n");
}

/*
 * print_verdict: analyzes all collected metrics via a weighted voting
 * system and prints which binary is the overall winner.  Time elapsed
 * carries the heaviest weight (4.0), followed by cycles and IPC (2.0
 * each), then instructions and branch misses (1.0 each).  If the score
 * gap is less than 2.0 the result is declared inconclusive.  Also
 * prints the primary reason based on elapsed-time difference.
 *
 *   run1 - benchmark results for binary 1
 *   run2 - benchmark results for binary 2
 */
static void print_verdict(const BenchmarkRun *run1, const BenchmarkRun *run2) {
    double score1 = 0.0;
    double score2 = 0.0;
    double time_diff_pct = 0.0;

    add_metric_vote(&score1, &score2,
                    run1->metrics.has_seconds, run2->metrics.has_seconds,
                    run1->metrics.seconds, run2->metrics.seconds,
                    4.0, 0.05, 1);
    add_metric_vote(&score1, &score2,
                    run1->metrics.has_cycles, run2->metrics.has_cycles,
                    (double)run1->metrics.cycles, (double)run2->metrics.cycles,
                    2.0, 0.03, 1);
    add_metric_vote(&score1, &score2,
                    run1->metrics.has_ipc, run2->metrics.has_ipc,
                    run1->metrics.ipc, run2->metrics.ipc,
                    2.0, 0.03, 0);
    add_metric_vote(&score1, &score2,
                    run1->metrics.has_instructions, run2->metrics.has_instructions,
                    (double)run1->metrics.instructions, (double)run2->metrics.instructions,
                    1.0, 0.03, 1);
    add_metric_vote(&score1, &score2,
                    run1->metrics.has_branch_misses, run2->metrics.has_branch_misses,
                    (double)run1->metrics.branch_misses, (double)run2->metrics.branch_misses,
                    1.0, 0.05, 1);

    if (run1->metrics.has_seconds && run2->metrics.has_seconds && run1->metrics.seconds > 0.0) {
        time_diff_pct = ((run2->metrics.seconds - run1->metrics.seconds) / run1->metrics.seconds) * 100.0;
    }

    printf("\nFinal Verdict\n");

    if (score1 >= score2 + 2.0) {
        printf("  %s is the stronger relative performer (score %.1f vs %.1f).\n",
               run1->display_name, score1, score2);
    } else if (score2 >= score1 + 2.0) {
        printf("  %s is the stronger relative performer (score %.1f vs %.1f).\n",
               run2->display_name, score2, score1);
    } else {
        printf("  Result is inconclusive: performance is close or split across metrics (score %.1f vs %.1f).\n",
               score1, score2);
    }

    if (run1->metrics.has_seconds && run2->metrics.has_seconds) {
        if (time_diff_pct > 5.0) {
            printf("  Primary reason: %s took %.1f%% more elapsed time than %s.\n",
                   run2->display_name, time_diff_pct, run1->display_name);
        } else if (time_diff_pct < -5.0) {
            printf("  Primary reason: %s took %.1f%% less elapsed time than %s.\n",
                   run2->display_name, -time_diff_pct, run1->display_name);
        } else {
            printf("  Primary reason: elapsed time is within a close margin, so supporting metrics matter.\n");
        }
    }

}

/*
 * trim_newline: removes a trailing newline character ('\n') from a
 * string in place.  Used to sanitize lines read by fgets.
 *
 *   text - null-terminated string to modify
 */
static void trim_newline(char *text) {
    size_t len = strlen(text);

    if (len > 0 && text[len - 1] == '\n') {
        text[len - 1] = '\0';
    }
}

/*
 * read_menu_int: displays a prompt and reads an integer from stdin.
 * If the user presses Enter without typing anything, default_value is
 * used.  The parsed value must fall within [min_value, max_value].
 *
 *   prompt        - text to display before reading input
 *   default_value - value returned on empty input
 *   min_value     - lower bound (inclusive) for valid input
 *   max_value     - upper bound (inclusive) for valid input
 *   value         - output pointer for the parsed integer
 *
 * Returns: 1 on success, 0 on invalid input or EOF.
 */
static int read_menu_int(const char *prompt, int default_value, int min_value, int max_value, int *value) {
    char line[64];
    int parsed;

    printf("%s", prompt);

    if (fgets(line, sizeof(line), stdin) == NULL) {
        return 0;
    }

    trim_newline(line);

    if (line[0] == '\0') {
        *value = default_value;
        return 1;
    }

    if (!parse_int_in_range(line, min_value, max_value, &parsed)) {
        return 0;
    }

    *value = parsed;
    return 1;
}

/*
 * choose_demo_pair: presents an interactive menu that lets the user
 * select one of the built-in DEMO_PAIRS benchmark pairs and configure
 * run parameters (duration, run count, cooldown mode, CPU pinning).
 * Populates all output parameters and prints the equivalent CLI command
 * so the user can reproduce the run non-interactively.
 *
 *   duration_seconds - output, benchmark duration per run
 *   run_count        - output, number of measured passes
 *   pin_core         - output, CPU core to pin to (-1 = disabled)
 *   cooldown_seconds - output, fixed cooldown between runs
 *   auto_cooldown    - output, 1 to use temperature-based cooldown
 *   binary1, binary2 - output, paths to the two demo binaries
 *
 * Returns: 1 on success, 0 on invalid user input.
 */
static int choose_demo_pair(int *duration_seconds,
                            int *run_count,
                            int *pin_core,
                            int *cooldown_seconds,
                            int *auto_cooldown,
                            const char **binary1,
                            const char **binary2) {
    int choice;
    int selected_duration;
    int selected_runs;
    int selected_pin_core;
    int selected_cooldown;
    int selected_auto;
    size_t i;

    int pair_count = (int)(sizeof(DEMO_PAIRS) / sizeof(DEMO_PAIRS[0]));
    char prompt[64];

    printf("CLI Tool For Relative Benchmarking\n");
    printf("Select a built-in benchmark pair:\n\n");

    for (i = 0; i < (size_t)pair_count; i++) {
        printf("  %zu. %s\n", i + 1, DEMO_PAIRS[i].title);
        printf("     Concept: %s\n", DEMO_PAIRS[i].concept);
    }

    printf("\n");

    snprintf(prompt, sizeof(prompt), "Enter choice [1-%d]: ", pair_count);
    if (!read_menu_int(prompt, 1, 1, pair_count, &choice)) {
        fprintf(stderr, "Error: invalid menu choice.\n");
        return 0;
    }

    if (!read_menu_int("Duration in seconds [10]: ", DEFAULT_DURATION_SECONDS,
                       MIN_DURATION_SECONDS, MAX_DURATION_SECONDS, &selected_duration)) {
        fprintf(stderr, "Error: invalid duration.\n");
        return 0;
    }

    if (!read_menu_int("Number of runs to average [1]: ", DEFAULT_RUN_COUNT,
                       MIN_RUN_COUNT, MAX_RUN_COUNT, &selected_runs)) {
        fprintf(stderr, "Error: invalid run count.\n");
        return 0;
    }

    if (!read_menu_int("Auto-cooldown? (1=yes, 0=no) [0]: ", 0, 0, 1, &selected_auto)) {
        fprintf(stderr, "Error: invalid auto-cooldown choice.\n");
        return 0;
    }

    selected_cooldown = DEFAULT_COOLDOWN_SECONDS;
    if (!selected_auto) {
        if (!read_menu_int("Cooldown between runs in seconds [5]: ", DEFAULT_COOLDOWN_SECONDS,
                           MIN_COOLDOWN_SECONDS, MAX_COOLDOWN_SECONDS, &selected_cooldown)) {
            fprintf(stderr, "Error: invalid cooldown duration.\n");
            return 0;
        }
    }

    if (!read_menu_int("Pin to CPU core (-1 for no pinning) [-1]: ", -1,
                       -1, MAX_CORE_ID, &selected_pin_core)) {
        fprintf(stderr, "Error: invalid CPU core.\n");
        return 0;
    }

    *duration_seconds = selected_duration;
    *run_count = selected_runs;
    *cooldown_seconds = selected_cooldown;
    *auto_cooldown = selected_auto;
    *pin_core = selected_pin_core;
    *binary1 = DEMO_PAIRS[choice - 1].binary1;
    *binary2 = DEMO_PAIRS[choice - 1].binary2;

    printf("\nSelected: %s\n", DEMO_PAIRS[choice - 1].title);
    if (selected_auto) {
        printf("Command equivalent: ./perfcmp --duration %d --runs %d --auto-cooldown",
               *duration_seconds, *run_count);
    } else {
        printf("Command equivalent: ./perfcmp --duration %d --runs %d --cooldown %d",
               *duration_seconds, *run_count, *cooldown_seconds);
    }
    if (*pin_core >= 0) {
        printf(" --pin-core %d", *pin_core);
    }
    printf(" %s %s\n\n", *binary1, *binary2);

    return 1;
}

/* ── Scaling analysis ─────────────────────────────────────────────────────── */

/*
 * parse_sizes_list: parses a comma-separated list of positive integers
 * from the --sizes CLI flag into an array.  Values must be in the range
 * (0, 100000000].  Parsing stops at max_count entries.
 *
 *   text      - null-terminated input string (e.g. "1000,2000,4000")
 *   sizes     - output array of parsed integers
 *   max_count - maximum number of entries to parse
 *
 * Returns: the number of sizes parsed, or -1 on format error.
 */
static int parse_sizes_list(const char *text, int *sizes, int max_count) {
    int count = 0;
    const char *p = text;

    while (*p != '\0' && count < max_count) {
        char *end = NULL;
        long val;

        errno = 0;
        val = strtol(p, &end, 10);

        if (errno != 0 || end == p || val <= 0 || val > 100000000) {
            return -1;
        }

        sizes[count++] = (int)val;

        if (*end == ',') {
            p = end + 1;
        } else if (*end == '\0') {
            break;
        } else {
            return -1;
        }
    }

    if (*p != '\0' && count >= max_count) {
        fprintf(stderr, "Warning: maximum of %d sizes supported; truncating list.\n", max_count);
    }

    return count;
}

/*
 * log_log_regression: performs ordinary least-squares linear regression
 * in log-log space.  By fitting log(time) = slope * log(N) + intercept,
 * the slope approximates the exponent in the power-law T(N) ~ N^slope.
 * Also computes R² as a goodness-of-fit measure.
 *
 *   sizes          - array of input sizes N
 *   times          - array of measured elapsed times for each N
 *   count          - number of data points
 *   out_slope      - output, the regression slope (power exponent)
 *   out_r_squared  - output, coefficient of determination R²
 */
static void log_log_regression(const int *sizes, const double *times, int count,
                                double *out_slope, double *out_r_squared) {
    double sum_x = 0.0, sum_y = 0.0;
    double sum_xx = 0.0, sum_xy = 0.0;
    double n = (double)count;
    double denom, slope, intercept, y_mean;
    double ss_res = 0.0, ss_tot = 0.0;
    int valid = 0;
    int i;

    for (i = 0; i < count; i++) {
        double x, y;

        if (times[i] <= 1e-9) {
            continue;
        }

        x = log((double)sizes[i]);
        y = log(times[i]);
        sum_x += x;
        sum_y += y;
        sum_xx += x * x;
        sum_xy += x * y;
        valid++;
    }

    if (valid < 2) {
        *out_slope = 0.0;
        *out_r_squared = 0.0;
        return;
    }

    n = (double)valid;
    denom = n * sum_xx - sum_x * sum_x;

    if (denom < 1e-15 && denom > -1e-15) {
        *out_slope = 0.0;
        *out_r_squared = 0.0;
        return;
    }

    slope = (n * sum_xy - sum_x * sum_y) / denom;
    intercept = (sum_y - slope * sum_x) / n;
    y_mean = sum_y / n;

    for (i = 0; i < count; i++) {
        double x, y, pred;

        if (times[i] <= 1e-9) {
            continue;
        }

        x = log((double)sizes[i]);
        y = log(times[i]);
        pred = slope * x + intercept;
        ss_res += (y - pred) * (y - pred);
        ss_tot += (y - y_mean) * (y - y_mean);
    }

    *out_slope = slope;
    *out_r_squared = (ss_tot > 1e-15) ? (1.0 - ss_res / ss_tot) : 0.0;
}

/*
 * nlogn_fit_r_squared: computes the R² goodness-of-fit for an O(N log N)
 * model.  It fits log(time) against log(N * log(N)) using linear
 * regression.  This is used to disambiguate O(N log N) from O(N) when
 * the log-log slope falls in the ambiguous range around 1.0.
 *
 *   sizes - array of input sizes N
 *   times - array of measured elapsed times for each N
 *   count - number of data points
 *
 * Returns: R² for the N log N fit (0.0 if insufficient data).
 */
static double nlogn_fit_r_squared(const int *sizes, const double *times, int count) {
    double sum_x = 0.0, sum_y = 0.0;
    double sum_xx = 0.0, sum_xy = 0.0;
    double n_valid;
    double denom, slope, intercept, y_mean;
    double ss_res = 0.0, ss_tot = 0.0;
    int valid = 0;
    int i;

    for (i = 0; i < count; i++) {
        double big_n, x, y;

        if (times[i] <= 1e-9 || sizes[i] <= 1) {
            continue;
        }

        big_n = (double)sizes[i];
        x = log(big_n * log(big_n));
        y = log(times[i]);
        sum_x += x;
        sum_y += y;
        sum_xx += x * x;
        sum_xy += x * y;
        valid++;
    }

    if (valid < 2) {
        return 0.0;
    }

    n_valid = (double)valid;
    denom = n_valid * sum_xx - sum_x * sum_x;

    if (denom < 1e-15 && denom > -1e-15) {
        return 0.0;
    }

    slope = (n_valid * sum_xy - sum_x * sum_y) / denom;
    intercept = (sum_y - slope * sum_x) / n_valid;
    y_mean = sum_y / n_valid;

    for (i = 0; i < count; i++) {
        double big_n, x, y, pred;

        if (times[i] <= 1e-9 || sizes[i] <= 1) {
            continue;
        }

        big_n = (double)sizes[i];
        x = log(big_n * log(big_n));
        y = log(times[i]);
        pred = slope * x + intercept;
        ss_res += (y - pred) * (y - pred);
        ss_tot += (y - y_mean) * (y - y_mean);
    }

    return (ss_tot > 1e-15) ? (1.0 - ss_res / ss_tot) : 0.0;
}

/*
 * classify_complexity: maps a regression exponent (slope from log-log
 * regression) to a Big O complexity class.  Uses the following bands:
 *   slope < 0.3      → O(1)
 *   0.3  ≤ slope < 1.35 → O(N)  (or O(N log N) if the N·log N model
 *                                  fits better)
 *   1.35 ≤ slope < 2.3  → O(N²)
 *   2.3  ≤ slope < 3.3  → O(N³)
 *   otherwise          → unknown (shown as O(N^slope))
 *
 *   sizes  - array of input sizes
 *   times  - array of measured elapsed times
 *   count  - number of data points
 *   result - output ScalingResult with best_fit, exponent, and R²
 */
static void classify_complexity(const int *sizes, const double *times, int count,
                                 ScalingResult *result) {
    double slope, r_sq;

    log_log_regression(sizes, times, count, &slope, &r_sq);

    result->exponent = slope;
    result->r_squared = r_sq;

    if (slope >= 0.8 && slope < 1.35) {
        double nlogn_r2 = nlogn_fit_r_squared(sizes, times, count);

        if (nlogn_r2 > r_sq + 0.005) {
            result->best_fit = COMPLEXITY_NLOGN;
            result->r_squared = nlogn_r2;
            return;
        }
    }

    if (slope < 0.3) {
        result->best_fit = COMPLEXITY_CONSTANT;
    } else if (slope < 1.35) {
        result->best_fit = COMPLEXITY_LINEAR;
    } else if (slope < 2.3) {
        result->best_fit = COMPLEXITY_QUADRATIC;
    } else if (slope < 3.3) {
        result->best_fit = COMPLEXITY_CUBIC;
    } else {
        result->best_fit = COMPLEXITY_UNKNOWN;
    }
}

/*
 * format_complexity_label: converts a ComplexityClass enum into a
 * human-readable string like "O(1)", "O(N)", "O(N log N)", "O(N^2)",
 * etc.  For COMPLEXITY_UNKNOWN, it shows the raw exponent as
 * "O(N^<exponent>)".
 *
 *   cls         - the classified complexity enum value
 *   exponent    - the raw slope from log-log regression
 *   buffer      - output character buffer
 *   buffer_size - size of the output buffer in bytes
 */
static void format_complexity_label(ComplexityClass cls, double exponent,
                                     char *buffer, size_t buffer_size) {
    switch (cls) {
        case COMPLEXITY_CONSTANT:
            snprintf(buffer, buffer_size, "O(1)");
            break;
        case COMPLEXITY_LINEAR:
            snprintf(buffer, buffer_size, "O(N)");
            break;
        case COMPLEXITY_NLOGN:
            snprintf(buffer, buffer_size, "O(N log N)");
            break;
        case COMPLEXITY_QUADRATIC:
            snprintf(buffer, buffer_size, "O(N^2)");
            break;
        case COMPLEXITY_CUBIC:
            snprintf(buffer, buffer_size, "O(N^3)");
            break;
        default:
            snprintf(buffer, buffer_size, "O(N^%.2f)", exponent);
            break;
    }
}

/*
 * print_scaling_table: prints a table of elapsed times at each input
 * size N for both binaries.  This gives the user raw data to visually
 * inspect how execution time grows with input size.
 *
 *   data  - scaling data containing sizes and per-binary times
 *   name1 - display name of binary 1
 *   name2 - display name of binary 2
 */
static void print_scaling_table(const ScalingData *data,
                                 const char *name1, const char *name2) {
    int i;

    printf("\n%s\n", METRIC_TABLE_LINE);
    printf("                       EMPIRICAL SCALING ANALYSIS                      \n");
    printf("%s\n", METRIC_TABLE_LINE);
    printf("%-12s | %-20.20s | %-20.20s\n", "N", name1, name2);
    printf("%s\n", METRIC_TABLE_LINE);

    for (i = 0; i < data->count; i++) {
        printf("%-12d | %-20.6f | %-20.6f\n",
               data->sizes[i], data->times1[i], data->times2[i]);
    }

    printf("%s\n", METRIC_TABLE_LINE);
}

/*
 * print_r_squared_bar: prints a 20-character visual bar graph
 * representing the R² confidence value.  Filled blocks (█) indicate
 * the proportion of variance explained; empty blocks (░) fill the
 * remainder.  The bar is clamped to [0.0, 1.0].
 *
 *   r_sq - R² value from the regression (0.0 to 1.0)
 */
static void print_r_squared_bar(double r_sq) {
    int filled;
    int i;

    if (r_sq < 0.0) {
        r_sq = 0.0;
    }

    filled = (int)(r_sq * 20.0 + 0.5);

    if (filled > 20) {
        filled = 20;
    }

    for (i = 0; i < filled; i++) {
        printf("\xe2\x96\x88");
    }

    for (i = filled; i < 20; i++) {
        printf("\xe2\x96\x91");
    }
}

/*
 * print_scaling_verdict: prints the final scaling analysis result.
 * Shows each binary's estimated Big O complexity class, the raw
 * exponent, R² confidence with a visual bar, and a summary stating
 * whether the difference is algorithmic or constant-factor.
 *
 *   res1, res2 - scaling analysis results for each binary
 *   name1      - display name of binary 1
 *   name2      - display name of binary 2
 */
static void print_scaling_verdict(const ScalingResult *res1, const ScalingResult *res2,
                                   const char *name1, const char *name2) {
    char label1[32], label2[32];

    format_complexity_label(res1->best_fit, res1->exponent, label1, sizeof(label1));
    format_complexity_label(res2->best_fit, res2->exponent, label2, sizeof(label2));

    printf("\nEstimated Complexity (log-log regression)\n");

    printf("  %-20.20s : %-12s  R\xc2\xb2=%.4f  ", name1, label1, res1->r_squared);
    print_r_squared_bar(res1->r_squared);
    printf("  (exponent=%.2f)\n", res1->exponent);

    printf("  %-20.20s : %-12s  R\xc2\xb2=%.4f  ", name2, label2, res2->r_squared);
    print_r_squared_bar(res2->r_squared);
    printf("  (exponent=%.2f)\n", res2->exponent);

    if (res1->best_fit == res2->best_fit && res1->best_fit != COMPLEXITY_UNKNOWN) {
        printf("\n  Both scale as %s", label1);
        printf(" -- differences are constant-factor, not algorithmic.\n");
    } else if (res1->best_fit != COMPLEXITY_UNKNOWN && res2->best_fit != COMPLEXITY_UNKNOWN) {
        printf("\n  %s scales as %s while %s scales as %s.\n",
               name1, label1, name2, label2);
    }
}

/*
 * run_scaling_analysis: orchestrates the full scaling (Big O) analysis
 * pipeline.  For each input size N, it runs both binaries via perf stat,
 * collects elapsed times, then performs log-log regression to classify
 * each binary's empirical complexity.  Outputs the raw timing table and
 * a verdict comparing the two complexity classes.
 *
 *   binary1, binary2 - paths to the two executable binaries
 *   name1, name2     - display names for the binaries
 *   sizes            - array of input sizes to test
 *   size_count       - number of input sizes
 *   pin_core         - CPU core to pin to (-1 = disabled)
 *
 * Returns: 1 on success, 0 on any perf or parsing failure.
 */
static int run_scaling_analysis(const char *binary1, const char *binary2,
                                 const char *name1, const char *name2,
                                 const int *sizes, int size_count,
                                 int pin_core) {
    ScalingData data;
    ScalingResult result1, result2;
    char log_path[LOG_PATH_SIZE];
    PerfMetrics metrics;
    char size_str[32];
    int i;

    data.count = size_count;

    snprintf(log_path, sizeof(log_path), ".perfcmp_perf_%ld_scale.log", (long)getpid());

    for (i = 0; i < size_count; i++) {
        data.sizes[i] = sizes[i];

        printf("[scaling %d/%d] %s  N=%d ...\n", i + 1, size_count, name1, sizes[i]);
        unlink(log_path);

        snprintf(size_str, sizeof(size_str), "%d", sizes[i]);

        if (!run_perf_stat(binary1, log_path, SCALING_TIMEOUT_SECONDS, pin_core, size_str)) {
            unlink(log_path);
            return 0;
        }

        if (!parse_perf_file(log_path, &metrics)) {
            unlink(log_path);
            return 0;
        }

        data.times1[i] = metrics.seconds;

        printf("[scaling %d/%d] %s  N=%d ...\n", i + 1, size_count, name2, sizes[i]);
        unlink(log_path);

        if (!run_perf_stat(binary2, log_path, SCALING_TIMEOUT_SECONDS, pin_core, size_str)) {
            unlink(log_path);
            return 0;
        }

        if (!parse_perf_file(log_path, &metrics)) {
            unlink(log_path);
            return 0;
        }

        data.times2[i] = metrics.seconds;
    }

    unlink(log_path);

    classify_complexity(data.sizes, data.times1, data.count, &result1);
    classify_complexity(data.sizes, data.times2, data.count, &result2);

    print_scaling_table(&data, name1, name2);
    print_scaling_verdict(&result1, &result2, name1, name2);

    return 1;
}

/*
 * parse_arguments: parses and validates all command-line arguments.
 * Handles --duration, --runs, --cooldown, --auto-cooldown, --pin-core,
 * --scaling, --sizes, --demo, --help, and the two positional binary
 * paths.  Enforces mutual exclusion between --cooldown and
 * --auto-cooldown, and between --demo and custom binaries.  Falls
 * through to choose_demo_pair() when no binaries are given or --demo
 * is specified.
 *
 *   argc, argv          - standard C main arguments
 *   duration_seconds     - output, per-run duration
 *   run_count            - output, number of measured passes
 *   pin_core             - output, CPU core to pin to (-1 = disabled)
 *   cooldown_seconds     - output, fixed cooldown between runs
 *   auto_cooldown        - output, 1 for temperature-based cooldown
 *   scaling_mode         - output, 1 if --scaling was requested
 *   scaling_sizes        - output array of custom sizes for --sizes
 *   scaling_size_count   - output, count of parsed sizes
 *   binary1, binary2     - output, paths to the two binaries
 *
 * Returns: 1 on success, 0 on validation failure.
 */
static int parse_arguments(int argc,
                           char *argv[],
                           int *duration_seconds,
                           int *run_count,
                           int *pin_core,
                           int *cooldown_seconds,
                           int *auto_cooldown,
                           int *scaling_mode,
                           int *scaling_sizes,
                           int *scaling_size_count,
                           const char **binary1,
                           const char **binary2) {
    int i;
    const char *binaries[2] = {NULL, NULL};
    int binary_count = 0;
    int demo_mode = 0;
    int cooldown_explicit = 0;

    *duration_seconds = DEFAULT_DURATION_SECONDS;
    *run_count = DEFAULT_RUN_COUNT;
    *pin_core = -1;
    *cooldown_seconds = DEFAULT_COOLDOWN_SECONDS;
    *auto_cooldown = 0;
    *scaling_mode = 0;
    *scaling_size_count = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            exit(0);
        } else if (strcmp(argv[i], "--demo") == 0) {
            demo_mode = 1;
        } else if (strcmp(argv[i], "--duration") == 0) {
            if (i + 1 >= argc || !parse_duration_seconds(argv[i + 1], duration_seconds)) {
                fprintf(stderr, "Error: --duration requires an integer from %d to %d seconds.\n",
                        MIN_DURATION_SECONDS, MAX_DURATION_SECONDS);
                return 0;
            }
            i++;
        } else if (strcmp(argv[i], "--runs") == 0) {
            if (i + 1 >= argc ||
                !parse_int_in_range(argv[i + 1], MIN_RUN_COUNT, MAX_RUN_COUNT, run_count)) {
                fprintf(stderr, "Error: --runs requires an integer from %d to %d.\n",
                        MIN_RUN_COUNT, MAX_RUN_COUNT);
                return 0;
            }
            i++;
        } else if (strcmp(argv[i], "--cooldown") == 0) {
            if (i + 1 >= argc ||
                !parse_int_in_range(argv[i + 1], MIN_COOLDOWN_SECONDS, MAX_COOLDOWN_SECONDS,
                                    cooldown_seconds)) {
                fprintf(stderr, "Error: --cooldown requires an integer from %d to %d seconds.\n",
                        MIN_COOLDOWN_SECONDS, MAX_COOLDOWN_SECONDS);
                return 0;
            }
            cooldown_explicit = 1;
            i++;
        } else if (strcmp(argv[i], "--auto-cooldown") == 0) {
            *auto_cooldown = 1;
        } else if (strcmp(argv[i], "--pin-core") == 0) {
            if (i + 1 >= argc ||
                !parse_int_in_range(argv[i + 1], MIN_CORE_ID, MAX_CORE_ID, pin_core)) {
                fprintf(stderr, "Error: --pin-core requires an integer from %d to %d.\n",
                        MIN_CORE_ID, MAX_CORE_ID);
                return 0;
            }
            i++;
        } else if (strcmp(argv[i], "--scaling") == 0) {
            *scaling_mode = 1;
        } else if (strcmp(argv[i], "--sizes") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --sizes requires a comma-separated list of integers.\n");
                return 0;
            }
            i++;
            *scaling_size_count = parse_sizes_list(argv[i], scaling_sizes, MAX_SCALE_POINTS);
            if (*scaling_size_count <= 0) {
                fprintf(stderr, "Error: --sizes format: comma-separated positive integers (e.g., 1000,2000,4000).\n");
                return 0;
            }
        } else {
            if (binary_count >= 2) {
                fprintf(stderr, "Error: too many binary paths provided.\n");
                return 0;
            }
            binaries[binary_count++] = argv[i];
        }
    }

    /* Mutual exclusion check */
    if (*auto_cooldown && cooldown_explicit) {
        fprintf(stderr, "Error: --auto-cooldown and --cooldown are mutually exclusive.\n");
        return 0;
    }

    if (demo_mode && binary_count > 0) {
        fprintf(stderr, "Error: --demo cannot be combined with custom binary paths.\n");
        return 0;
    }

    if (demo_mode || binary_count == 0) {
        return choose_demo_pair(duration_seconds, run_count, pin_core,
                                cooldown_seconds, auto_cooldown, binary1, binary2);
    }

    if (binary_count != 2) {
        fprintf(stderr, "Error: expected exactly two binary paths.\n\n");
        print_usage(argv[0]);
        return 0;
    }

    *binary1 = binaries[0];
    *binary2 = binaries[1];
    return 1;
}

/*
 * main: program entry point.  Orchestrates the full benchmark workflow:
 *   1. Parse CLI arguments (or launch interactive demo menu).
 *   2. Validate that both binaries exist and required tools (perf,
 *      timeout, optionally taskset) are installed.
 *   3. Run benchmark passes for each binary with warm-up and cooldown.
 *   4. Print the performance comparison table, system context table,
 *      weighted verdict, and metric guide.
 *   5. Optionally run scaling (Big O) analysis if --scaling was given.
 *   6. Clean up temporary log files and exit.
 *
 * Returns: 0 on success, 1 on any failure.
 */
int main(int argc, char *argv[]) {
    int duration_seconds;
    int run_count;
    int pin_core;
    int cooldown_seconds;
    int auto_cooldown;
    int scaling_mode;
    int scaling_sizes[MAX_SCALE_POINTS];
    int scaling_size_count;
    const char *binary1;
    const char *binary2;
    char log1[LOG_PATH_SIZE];
    char log2[LOG_PATH_SIZE];
    BenchmarkRun run1;
    BenchmarkRun run2;
    int success = 0;
    double idle_baseline_temp = 0.0;
    int has_idle_baseline = 0;

    g_use_color = isatty(STDOUT_FILENO);

    if (!parse_arguments(argc, argv, &duration_seconds, &run_count,
                         &pin_core, &cooldown_seconds, &auto_cooldown,
                         &scaling_mode, scaling_sizes,
                         &scaling_size_count, &binary1, &binary2)) {
        return 1;
    }

    if (access(binary1, X_OK) != 0 || access(binary2, X_OK) != 0) {
        fprintf(stderr, "Error: one or both binary paths do not exist or lack execute permission.\n");
        return 1;
    }

    if (!shell_command_succeeds("command -v perf > /dev/null 2>&1")) {
        fprintf(stderr, "Error: perf is not available on PATH.\n");
        print_common_perf_failure_help();
        return 1;
    }

    if (!shell_command_succeeds("command -v timeout > /dev/null 2>&1")) {
        fprintf(stderr, "Error: GNU timeout is not available on PATH.\n");
        print_common_perf_failure_help();
        return 1;
    }

    if (pin_core >= 0 && !shell_command_succeeds("command -v taskset > /dev/null 2>&1")) {
        fprintf(stderr, "Error: --pin-core requires taskset, but taskset is not available on PATH.\n");
        fprintf(stderr, "Install util-linux or run without --pin-core.\n");
        return 1;
    }

    printf("Benchmark settings: duration=%ds, runs=%d, cooldown=%s",
           duration_seconds, run_count,
           auto_cooldown ? "auto" : "");
    if (!auto_cooldown) {
        printf("%ds", cooldown_seconds);
    }
    if (pin_core >= 0) {
        printf(", pinned_core=%d", pin_core);
    } else {
        printf(", pinned_core=disabled");
    }
    if (scaling_mode) {
        printf(", scaling=enabled");
    }
    printf("\n");

    snprintf(log1, sizeof(log1), ".perfcmp_perf_%ld_1.log", (long)getpid());
    snprintf(log2, sizeof(log2), ".perfcmp_perf_%ld_2.log", (long)getpid());
    snprintf(g_cleanup_logs[0], sizeof(g_cleanup_logs[0]), "%s", log1);
    snprintf(g_cleanup_logs[1], sizeof(g_cleanup_logs[1]), "%s", log2);
    snprintf(g_cleanup_logs[2], sizeof(g_cleanup_logs[2]),
             ".perfcmp_perf_%ld_scale.log", (long)getpid());

    signal(SIGINT, cleanup_on_signal);
    signal(SIGTERM, cleanup_on_signal);

    unlink(log1);
    unlink(log2);

    /* Capture idle CPU temperature before any benchmarks heat the system. */
    if (auto_cooldown) {
        has_idle_baseline = read_temperature_c(&idle_baseline_temp);
        if (has_idle_baseline) {
            printf("Idle CPU baseline temperature: %.1f C\n", idle_baseline_temp);
        }
    }

    memset(&run1, 0, sizeof(run1));
    memset(&run2, 0, sizeof(run2));

    run1.binary_path = binary1;
    run1.display_name = base_name(binary1);
    run1.log_path = log1;

    run2.binary_path = binary2;
    run2.display_name = base_name(binary2);
    run2.log_path = log2;

    if (run_benchmark(&run1, duration_seconds, 1, run_count, pin_core,
                      cooldown_seconds, auto_cooldown)) {
        if (auto_cooldown) {
            /* Between binaries: cool back to the idle baseline captured
             * before any benchmarks ran, not the current (hot) temperature. */
            if (has_idle_baseline) {
                printf("\nAuto-cooldown between binaries: waiting for temperature "
                       "to return to idle baseline (%.1f C) ...\n", idle_baseline_temp);
                wait_for_temp_baseline(idle_baseline_temp, 2, 1, run_count);
            } else {
                printf("\nCooling down for %d s between binaries (no temp sensor) ...\n",
                       DEFAULT_COOLDOWN_SECONDS);
                sleep((unsigned int)DEFAULT_COOLDOWN_SECONDS);
            }
        } else if (cooldown_seconds > 0) {
            printf("\nCooling down for %d s between binaries ...\n", cooldown_seconds);
            sleep((unsigned int)cooldown_seconds);
        }
        if (run_benchmark(&run2, duration_seconds, 2, run_count, pin_core,
                          cooldown_seconds, auto_cooldown)) {
            print_metric_table(&run1, &run2);
            print_system_context_table(&run1, &run2);
            print_verdict(&run1, &run2);
            print_metric_explanations();
            success = 1;
        }
    }

    if (success && scaling_mode) {
        size_t p;

        /* Resolve default sizes if none were provided via --sizes */
        if (scaling_size_count == 0) {
            int found = 0;

            for (p = 0; p < sizeof(DEMO_PAIRS) / sizeof(DEMO_PAIRS[0]); p++) {
                if (strcmp(binary1, DEMO_PAIRS[p].binary1) == 0 &&
                    strcmp(binary2, DEMO_PAIRS[p].binary2) == 0) {
                    memcpy(scaling_sizes, DEMO_PAIRS[p].default_sizes,
                           DEFAULT_SCALE_POINTS * sizeof(int));
                    scaling_size_count = DEFAULT_SCALE_POINTS;
                    found = 1;
                    break;
                }
            }

            if (!found) {
                static const int generic[] = {1000, 2000, 4000, 8000, 16000};
                memcpy(scaling_sizes, generic, sizeof(generic));
                scaling_size_count = 5;
            }
        }

        printf("\nRunning scaling analysis with %d size points ...\n", scaling_size_count);

        if (!run_scaling_analysis(binary1, binary2,
                                  run1.display_name, run2.display_name,
                                  scaling_sizes, scaling_size_count,
                                  pin_core)) {
            success = 0;
        }
    }

    unlink(log1);
    unlink(log2);

    return success ? 0 : 1;
}

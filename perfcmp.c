/* perfcmp.c — Relative benchmarking of two binaries using perf stat.
 * Compares cycles, instructions, branch-misses, and wall-clock time.
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

/* 1 = color output, 0 = plain text */
static int g_use_color = 1;

/* Temp log paths for cleanup on Ctrl+C. Only async-signal-safe calls here. */
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
 *  Data types
 * ══════════════════════════════════════════════════════════════════════════ */

/* Hardware counter values from a perf stat run. has_* flags track availability. */
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

/* CPU temp + freq snapshot for detecting thermal bias. */
typedef struct {
    double temperature_c;
    double frequency_mhz;
    int has_temperature;
    int has_frequency;
} SystemContext;

/* Everything about one binary's benchmark: path, metrics, thermal context. */
typedef struct {
    const char *binary_path;
    const char *display_name;
    const char *log_path;
    PerfMetrics metrics;
    SystemContext before;
    SystemContext after;
} BenchmarkRun;

/* Big O classes for the scaling engine. */
typedef enum {
    COMPLEXITY_CONSTANT,
    COMPLEXITY_LINEAR,
    COMPLEXITY_NLOGN,
    COMPLEXITY_QUADRATIC,
    COMPLEXITY_CUBIC,
    COMPLEXITY_UNKNOWN
} ComplexityClass;

/* Raw timing data at multiple input sizes for scaling analysis. */
typedef struct {
    int sizes[MAX_SCALE_POINTS];
    double times1[MAX_SCALE_POINTS];
    double times2[MAX_SCALE_POINTS];
    int count;
} ScalingData;

/* Result of fitting a complexity model: class, exponent, R². */
typedef struct {
    ComplexityClass best_fit;
    double exponent;
    double r_squared;
} ScalingResult;

/* A built-in demo pair: title, binaries, concept, expected complexity, default sizes. */
typedef struct {
    const char *title;
    const char *binary1;
    const char *binary2;
    const char *concept;
    const char *expected_complexity;
    int default_sizes[DEFAULT_SCALE_POINTS];
} DemoPair;

/* Built-in benchmark experiments shipped with the tool. */
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
 *  Usage / help
 * ══════════════════════════════════════════════════════════════════════════ */

/* Print the full --help text. */
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
 *  String / parsing helpers
 * ══════════════════════════════════════════════════════════════════════════ */

/* Extract filename from a path (handles / and \). */
static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    const char *last_sep = slash;

    if (backslash != NULL && (last_sep == NULL || backslash > last_sep)) {
        last_sep = backslash;
    }

    return (last_sep != NULL) ? last_sep + 1 : path;
}

/* Strip commas from perf output numbers (e.g. "1,234" -> "1234"). */
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

/* Safe string-to-int with range check. Returns 1 on success. */
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

/* Validate a duration value against min/max bounds. */
static int parse_duration_seconds(const char *text, int *value) {
    return parse_int_in_range(text, MIN_DURATION_SECONDS, MAX_DURATION_SECONDS, value);
}

/* Extract exit code from system() return value. */
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

/* Run a command, return 1 if it exits 0. */
static int shell_command_succeeds(const char *command) {
    int status = system(command);
    return command_exit_code(status) == 0;
}

/* Wrap a string in single quotes for safe shell interpolation. */
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
 *  System monitoring
 * ══════════════════════════════════════════════════════════════════════════ */

/* Read CPU temp from /sys/class/thermal/. Averages all available zones. */
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

/* Read average CPU frequency from /proc/cpuinfo. */
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

/* Snapshot current CPU temp + freq. */
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
 *  Perf stat
 * ══════════════════════════════════════════════════════════════════════════ */

/* Parse a perf stat log file and extract counter values. */
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

/* Print troubleshooting tips when perf fails. */
static void print_common_perf_failure_help(void) {
    fprintf(stderr, "\nCommon causes:\n");
    fprintf(stderr, "  - perf is not installed. Ubuntu: sudo apt install linux-tools-common linux-tools-generic\n");
    fprintf(stderr, "  - perf is not installed. Fedora: sudo dnf install perf\n");
    fprintf(stderr, "  - perf_event permissions are restricted. Try running with sudo or adjusting perf_event_paranoid.\n");
    fprintf(stderr, "  - GNU timeout is missing.\n");
    fprintf(stderr, "  - taskset is missing, if --pin-core is enabled.\n");
    fprintf(stderr, "  - The benchmark binary crashed or does not have execute permission.\n");
}

/* Build and run the perf stat command. Exit code 124 = timeout (OK). */
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
 *  Metric accumulation
 * ══════════════════════════════════════════════════════════════════════════ */

/* Add one run's metrics to running totals. */
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

/* Divide accumulated totals by run count to get averages. */
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

/* Format an unsigned long long for display, or "N/A" if unavailable. */
static void format_ull_metric(int available, unsigned long long value, char *buffer, size_t buffer_size) {
    if (!available) {
        snprintf(buffer, buffer_size, "N/A");
    } else {
        snprintf(buffer, buffer_size, "%llu", value);
    }
}

/* Format a double for display, or "N/A" if unavailable. */
static void format_double_metric(int available, double value, int precision, char *buffer, size_t buffer_size) {
    if (!available) {
        snprintf(buffer, buffer_size, "N/A");
    } else {
        snprintf(buffer, buffer_size, "%.*f", precision, value);
    }
}

/* Print one row of the comparison table with color-coded % diff. */
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

/* Print the full side-by-side benchmark comparison table. */
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

/* Format a sensor reading (temp/freq) with unit suffix, or "N/A". */
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

/* Print CPU temp/freq before and after each run, with bias warnings. */
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

/* Compute (larger - smaller) / larger. Returns 0 if both are zero. */
static double relative_difference(double a, double b) {
    double larger = a > b ? a : b;
    double smaller = a > b ? b : a;

    if (larger == 0.0) {
        return 0.0;
    }

    return (larger - smaller) / larger;
}

/* Add a weighted vote to the verdict score if the metric difference exceeds threshold. */
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

/* Print a short guide explaining what each metric means. */
static void print_metric_explanations(void) {
    printf("\nMetric Guide\n");
    printf("  Time Elapsed : Real wall-clock runtime. Lower is better.\n");
    printf("  CPU Cycles   : Number of processor cycles spent. Lower is better.\n");
    printf("  Instructions : Number of executed instructions. Lower can indicate a smaller work path.\n");
    printf("  IPC          : Instructions per cycle. Higher often means better CPU pipeline use.\n");
    printf("  Branch Misses: Failed branch predictions. Lower usually means less wasted work.\n");
    printf("  Temp/Freq    : Fairness context only; temperature/frequency do not normalize scores.\n");
}

/* Weighted voting across all metrics to decide the winner. */
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

/* Strip trailing newline from fgets output. */
static void trim_newline(char *text) {
    size_t len = strlen(text);

    if (len > 0 && text[len - 1] == '\n') {
        text[len - 1] = '\0';
    }
}

/* Read an int from stdin with a default value and range validation. */
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

/* Interactive menu: pick a demo pair and configure run parameters. */
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

/* Parse comma-separated integers from the --sizes flag. */
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

/* Least-squares regression in log-log space. Slope = power exponent. */
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

/* R² for an O(N log N) model, used to disambiguate from O(N). */
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

/* Map the regression slope to a Big O class (O(1), O(N), O(N²), etc). */
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

/* Turn a ComplexityClass enum into a string like "O(N^2)". */
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

/* Print elapsed times at each input size for both binaries. */
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

/* Print a visual bar graph for the R² confidence value. */
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

/* Print the final scaling result: complexity class + R² for each binary. */
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

/* Run both binaries at multiple sizes, do regression, print results. */
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

/* Parse and validate all CLI flags and positional args. */
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

/* Entry point: parse args, benchmark both binaries, print results, cleanup. */
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

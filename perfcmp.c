#include <errno.h>
#include <limits.h>
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

#define DEFAULT_DURATION_SECONDS 10
#define MIN_DURATION_SECONDS 1
#define MAX_DURATION_SECONDS 3600
#define TEMP_WARNING_DELTA_C 5.0
#define FREQ_WARNING_DELTA_MHZ 150.0
#define LOG_PATH_SIZE 128
#define CMD_SIZE 2048
#define METRIC_TABLE_LINE "=============================================================================="
#define CONTEXT_TABLE_LINE "=============================================================================================="

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

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

typedef struct {
    double temperature_c;
    double frequency_mhz;
    int has_temperature;
    int has_frequency;
} SystemContext;

typedef struct {
    const char *binary_path;
    const char *display_name;
    const char *log_path;
    PerfMetrics metrics;
    SystemContext before;
    SystemContext after;
} BenchmarkRun;

typedef struct {
    const char *title;
    const char *binary1;
    const char *binary2;
    const char *concept;
} DemoPair;

static const DemoPair DEMO_PAIRS[] = {
    {
        "Row-major vs column-major",
        "./testcases/01_row_major",
        "./testcases/02_col_major",
        "cache locality in C arrays"
    },
    {
        "Bubble sort vs selection sort",
        "./testcases/03_bubble_sort",
        "./testcases/04_selection_sort",
        "algorithm behavior and swap count"
    },
    {
        "Without prefetch vs with prefetch",
        "./testcases/05_without_prefetch",
        "./testcases/06_with_prefetch",
        "software prefetch hints"
    },
    {
        "Heap allocation vs stack allocation",
        "./testcases/07_malloc_alloc",
        "./testcases/08_stack_alloc",
        "allocation overhead"
    }
};

static void print_usage(const char *program_name) {
    size_t i;

    printf("CLI Tool For Relative Benchmarking\n\n");
    printf("Usage:\n");
    printf("  %s\n", program_name);
    printf("  %s --demo\n", program_name);
    printf("  %s <binary1> <binary2>\n", program_name);
    printf("  %s --duration <seconds> <binary1> <binary2>\n", program_name);
    printf("  %s --help\n\n", program_name);
    printf("Demo mode:\n");
    printf("  Running without binary paths opens a menu of built-in testcase pairs.\n");
    printf("  Use full binary paths only when benchmarking your own new programs.\n\n");
    printf("Built-in pairs:\n");
    for (i = 0; i < sizeof(DEMO_PAIRS) / sizeof(DEMO_PAIRS[0]); i++) {
        printf("  %zu. %s (%s)\n", i + 1, DEMO_PAIRS[i].title, DEMO_PAIRS[i].concept);
    }
    printf("\n");
    printf("Examples:\n");
    printf("  %s\n", program_name);
    printf("  %s --demo\n", program_name);
    printf("  %s ./testcases/01_row_major ./testcases/02_col_major\n", program_name);
    printf("  %s --duration 10 ./testcases/03_bubble_sort ./testcases/04_selection_sort\n\n", program_name);
    printf("Requirements:\n");
    printf("  Linux, gcc, make, GNU timeout, and perf.\n");
}

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    const char *last_sep = slash;

    if (backslash != NULL && (last_sep == NULL || backslash > last_sep)) {
        last_sep = backslash;
    }

    return (last_sep != NULL) ? last_sep + 1 : path;
}

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

static int parse_positive_int(const char *text, int *value) {
    char *end = NULL;
    long parsed;

    errno = 0;
    parsed = strtol(text, &end, 10);

    if (errno != 0 || end == text || *end != '\0') {
        return 0;
    }

    if (parsed < MIN_DURATION_SECONDS || parsed > MAX_DURATION_SECONDS) {
        return 0;
    }

    *value = (int)parsed;
    return 1;
}

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

static int shell_command_succeeds(const char *command) {
    int status = system(command);
    return command_exit_code(status) == 0;
}

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

        if (sscanf(line, "cpu MHz : %lf", &mhz) == 1 ||
            sscanf(line, "cpu MHz\t: %lf", &mhz) == 1) {
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

static SystemContext capture_system_context(void) {
    SystemContext context;

    context.temperature_c = 0.0;
    context.frequency_mhz = 0.0;
    context.has_temperature = read_temperature_c(&context.temperature_c);
    context.has_frequency = read_average_frequency_mhz(&context.frequency_mhz);

    return context;
}

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

static void print_common_perf_failure_help(void) {
    fprintf(stderr, "\nCommon causes:\n");
    fprintf(stderr, "  - perf is not installed. Ubuntu: sudo apt install linux-tools-common linux-tools-generic\n");
    fprintf(stderr, "  - perf is not installed. Fedora: sudo dnf install perf\n");
    fprintf(stderr, "  - perf_event permissions are restricted. Try running with sudo or adjusting perf_event_paranoid.\n");
    fprintf(stderr, "  - GNU timeout is missing.\n");
    fprintf(stderr, "  - The benchmark binary crashed or does not have execute permission.\n");
}

static int run_perf_stat(const char *binary_path, const char *log_path, int duration_seconds) {
    char quoted_binary[PATH_MAX + 128];
    char quoted_log[PATH_MAX + 128];
    char command[CMD_SIZE];
    int status;
    int exit_code;

    if (!quote_shell_arg(binary_path, quoted_binary, sizeof(quoted_binary)) ||
        !quote_shell_arg(log_path, quoted_log, sizeof(quoted_log))) {
        fprintf(stderr, "Error: path is too long to quote safely.\n");
        return 0;
    }

    if (snprintf(command, sizeof(command),
                 "perf stat -e cycles,instructions,branch-misses -o %s "
                 "timeout %ds %s > /dev/null 2>&1",
                 quoted_log, duration_seconds, quoted_binary) >= (int)sizeof(command)) {
        fprintf(stderr, "Error: generated perf command is too long.\n");
        return 0;
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

static int run_benchmark(BenchmarkRun *run, int duration_seconds, int run_number) {
    printf("[%d/2] Profiling %s for up to %d seconds...\n",
           run_number, run->binary_path, duration_seconds);

    run->before = capture_system_context();

    if (!run_perf_stat(run->binary_path, run->log_path, duration_seconds)) {
        return 0;
    }

    run->after = capture_system_context();

    if (!parse_perf_file(run->log_path, &run->metrics)) {
        return 0;
    }

    return 1;
}

static void format_ull_metric(int available, unsigned long long value, char *buffer, size_t buffer_size) {
    if (!available) {
        snprintf(buffer, buffer_size, "N/A");
    } else {
        snprintf(buffer, buffer_size, "%llu", value);
    }
}

static void format_double_metric(int available, double value, int precision, char *buffer, size_t buffer_size) {
    if (!available) {
        snprintf(buffer, buffer_size, "N/A");
    } else {
        snprintf(buffer, buffer_size, "%.*f", precision, value);
    }
}

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
            printf(ANSI_COLOR_GREEN "%-18s" ANSI_COLOR_RESET " | "
                   ANSI_COLOR_GREEN "%-+11.1f%%" ANSI_COLOR_RESET "\n",
                   value2, pct_diff);
        } else {
            printf(ANSI_COLOR_RED "%-18s" ANSI_COLOR_RESET " | "
                   ANSI_COLOR_RED "%-+11.1f%%" ANSI_COLOR_RESET "\n",
                   value2, pct_diff);
        }
    }
}

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

    if (run1->before.has_temperature && run2->before.has_temperature &&
        run2->before.temperature_c > run1->before.temperature_c + TEMP_WARNING_DELTA_C) {
        printf(ANSI_COLOR_YELLOW
               "Warning: second run started %.1f C hotter; results may have thermal bias.\n"
               ANSI_COLOR_RESET,
               run2->before.temperature_c - run1->before.temperature_c);
    }

    if (run1->before.has_frequency && run1->after.has_frequency &&
        run2->before.has_frequency && run2->after.has_frequency) {
        double avg_freq1 = (run1->before.frequency_mhz + run1->after.frequency_mhz) / 2.0;
        double avg_freq2 = (run2->before.frequency_mhz + run2->after.frequency_mhz) / 2.0;
        double freq_delta = avg_freq2 - avg_freq1;

        if (freq_delta > FREQ_WARNING_DELTA_MHZ) {
            printf(ANSI_COLOR_YELLOW
                   "Warning: %s ran with %.1f MHz higher average observed CPU frequency; timing may be biased.\n"
                   ANSI_COLOR_RESET,
                   run2->display_name, freq_delta);
        } else if (freq_delta < -FREQ_WARNING_DELTA_MHZ) {
            printf(ANSI_COLOR_YELLOW
                   "Warning: %s ran with %.1f MHz higher average observed CPU frequency; timing may be biased.\n"
                   ANSI_COLOR_RESET,
                   run1->display_name, -freq_delta);
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

static double relative_difference(double a, double b) {
    double larger = a > b ? a : b;
    double smaller = a > b ? b : a;

    if (larger == 0.0) {
        return 0.0;
    }

    return (larger - smaller) / larger;
}

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

static void print_metric_explanations(void) {
    printf("\nMetric Guide\n");
    printf("  Time Elapsed : Real wall-clock runtime. Lower is better.\n");
    printf("  CPU Cycles   : Number of processor cycles spent. Lower is better.\n");
    printf("  Instructions : Number of executed instructions. Lower can indicate a smaller work path.\n");
    printf("  IPC          : Instructions per cycle. Higher often means better CPU pipeline use.\n");
    printf("  Branch Misses: Failed branch predictions. Lower usually means less wasted work.\n");
    printf("  Temp/Freq    : Fairness context only; temperature/frequency do not normalize scores.\n");
}

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

static void trim_newline(char *text) {
    size_t len = strlen(text);

    if (len > 0 && text[len - 1] == '\n') {
        text[len - 1] = '\0';
    }
}

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

    if (!parse_positive_int(line, &parsed) || parsed < min_value || parsed > max_value) {
        return 0;
    }

    *value = parsed;
    return 1;
}

static int choose_demo_pair(int *duration_seconds, const char **binary1, const char **binary2) {
    int choice;
    int selected_duration;
    size_t i;

    printf("CLI Tool For Relative Benchmarking\n");
    printf("Select a built-in benchmark pair:\n\n");

    for (i = 0; i < sizeof(DEMO_PAIRS) / sizeof(DEMO_PAIRS[0]); i++) {
        printf("  %zu. %s\n", i + 1, DEMO_PAIRS[i].title);
        printf("     Concept: %s\n", DEMO_PAIRS[i].concept);
    }

    printf("\n");

    if (!read_menu_int("Enter choice [1-4]: ", 1, 1, 4, &choice)) {
        fprintf(stderr, "Error: invalid menu choice.\n");
        return 0;
    }

    if (!read_menu_int("Duration in seconds [10]: ", DEFAULT_DURATION_SECONDS,
                       MIN_DURATION_SECONDS, MAX_DURATION_SECONDS, &selected_duration)) {
        fprintf(stderr, "Error: invalid duration.\n");
        return 0;
    }

    *duration_seconds = selected_duration;
    *binary1 = DEMO_PAIRS[choice - 1].binary1;
    *binary2 = DEMO_PAIRS[choice - 1].binary2;

    printf("\nSelected: %s\n", DEMO_PAIRS[choice - 1].title);
    printf("Command equivalent: ./perfcmp --duration %d %s %s\n\n",
           *duration_seconds, *binary1, *binary2);

    return 1;
}

static int parse_arguments(int argc,
                           char *argv[],
                           int *duration_seconds,
                           const char **binary1,
                           const char **binary2) {
    int i;
    const char *binaries[2] = {NULL, NULL};
    int binary_count = 0;
    int demo_mode = 0;

    *duration_seconds = DEFAULT_DURATION_SECONDS;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            exit(0);
        } else if (strcmp(argv[i], "--demo") == 0) {
            demo_mode = 1;
        } else if (strcmp(argv[i], "--duration") == 0) {
            if (i + 1 >= argc || !parse_positive_int(argv[i + 1], duration_seconds)) {
                fprintf(stderr, "Error: --duration requires an integer from %d to %d seconds.\n",
                        MIN_DURATION_SECONDS, MAX_DURATION_SECONDS);
                return 0;
            }
            i++;
        } else {
            if (binary_count >= 2) {
                fprintf(stderr, "Error: too many binary paths provided.\n");
                return 0;
            }
            binaries[binary_count++] = argv[i];
        }
    }

    if (demo_mode && binary_count > 0) {
        fprintf(stderr, "Error: --demo cannot be combined with custom binary paths.\n");
        return 0;
    }

    if (demo_mode || binary_count == 0) {
        return choose_demo_pair(duration_seconds, binary1, binary2);
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

int main(int argc, char *argv[]) {
    int duration_seconds;
    const char *binary1;
    const char *binary2;
    char log1[LOG_PATH_SIZE];
    char log2[LOG_PATH_SIZE];
    BenchmarkRun run1;
    BenchmarkRun run2;
    int success = 0;

    if (!parse_arguments(argc, argv, &duration_seconds, &binary1, &binary2)) {
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

    snprintf(log1, sizeof(log1), ".perfcmp_perf_%ld_1.log", (long)getpid());
    snprintf(log2, sizeof(log2), ".perfcmp_perf_%ld_2.log", (long)getpid());
    unlink(log1);
    unlink(log2);

    memset(&run1, 0, sizeof(run1));
    memset(&run2, 0, sizeof(run2));

    run1.binary_path = binary1;
    run1.display_name = base_name(binary1);
    run1.log_path = log1;

    run2.binary_path = binary2;
    run2.display_name = base_name(binary2);
    run2.log_path = log2;

    if (run_benchmark(&run1, duration_seconds, 1) &&
        run_benchmark(&run2, duration_seconds, 2)) {
        print_metric_table(&run1, &run2);
        print_system_context_table(&run1, &run2);
        print_verdict(&run1, &run2);
        print_metric_explanations();
        success = 1;
    }

    unlink(log1);
    unlink(log2);

    return success ? 0 : 1;
}

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* Sequential scan without software prefetch hints. */
#define DEFAULT_N (1 << 24)

static double elapsed_seconds(struct timespec start, struct timespec end) {
    return (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
}

int main(int argc, char *argv[]) {
    int n = DEFAULT_N;
    if (argc > 1) {
        int parsed = atoi(argv[1]);
        if (parsed > 0 && parsed <= (1 << 26))
            n = parsed;
    }

    float *arr = malloc(n * sizeof(*arr));
    volatile double checksum = 0.0;
    struct timespec start;
    struct timespec end;

    if (arr == NULL) {
        perror("malloc");
        return 1;
    }

    for (int i = 0; i < n; i++) {
        arr[i] = (float)i;
    }

    printf("=== Without Prefetch ===\n");
    printf("Workload: %.0f MB float array\n\n", n * sizeof(*arr) / 1e6);

    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < n; i++) {
        checksum += arr[i];
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    printf("Time: %.4f seconds\n", elapsed_seconds(start, end));
    printf("Checksum: %.0f\n", checksum);
    printf("Concept: data is requested only when each element is reached.\n");

    free(arr);
    return 0;
}

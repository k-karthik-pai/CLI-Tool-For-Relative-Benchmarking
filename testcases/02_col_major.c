#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* Demonstrates cache-unfriendly column-style traversal in C arrays. */
#define DEFAULT_N 4096

static double elapsed_seconds(struct timespec start, struct timespec end) {
    return (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
}

int main(int argc, char *argv[]) {
    int n = DEFAULT_N;
    if (argc > 1) {
        int parsed = atoi(argv[1]);
        if (parsed > 0 && parsed <= 16384)
            n = parsed;
    }

    double *matrix = malloc((size_t)n * n * sizeof(double));
    if (matrix == NULL) {
        perror("malloc");
        return 1;
    }

    struct timespec start;
    struct timespec end;
    double checksum = 0.0;

    printf("=== Column Major Access ===\n");
    printf("Workload: %d x %d double matrix\n\n", n, n);

    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int j = 0; j < n; j++) {
        for (int i = 0; i < n; i++) {
            matrix[i * n + j] = (double)(i * n + j);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    for (int i = 0; i < n; i += 512) {
        for (int j = 0; j < n; j += 512) {
            checksum += matrix[i * n + j];
        }
    }

    printf("Time: %.4f seconds\n", elapsed_seconds(start, end));
    printf("Checksum: %.0f\n", checksum);
    printf("Concept: large memory strides reduce cache locality.\n");

    free(matrix);
    return 0;
}

#include <stdio.h>
#include <time.h>

/* Demonstrates cache-friendly row-major traversal in C's row-major arrays. */
#define N 4096

static double matrix[N][N];

static double elapsed_seconds(struct timespec start, struct timespec end) {
    return (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
}

int main(void) {
    struct timespec start;
    struct timespec end;
    double checksum = 0.0;

    printf("=== Row Major Access ===\n");
    printf("Workload: %d x %d double matrix\n\n", N, N);

    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            matrix[i][j] = (double)(i * N + j);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    for (int i = 0; i < N; i += 512) {
        for (int j = 0; j < N; j += 512) {
            checksum += matrix[i][j];
        }
    }

    printf("Time: %.4f seconds\n", elapsed_seconds(start, end));
    printf("Checksum: %.0f\n", checksum);
    printf("Concept: sequential row-wise access improves spatial locality.\n");
    return 0;
}

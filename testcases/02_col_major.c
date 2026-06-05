#include <stdio.h>
#include <time.h>

/* Demonstrates cache-unfriendly column-style traversal in C arrays. */
#define N 4096

static double matrix[N][N];

static double elapsed_seconds(struct timespec start, struct timespec end) {
    return (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
}

int main(void) {
    struct timespec start;
    struct timespec end;
    double checksum = 0.0;

    printf("=== Column Major Access ===\n");
    printf("Workload: %d x %d double matrix\n\n", N, N);

    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int j = 0; j < N; j++) {
        for (int i = 0; i < N; i++) {
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
    printf("Concept: large memory strides reduce cache locality.\n");
    return 0;
}

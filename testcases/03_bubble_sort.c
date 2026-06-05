#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* Demonstrates an O(n^2) sort with many adjacent swaps and branch checks. */
#define N 20000

static double elapsed_seconds(struct timespec start, struct timespec end) {
    return (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
}

int main(void) {
    int *arr;
    struct timespec start;
    struct timespec end;
    long long checksum = 0;

    srand(42);
    arr = malloc(N * sizeof(*arr));
    if (arr == NULL) {
        perror("malloc");
        return 1;
    }

    for (int i = 0; i < N; i++) {
        arr[i] = rand() % 100000;
    }

    printf("=== Bubble Sort ===\n");
    printf("Workload: %d integers\n\n", N);

    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < N - 1; i++) {
        for (int j = 0; j < N - i - 1; j++) {
            if (arr[j] > arr[j + 1]) {
                int tmp = arr[j];
                arr[j] = arr[j + 1];
                arr[j + 1] = tmp;
            }
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    for (int i = 0; i < N; i += 1000) {
        checksum += arr[i];
    }

    printf("Time: %.4f seconds\n", elapsed_seconds(start, end));
    printf("Checksum: %lld\n", checksum);
    printf("Concept: O(n^2) comparisons with many swaps and writes.\n");

    free(arr);
    return 0;
}

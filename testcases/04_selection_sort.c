#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* Demonstrates an O(n^2) sort with fewer swaps than bubble sort. */
#define DEFAULT_N 20000

static double elapsed_seconds(struct timespec start, struct timespec end) {
    return (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
}

int main(int argc, char *argv[]) {
    int n = DEFAULT_N;
    if (argc > 1) {
        int parsed = atoi(argv[1]);
        if (parsed > 0 && parsed <= 100000)
            n = parsed;
    }

    int *arr;
    struct timespec start;
    struct timespec end;
    long long checksum = 0;

    srand(42);
    arr = malloc(n * sizeof(*arr));
    if (arr == NULL) {
        perror("malloc");
        return 1;
    }

    for (int i = 0; i < n; i++) {
        arr[i] = rand() % 100000;
    }

    printf("=== Selection Sort ===\n");
    printf("Workload: %d integers\n\n", n);

    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < n - 1; i++) {
        int min_idx = i;

        for (int j = i + 1; j < n; j++) {
            if (arr[j] < arr[min_idx]) {
                min_idx = j;
            }
        }

        if (min_idx != i) {
            int tmp = arr[min_idx];
            arr[min_idx] = arr[i];
            arr[i] = tmp;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    for (int i = 0; i < n; i += 1000) {
        checksum += arr[i];
    }

    printf("Time: %.4f seconds\n", elapsed_seconds(start, end));
    printf("Checksum: %lld\n", checksum);
    printf("Concept: O(n^2) comparisons but only O(n) swaps.\n");

    free(arr);
    return 0;
}

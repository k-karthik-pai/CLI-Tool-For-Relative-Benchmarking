#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* Demonstrates automatic stack allocation for fixed-size local buffers. */
#define DEFAULT_ITERS 1000000
#define BLOCK_SIZE 256

static double elapsed_seconds(struct timespec start, struct timespec end) {
    return (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
}

static unsigned int touch(volatile char *p, int n) {
    unsigned int checksum = 0;

    for (int i = 0; i < n; i += 64) {
        p[i] = (char)i;
        checksum += (unsigned char)p[i];
    }

    return checksum;
}

int main(int argc, char *argv[]) {
    int iters = DEFAULT_ITERS;
    if (argc > 1) {
        int parsed = atoi(argv[1]);
        if (parsed > 0 && parsed <= 10000000)
            iters = parsed;
    }

    struct timespec start;
    struct timespec end;
    unsigned long long checksum = 0;

    printf("=== Stack Allocation ===\n");
    printf("Workload: %d-byte block, %d iterations\n\n", BLOCK_SIZE, iters);

    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < iters; i++) {
        char p[BLOCK_SIZE];
        checksum += touch(p, BLOCK_SIZE);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    printf("Time: %.4f seconds\n", elapsed_seconds(start, end));
    printf("Checksum: %llu\n", checksum);
    printf("Concept: stack allocation is automatic and avoids malloc/free metadata.\n");
    return 0;
}

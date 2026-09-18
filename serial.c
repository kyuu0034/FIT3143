#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>

int main(int argc, char *argv[]) {

    /* n comes from the command line, e.g. ./serial 10000000 */
    if (argc != 2) {
        printf("Usage: %s n\n", argv[0]);
        return 1;
    }
    int n = atoi(argv[1]);

    struct timespec start, end, startComp, endComp;
    double time_taken;

    /* Overall timer starts right after n is known. */
    clock_gettime(CLOCK_MONOTONIC, &start);

    FILE *fptr = fopen("output.txt", "w");

    /* prime[num] = 1 means num is prime */
    int *prime = calloc(n, sizeof(int));
    if (n > 2) prime[2] = 1;

    /* Computational timer includes only the search work */
    clock_gettime(CLOCK_MONOTONIC, &startComp);

    for (int num = 3; num < n; num += 2) {
        int is_prime = 1; /* Assume the number is prime */
        double limit = sqrt(num);

        for (int i = 3; i <= limit; i += 2) {
            if (num % i == 0) {
                is_prime = 0;
                break; /* Divisor found, stop checking */
            }
        }
        prime[num] = is_prime;
    }

    clock_gettime(CLOCK_MONOTONIC, &endComp);
    time_taken = (endComp.tv_sec - startComp.tv_sec) * 1e9;
    time_taken = (time_taken + (endComp.tv_nsec - startComp.tv_nsec)) * 1e-9;
    printf("Prime search complete - Computational time only(s): %lf\n", time_taken);

    /* Write results - not part of the computational timer.
       Fast write: build one big text buffer, then write it once. */
    char *buf = malloc((size_t)n * 11 + 1);
    char *p = buf;
    for (int num = 2; num < n; num++) {
        if (prime[num]) {
            p += sprintf(p, "%d ", num);
        }
    }
    fwrite(buf, 1, p - buf, fptr);
    fclose(fptr);
    free(buf);
    free(prime);

    /* Overall timer stops after everything: setup, search, write and cleanup */
    clock_gettime(CLOCK_MONOTONIC, &end);
    time_taken = (end.tv_sec - start.tv_sec) * 1e9;
    time_taken = (time_taken + (end.tv_nsec - start.tv_nsec)) * 1e-9;

    printf("Overall time (Including setup, search and write)(s): %lf. Results saved to output.txt.\n", time_taken);

    return 0;
}






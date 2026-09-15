#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <omp.h>

int main() {

    /* Get user input for n */
    int n;
    printf("Find all prime numbers strictly less than which number? \n");
    scanf("%d", &n);

    struct timespec start, end, startComp, endComp;
    double time_taken;

    /* Overall timer starts right after n is known. */
    clock_gettime(CLOCK_MONOTONIC, &start);

    FILE *fptr = NULL; /* Only needed if n >= 100 */
    if (n >= 100) {
        fptr = fopen("output.txt", "w");
    }

    /* prime[num] = 1 means num is prime */
    int *prime = calloc(n, sizeof(int));
    if (n > 2) prime[2] = 1;

    /* Computational timer includes only the search work */
    clock_gettime(CLOCK_MONOTONIC, &startComp);

    /* Pragma splits the iterations of this loop across the threads
        schedule(dynamic, 1000) hands out 1000 iterations at a time,
        so a thread that finishes early comes back for more. */
    #pragma omp parallel for schedule(dynamic, 1000)
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

    /* Write results - not part of the computational timer */
    for (int num = 2; num < n; num++) {
        if (prime[num]) {
            if (n < 100) {
                printf("%d ", num);
            } else {
                fprintf(fptr, "%d ", num);
            }
        }
    }

    if (n >= 100) fclose(fptr);
    free(prime);

    /* Overall timer stops after everything: setup, search, write and cleanup */
    clock_gettime(CLOCK_MONOTONIC, &end);
    time_taken = (end.tv_sec - start.tv_sec) * 1e9;
    time_taken = (time_taken + (end.tv_nsec - start.tv_nsec)) * 1e-9;

    if (n < 100) {
        printf("\nOverall time (Including setup, search and write)(s): %lf\n", time_taken);
    } else {
        printf("\nOverall time (Including setup, search and write)(s): %lf. Results saved to output.txt.\n", time_taken);
    }

    return 0;
}

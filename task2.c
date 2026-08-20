#include <stdio.h> 
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#define NUM_THREADS 4

/* Declare n and prime as global variables */
int n;
int *prime;

/* Function executed by each thread*/
void *find_primes(void *arg)
{
    int thread_id = *(int *)arg;

    for (int num = 3 + (2*thread_id); num < n; num += 2 * NUM_THREADS) {
        int is_prime = 1; /* Assume the number is prime */
        double limit = sqrt(num);

        /* Inner loop: Test odd divisors */
        for (int i = 3; i <= limit; i += 2) {
            if (num % i == 0) {
                is_prime = 0;
                break;        /* Divisor found, stop checking */
            }
        }

        /*Store result*/
        prime[num] = is_prime;

    }
    return NULL;
}

int main() {
 
    /* Get user input for n */
    printf("Find all prime numbers strictly less than which number? \n");
    scanf("%d", &n);
 
    struct timespec start, end, startComp, endComp;
    double time_taken;
 
    /* Overall timer starts right after n is known */
    clock_gettime(CLOCK_MONOTONIC, &start);
 
    FILE *fptr = NULL; /* Only needed if n >= 100 */
    if (n >= 100) {
        fptr = fopen("output.txt", "w");
    }
 
    prime = calloc(n, sizeof(int));
    if (n > 2) prime[2] = 1;
 
    pthread_t threads[NUM_THREADS];
    int thread_id[NUM_THREADS];
 
    /* Computational timer includes only the parallel search work */
    clock_gettime(CLOCK_MONOTONIC, &startComp);
 
    for (int t = 0; t < NUM_THREADS; t++) {
        thread_id[t] = t;
        pthread_create(&threads[t], NULL, find_primes, &thread_id[t]);
    }
    for (int t = 0; t < NUM_THREADS; t++) {
        pthread_join(threads[t], NULL);
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

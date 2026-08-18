#include <stdio.h> 
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#define NUM_THREADS 4

/* Declare n and prime as global variables */
int n;
int prime 

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

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

    FILE *fptr = NULL; /*By default set file pointer to NULL, it is only needed if n >= 100*/

    if (n>= 100){
        fptr = fopen("output.txt", "w");
    }

    /* Print 2 first, as it is the only even prime */
    if (n > 2) {
        if (n<100){
                printf("2 ");
            }
            else {
                fprintf(fptr, "2 ");
            }
    }

    /* Outer loop: Check every odd number from 3 up to but excluding n */
    #pragma omp parallel for schedule(dynamic, 1000)
    for (int num = 3; num < n; num += 2) {
        int is_prime = 1; /* Assume the number is prime */
        double limit = sqrt(num);

        /* Inner loop: Test odd divisors */
        for (int i = 3; i <= limit; i += 2) {
            if (num % i == 0) {
                is_prime = 0;
                break;        /* Divisor found, stop checking */
            }
        }

        /* If no divisors were found, print the number */
        if (is_prime) {
            #pragma omp critical
            if (n<100){
                printf("%d ", num);
            }
            else {
                fprintf(fptr, "%d ", num);
    
            }
            
        }
    }
    
    /* If n was greater than 100, close the txt file*/
    if (n < 100) {
        printf("\nCode finished executing.\n");
    } else {
        printf("\nCode finished executing. Results saved to output.txt.\n");
        fclose(fptr);
    }
    return 0;
}

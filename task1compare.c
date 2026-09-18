#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <mpi.h>
/*
General structure by Kevin Yu (33184828)
AI assisted in optimisation and error fixing
*/


/* used by qsort to sort ints in ascending order */
int cmp(const void *a, const void *b) {
    return (*(int *)a - *(int *)b);
}

int main(int argc, char *argv[]) {
    int rank, size;

    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    /* only root reads n and the distribution mode from the command line.
       mode 0 = cyclic (default), mode 1 = block */
    int n = 0, mode = 0;
    if (rank == 0) {
        if (argc < 2) {
            printf("Usage: mpirun -np <procs> %s n [cyclic|block]\n", argv[0]);
            MPI_Abort(MPI_COMM_WORLD, 1);
        }
        n = atoi(argv[1]);
        if (argc >= 3 && strcmp(argv[2], "block") == 0) mode = 1;
    }

    /* ---- phase timers ---- */
    double t_start = MPI_Wtime();

    /* send n and mode from root to every process */
    MPI_Bcast(&n, 1, MPI_INT, 0, MPI_COMM_WORLD);
    MPI_Bcast(&mode, 1, MPI_INT, 0, MPI_COMM_WORLD);

    /* each process keeps its own list of primes */
    int *local = malloc(n * sizeof(int));
    int local_count = 0;

    if (rank == 0 && n > 2) local[local_count++] = 2;   /* root handles 2 */

    double t_comp_start = MPI_Wtime();

    /* work out this process's loop: start, end (exclusive) and step.
       Only odd numbers from 3 up to n-1 are tested. */
    int start, end, step;

    if (mode == 0) {
        /* cyclic: 3 + 2*rank, then every 2*size-th odd number */
        start = 3 + 2 * rank;
        end   = n;
        step  = 2 * size;
    } else {
        /* block: split the odd numbers into contiguous equal chunks.
           There are 'odds' odd numbers in [3, n). Process i gets a slice of them. */
        int odds  = (n > 3) ? (n - 3 + 1) / 2 : 0;   /* count of odd numbers in [3, n) */
        int chunk = odds / size;
        int extra = odds % size;                     /* first 'extra' ranks get one more */
        int first_idx = rank * chunk + (rank < extra ? rank : extra);
        int last_idx  = first_idx + chunk + (rank < extra ? 1 : 0);
        start = 3 + 2 * first_idx;
        end   = 3 + 2 * last_idx;
        step  = 2;
    }

    for (int num = start; num < end; num += step) {
        int is_prime = 1;
        double limit = sqrt(num);
        for (int i = 3; i <= limit; i += 2) {
            if (num % i == 0) { is_prime = 0; break; }
        }
        if (is_prime) local[local_count++] = num;
    }

    /* each process measures its own compute time; root keeps the slowest */
    double my_comp = MPI_Wtime() - t_comp_start;
    double max_comp;
    MPI_Reduce(&my_comp, &max_comp, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);

    double t_gather_start = MPI_Wtime();

    /* 1. root collects how many primes each process found */
    int *counts = NULL, *displs = NULL, *all = NULL;
    if (rank == 0) counts = malloc(size * sizeof(int));

    MPI_Gather(&local_count, 1, MPI_INT, counts, 1, MPI_INT, 0, MPI_COMM_WORLD);

    /* 2. root works out where each process's data goes in the big array */
    int total = 0;
    if (rank == 0) {
        displs = malloc(size * sizeof(int));
        for (int i = 0; i < size; i++) {
            displs[i] = total;
            total += counts[i];
        }
        all = malloc(total * sizeof(int));
    }

    /* 3. root collects the primes themselves */
    MPI_Gatherv(local, local_count, MPI_INT,
                all, counts, displs, MPI_INT, 0, MPI_COMM_WORLD);

    /* root sorts and writes the result */
    if (rank == 0) {
        double t_sort_start = MPI_Wtime();
        qsort(all, total, sizeof(int), cmp);

        double t_write_start = MPI_Wtime();

        /* fast write: build one big text buffer, then write it once.
           Each int needs at most 11 chars ("2147483647 ") */
        char *buf = malloc((size_t)total * 11 + 1);
        char *p = buf;
        for (int i = 0; i < total; i++) {
            p += sprintf(p, "%d ", all[i]);
        }

        FILE *fptr = fopen("output.txt", "w");
        fwrite(buf, 1, p - buf, fptr);
        fclose(fptr);
        free(buf);

        double t_end = MPI_Wtime();

        printf("Found %d primes. Results saved to output.txt.\n", total);
        printf("  compute (slowest process): %lf s\n", max_comp);
        printf("  gather:                    %lf s\n", t_sort_start - t_gather_start);
        printf("  sort:                      %lf s\n", t_write_start - t_sort_start);
        printf("  write:                     %lf s\n", t_end - t_write_start);
        printf("Overall time (s): %lf\n", t_end - t_start);

        free(counts); free(displs); free(all);
    }

    free(local);
    MPI_Finalize();
    return 0;
}
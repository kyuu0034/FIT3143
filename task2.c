#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <mpi.h>
#include <omp.h>

/*
 * FIT3143 Lab 2 - Task 2: Prime search with hybrid Open MPI + OpenMP
 *
 * Same architecture as Task 1 (MPI-only), with OpenMP added inside each
 * process:
 *
 * Workload distribution: cyclic across MPI processes. Process r tests the
 * odd numbers 3 + 2r, 3 + 2r + 2*size, 3 + 2r + 4*size, ... so every
 * process gets an even mix of small and large numbers. Within a process,
 * that same cyclic range is further split across OpenMP threads via
 * "#pragma omp parallel for schedule(dynamic, 1000)".
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

    /* only root reads n from the command line */
    int n = 0;
    if (rank == 0) {
        if (argc != 2) {
            printf("Usage: mpirun -np <procs> %s n\n", argv[0]);
            MPI_Abort(MPI_COMM_WORLD, 1);
        }
        n = atoi(argv[1]);
    }

    /* overall timer: starts once n is known, stops after file is written */
    double t_start = MPI_Wtime();

    /* send n from root to every process */
    MPI_Bcast(&n, 1, MPI_INT, 0, MPI_COMM_WORLD);

    /* each process keeps its own list of primes, built by all its threads */
    int *local = malloc(n * sizeof(int));
    int local_count = 0;

    if (rank == 0 && n > 2) local[local_count++] = 2;   /* root handles 2 */

    double t_comp_start = MPI_Wtime();

    /* cyclic distribution across processes: start at 3 + 2*rank, step by
       2*size (odd numbers only). Within that range, OpenMP threads further
       split the work; each thread claims its insertion slot atomically
       since local_count is shared by every thread in this process. */
    #pragma omp parallel for schedule(dynamic, 1000)
    for (int num = 3 + 2 * rank; num < n; num += 2 * size) {
        int is_prime = 1;
        double limit = sqrt(num);
        for (int i = 3; i <= limit; i += 2) {
            if (num % i == 0) { is_prime = 0; break; }
        }
        if (is_prime) {
            int idx;
            #pragma omp atomic capture
            idx = local_count++;
            local[idx] = num;
        }
    }

    /* each process measures its own compute time (all its threads included);
       root keeps the slowest, same as the MPI-only version */
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

    /* 3. root collects the primes themselves. Order within each rank's
       segment is not sorted (OpenMP threads may finish out of order), so
       the following qsort on root is what guarantees the final order. */
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
        printf("  MPI processes:             %d\n", size);
        printf("  OpenMP threads/process:    %d\n", omp_get_max_threads());
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

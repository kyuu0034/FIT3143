#!/bin/bash
# sweep.sh - collect communication delay data for Lab 3 Task 3.
#
# Usage:   ./sweep.sh grid     (2-D grids: delay vs EV grid size)
#          ./sweep.sh line     (1 x n line topologies: delay vs line length)
#
# Runs both the blocking and non-blocking versions, REPEATS times each.
# Results append to timing.csv (blocking) and timing_nonblocking.csv (non-blocking).
#
# Works locally with mpirun, or inside a CAAS SLURM job with srun.

MODE=${1:-grid}
PORTS=6           # k, charging ports per node
THRESHOLD=2       # "low" means this many free ports or fewer
SECONDS_PER_RUN=3 # how long each simulation runs
REPEATS=2         # repeat each configuration this many times
MAX_PROCS=31      # CAAS allows 32 MPI processes per job; 1 is the base station

# Use srun on CAAS, mpirun everywhere else.
if command -v srun > /dev/null 2>&1 && [ -n "$SLURM_JOB_ID" ]; then
    LAUNCH() { srun --ntasks=$1 --overlap ./$2 $3 $4 $PORTS $THRESHOLD $SECONDS_PER_RUN; }
else
    LAUNCH() { mpirun --oversubscribe -np $1 ./$2 $3 $4 $PORTS $THRESHOLD $SECONDS_PER_RUN; }
fi

# ---- build the list of configurations ----
CONFIGS=""
if [ "$MODE" = "line" ]; then
    # line topology: 1 x n, for n = 2 .. 31  (30 instances)
    for n in $(seq 2 31); do
        CONFIGS="$CONFIGS 1,$n"
    done
else
    # 2-D grids: every m x n with 2 <= m <= n and m*n <= 31  (28 sizes)
    for m in 2 3 4 5; do
        for n in $(seq $m 15); do
            [ $((m * n)) -le $MAX_PROCS ] || break
            CONFIGS="$CONFIGS $m,$n"
        done
    done
fi

TOTAL=$(echo $CONFIGS | wc -w)
echo "Mode: $MODE   configurations: $TOTAL   repeats: $REPEATS   seconds/run: $SECONDS_PER_RUN"
echo "Estimated time: $(( TOTAL * REPEATS * 2 * (SECONDS_PER_RUN + 2) / 60 )) minutes"
echo

i=0
for cfg in $CONFIGS; do
    m=${cfg%,*}
    n=${cfg#*,}
    procs=$(( m * n + 1 ))
    i=$((i + 1))

    for r in $(seq 1 $REPEATS); do
        LAUNCH $procs task1   $m $n > /dev/null 2>&1
        LAUNCH $procs task1nb $m $n > /dev/null 2>&1
    done
    echo "[$i/$TOTAL] ${m}x${n} (${procs} processes) done"
done

echo
echo "Finished. Rows in timing.csv: $(( $(wc -l < timing.csv) - 1 ))"
echo "          Rows in timing_nonblocking.csv: $(( $(wc -l < timing_nonblocking.csv) - 1 ))"

#!/bin/bash
# sweep.sh - collect communication delay data for Lab 3 Task 3.
#
# Usage:   ./sweep.sh grid     (2-D grids: delay vs EV grid size)
#          ./sweep.sh line     (1 x n line topologies: delay vs line length)
#
# Runs BOTH the blocking (task1) and non-blocking (task1nb) versions for every
# configuration, interleaved, so both see the same cluster conditions.
# Results append to timing.csv (blocking) and timing_nonblocking.csv.
#
# CAAS limits: 10 min per job, one running job per user, 32 MPI tasks per job.
# One pass (REPEATS=1) takes about 5 minutes, so submit the job several times
# (see submit_all.sh) rather than raising REPEATS or --time.
#
# Works on CAAS (srun) or locally (mpirun --oversubscribe).

MODE=${1:-grid}
PORTS=${PORTS:-6}                       # k, charging ports per node
THRESHOLD=${THRESHOLD:-2}               # "low" means this many free ports or fewer
SECONDS_PER_RUN=${SECONDS_PER_RUN:-3}   # how long each simulation runs
REPEATS=${REPEATS:-1}                   # passes per configuration inside this job
MAX_PROCS=31                            # 32 tasks per job; 1 is the base station
LINE_MAX=${LINE_MAX:-31}                # longest line to test (lower it for quick local tests)

if command -v srun > /dev/null 2>&1 && [ -n "$SLURM_JOB_ID" ]; then
    LAUNCH() { srun --ntasks=$1 --overlap ./$2 $3 $4 $PORTS $THRESHOLD $SECONDS_PER_RUN; }
else
    LAUNCH() { mpirun --oversubscribe -np $1 ./$2 $3 $4 $PORTS $THRESHOLD $SECONDS_PER_RUN; }
fi

CONFIGS=""
if [ "$MODE" = "line" ]; then
    for n in $(seq 2 $LINE_MAX); do CONFIGS="$CONFIGS 1,$n"; done
else
    for m in 2 3 4 5; do
        for n in $(seq $m 15); do
            [ $((m * n)) -le $MAX_PROCS ] || break
            CONFIGS="$CONFIGS $m,$n"
        done
    done
fi

TOTAL=$(echo $CONFIGS | wc -w)
echo "Mode: $MODE   configurations: $TOTAL   repeats: $REPEATS   seconds/run: $SECONDS_PER_RUN"
echo "Estimated time: $(( TOTAL * REPEATS * 2 * (SECONDS_PER_RUN + 2) / 60 )) minutes (job limit is 10)"
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

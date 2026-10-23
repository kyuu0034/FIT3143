#!/bin/bash
# period_test.sh - Task 2: test different base-station report periods.
# Runs a 4x4 grid (17 processes) with a high threshold so congestion is common,
# for several period values, 3 runs each. Saves each base station log in logs/.
# Work happens in period_run/ so its CSV rows never mix into timing.csv.
ROOT=$(pwd)
M=4; N=4; PORTS=6; THRESHOLD=4; SECONDS_RUN=${SECONDS_RUN:-10}; REPEATS=${REPEATS:-3}
PERIODS="0.5 1 2 5"

if command -v srun > /dev/null 2>&1 && [ -n "$SLURM_JOB_ID" ]; then
    LAUNCH() { srun --ntasks=$1 --overlap $ROOT/$2 $3 $4 $PORTS $THRESHOLD $SECONDS_RUN $5; }
else
    LAUNCH() { mpirun --oversubscribe -np $1 $ROOT/$2 $3 $4 $PORTS $THRESHOLD $SECONDS_RUN $5; }
fi

mkdir -p logs period_run
cd period_run
for p in $PERIODS; do
    for r in $(seq 1 $REPEATS); do
        LAUNCH $(( M * N + 1 )) task1 $M $N $p > /dev/null 2>&1
        cp base_station_log.txt $ROOT/logs/period_${p}_run${r}.txt
        echo "period $p run $r done"
    done
done

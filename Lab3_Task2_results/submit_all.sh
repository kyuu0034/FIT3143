#!/bin/bash
# Run on the CAAS head node (submitting is fine, running programs is not).
# CAAS lets only one of your jobs run at a time, so these queue up and run
# one after another (about 5 minutes each).
rm -f timing.csv timing_nonblocking.csv      # start clean: no old data mixed in
PASSES=${1:-4}                               # how many times to repeat each sweep
for p in $(seq 1 $PASSES); do
    sbatch sweep_grid.job
    sbatch sweep_line.job
done
sbatch period_test.job
echo "Submitted $((PASSES * 2 + 1)) jobs. Watch them with:  squeue -u \$USER"

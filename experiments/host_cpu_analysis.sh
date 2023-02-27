#!/bin/bash

echo "Running the perf experiment"

echo "Setting /proc/sys/kernel/perf_event_paranoid to -1 for perf. A sudo prompt might come up."
sudo sh -c "echo -1 > /proc/sys/kernel/perf_event_paranoid"

RUNTIME="600"
REPS=5

# We are doing -a (system wide profiling) to take the RX path into account that partially doesn't get attributed to the process.
# Delay the perf measuring by 10s because of the fio startup and warmup time
PERF_T1_METRICS="cpu-clock,cycles,instructions,branches,branch-misses"
echo "Running: perf CPU cycle analysis T1 for ${RUNTIME}s seconds with a fio stress load. Measuring the following metrics: ${PERF_T1_METRICS}"
for i in $(seq 1 $REPS); do
	perf stat -a -x "," --delay 10000 --all-kernel -e $PERF_T1_METRICS -- env RW=randrw BS=4k IODEPTH=128 P=2 RUNTIME=${RUNTIME}s ./workloads/fio.sh 1> $OUT/cpu_load_T1_fio_${i}.out 2> $OUT/cpu_load_T1_perf_${i}.out
done

PERF_T2_METRICS="L1-dcache-load-misses,L1-dcache-loads"
echo "Running: perf CPU cycle analysis T2 for ${RUNTIME}s seconds with a fio stress load. Measuring the following metrics: ${PERF_T2_METRICS}"
for i in $(seq 1 $REPS); do
	perf stat -a -x "," --delay 10000 --all-kernel -e $PERF_T2_METRICS -- env RW=randrw BS=4k IODEPTH=128 P=2 RUNTIME=${RUNTIME}s ./workloads/fio.sh 1> $OUT/cpu_load_T2_fio_${i}.out 2> $OUT/cpu_load_T2_perf_${i}.out
done

PERF_T3_METRICS="dTLB-loads,dTLB-loads-misses"
echo "Running: perf CPU cycle analysis T3 for ${RUNTIME}s seconds with a fio stress load. Measuring the following metrics: ${PERF_T3_METRICS}"
for i in $(seq 1 $REPS); do
	perf stat -a -x "," --delay 10000 --all-kernel -e $PERF_T3_METRICS -- env RW=randrw BS=4k IODEPTH=128 P=2 RUNTIME=${RUNTIME}s ./workloads/fio.sh 1> $OUT/cpu_load_T3_fio_${i}.out 2> $OUT/cpu_load_T3_perf_${i}.out
done

# We take the baseline last because the libnfs back-end on the DPU still times out sometimes, PoC mode ;)
echo "Giving the system some rest before starting the baseline measurments (60 seconds)"
sleep 60

PERF_BASELINE_METRICS="cpu-clock,cycles,instructions"
echo "Running: perf CPU cycle analysis for ${RUNTIME}s seconds without a load (baseline). Measuring the following metrics: ${PERF_BASELINE_METRICS}"
for i in $(seq 1 $REPS); do
	perf stat -a -x "," --delay 10000 --all-kernel -e $PERF_BASELINE_METRICS -- sleep 610 2> $OUT/cpu_baseline_perf_${i}.out
done

#!/bin/bash

echo "Welcome to the dpu-virtio-fs workload runner! :)"
echo "Please run this script with numactl to bind all the workloads to the NUMA node on which the device is located"
echo "Furthermore please make sure that any resource hogs have been removed from the system"

if [ -z $MNT ]; then
	echo "You must set the MNT env variable to where you want to run the workloads!"
	exit 1
fi

if perf 2>&1 | grep "WARNING: perf not found for kernel"; then
	exit 1
fi

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
cd $SCRIPT_DIR

TIMESTAMP=$(date +"%Y-%m-%d_%T")
HOST=$(uname -n)
COMMIT=$(git rev-parse --short HEAD)
OUT=./output/${COMMIT}_${HOST}_${TIMESTAMP}

echo "Setting CPU/DMA latency to 0. A sudo prompt will now come up"
gcc setcpulatency.c -o setcpulatency
sudo ./setcpulatency 0 & 

echo "Setting /proc/sys/kernel/perf_event_paranoid to -1 for perf."
sudo sh -c "echo -1 > /proc/sys/kernel/perf_event_paranoid"

echo "The output will be stored under $OUT"
echo "This run.sh will take $(python3 -c 'print(round((2*7*25 + 7*8*5*25 + 3*6*8*5*25 + 600)/60/60, 2))') hours."

echo "START"
mkdir -p $OUT

echo "Running: fio latency benchmarks"
for RW in "randread" "randwrite"; do
	for BS in "1" "4k" "8k" "16k" "32k" "64k" "128k"; do
		for IODEPTH in 1; do
			for P in 1; do
				echo fio RW=$RW BS=$BS IODEPTH=$IODEPTH P=$P
				RW=$RW BS=$BS IODEPTH=$IODEPTH P=$P ./workloads/fio.sh > $OUT/fio_${RW}_${BS}_${IODEPTH}_${P}.out
			done
		done
	done
done

echo "Running: fio IOPS benchmarks"
for RW in "randrw"; do
	for BS in "1" "4k" "8k" "16k" "32k" "64k" "128k"; do
		for IODEPTH in 1 2 4 8 16 32 64 128; do
			for P in 1 2 4 8 16; do
				echo fio RW=$RW BS=$BS IODEPTH=$IODEPTH P=$P
				RW=$RW BS=$BS IODEPTH=$IODEPTH P=$P ./workloads/fio.sh > $OUT/fio_${RW}_${BS}_${IODEPTH}_${P}.out
			done
		done
	done
done

echo "Running: fio read, write throughput benchmarks"
for RW in "read" "write"; do
	for BS in "4k" "8k" "16k" "32k" "64k" "128k"; do
		for IODEPTH in 1 2 4 8 16 32 64 128; do
			for P in 1 2 4 8 16; do
				echo fio RW=$RW BS=$BS IODEPTH=$IODEPTH P=$P
				RW=$RW BS=$BS IODEPTH=$IODEPTH P=$P ./workloads/fio.sh > $OUT/fio_${RW}_${BS}_${IODEPTH}_${P}.out
			done
		done
	done
done

# Single operation latency
echo "Running: stat (getattr) latency"
gcc ./workloads/lat/lat_stat.c -O3 -o ./workloads/lat/lat_stat
./workloads/lat/lat_stat $MNT/test 100000 1 > $OUT/lat_stat.out
echo "Running: statfs latency"
gcc ./workloads/lat/lat_statfs.c -O3 -o ./workloads/lat/lat_statfs
./workloads/lat/lat_statfs $MNT 100000 1 > $OUT/lat_statfs.out

sudo pkill setcpulatency
echo "Reset CPU/DMA latency to default value"

RUNTIME="300"
echo "Running: perf CPU cycle analysis for ${RUNTIME}s seconds without a load (baseline)"
# We are doing -a (system wide profiling) to take the RX path into account that partially doesn't get attributed to the process.
perf stat -a -x "," -- sleep $RUNTIME 2> $OUT/cpu_baseline_perf.out

echo "Running: perf CPU cycle analysis for ${RUNTIME}s seconds with a fio stress load"
# We subtract 10s because fio by default warms up for 10s
PERF_FIO_RUNTIME="$(echo ${RUNTIME}-10 | bc)s"
perf stat -a -x "," -- env RW=randrw BS=4k IODEPTH=128 P=1 RUNTIME=$PERF_FIO_RUNTIME ./workloads/fio.sh 1> $OUT/cpu_load_fio.out 2> $OUT/cpu_load_perf.out

echo "DONE"

echo If the experiment was successful and the results are verified, please move them to the results folder of the implementation that was used

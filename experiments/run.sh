#!/bin/bash

echo "Welcome to the dpu-virtio-fs workload runner! :)"
echo "Please run this script with numactl to bind all the workloads to the NUMA node on which the device is located"

if [ -z $MNT ]; then
	echo "You must set the MNT env variable to where you want to run the workloads!"
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

mkdir -p $OUT
echo "The output will be stored under $OUT"
echo "This run.sh will take $(python -c 'print(round((2*7*25 + 7*8*5*25 + 3*6*8*5*25 + 600)/60/60, 2))') hours."

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
for RW in "read" "write" "randrw"; do
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
gcc ./lat/lat_stat.c -O3 -o ./lat/lat_stat
./lat/lat_stat $MNT/test 50000 1 > $OUT/lat_stat.out
echo "Running: statfs latency"
gcc ./lat/lat_statfs.c -O3 -o ./lat/lat_statfs
./lat/lat_statfs $MNT 50000 1 > $OUT/lat_statfs.out

pkill setcpulatency
echo "Reset CPU/DMA latency to default value"

RUNTIME="300s"
echo "Running: perf CPU cycle analysis for $RUNTIME seconds with a fio stress load"
# We are doing -a (system wide profiling) to take the RX path into account that partially doesn't get attributed to the process.
RW=randrw BS=4k IODEPTH=128 P=1 RUNTIME=$RUNTIME perf stat -a -- ./workloads/fio.sh > $OUT/perf_${RW}_${BS}_${IODEPTH}_${P}_${RUNTIME}.out

echo "DONE"

echo If the experiment was successful and the results are verified, please move them to the results folder of the implementation that was used

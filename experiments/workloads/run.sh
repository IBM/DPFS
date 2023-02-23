#!/bin/bash

echo "Welcome to the dpu-virtio-fs workload runner :)"
echo "Please run this script with numactl to bind all the workloads to the NUMA node on which the device is located"

if [ -z $MNT ]; then
	echo "You must set the MNT env variable to where you want to run the workloads"
	exit 1
fi

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
TIMESTAMP=$(date +"%Y-%m-%d_%T")
HOST=$(uname -n)
COMMIT=$(git rev-parse --short HEAD)
OUT=$SCRIPT_DIR/output/${COMMIT}_${HOST}_${TIMESTAMP}

mkdir -p $OUT
echo "The output will be stored under $OUT"

echo "Running: fio latency benchmarks"
for RW in "randread" "randwrite"; do
	for BS in "1" "4k" "8k" "16k" "32k" "64k" "128k"; do
		for IODEPTH in 1; do
			for P in 1; do
				echo fio RW=$RW BS=$BS IODEPTH=$IODEPTH P=$P
				RW=$RW BS=$BS IODEPTH=$IODEPTH P=$P $SCRIPT_DIR/fio.sh > $OUT/fio_${RW}_${BS}_${IODEPTH}_${P}.out
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
				RW=$RW BS=$BS IODEPTH=$IODEPTH P=$P $SCRIPT_DIR/fio.sh > $OUT/fio_${RW}_${BS}_${IODEPTH}_${P}.out
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
				RW=$RW BS=$BS IODEPTH=$IODEPTH P=$P $SCRIPT_DIR/fio.sh > $OUT/fio_${RW}_${BS}_${IODEPTH}_${P}.out
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

echo "DONE"

echo If the experiment was successful and the results are verified, please move them to the results folder of the implementation that was used

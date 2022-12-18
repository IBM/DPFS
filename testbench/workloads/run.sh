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

mkdir $OUT
# fio
echo "The output will be stored under $OUT"
echo "Running: lat.fio"
fio lat.fio > $OUT/lat.fio.out
echo "Running: rand_iops.fio"
fio rand_iops.fio > $OUT/rand_iops.fio.out
echo "Running: seq_tp.fio"
fio seq_tp.fio > $OUT/seq_tp.fio.out

# Single operation latency
echo "Running: stat (getattr) latency"
gcc ./lat/lat_stat.c -O3 -o ./lat/lat_stat
./lat/lat_stat $MNT/test 50000 1 > $OUT/lat_stat.out
echo "Running: statfs latency"
gcc ./lat/lat_statfs.c -O3 -o ./lat/lat_statfs
./lat/lat_statfs $MNT 50000 1 > $OUT/lat_statfs.out

echo "DONE"

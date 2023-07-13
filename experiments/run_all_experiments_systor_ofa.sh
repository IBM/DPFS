#!/bin/bash

echo "Welcome to the dpu-virtio-fs workload runner! :)"
echo "Please run this script with numactl to bind all the workloads to the NUMA node on which the device is located."
echo "This script might bring up a sudo prompt after a long while, make sure you are ready to always enter your password or configure your system to not require a password!"
echo "Furthermore please make sure that any resource hogs have been removed from the system."

if [ -z $MNT ]; then
	echo "You must set the MNT env variable to where you want to run the workloads!"
	exit 1
fi

if perf 2>&1 | grep "WARNING: perf not found for kernel"; then
	exit 1
fi

export DEV=$1
if [[ $DEV != "NFS" && $DEV !=  "VNFS" && $DEV != "nulldev" ]]; then
	echo "You must supply this script with one of the following parameters: NFS, VNFS or nulldev"
	exit 1
fi

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
cd $SCRIPT_DIR

TIMESTAMP=$(date +"%Y-%m-%d_%T")
HOST=$(uname -n)
COMMIT=$(git rev-parse --short HEAD)
export OUT=./output/${COMMIT}_${HOST}_${TIMESTAMP}

gcc setcpulatency.c -o setcpulatency

TIME=$(python3 -c '
import datetime
t = 70 + 2*10*11*70 + 2*10*70 + 2*2*4*70 + 4*5*610
print(datetime.timedelta(seconds=t))
')
echo "The output will be stored under $OUT"

echo "STARTING in 10 seconds! quit the system now to reduce variability!"
echo "This run.sh will take $TIME hours. Only log back in after that amount of time!"
sleep 10
echo "START"

echo Running random r/w mix multicore fio workload for 70 seconds to warm up the system
RW=randrw BS=4k IODEPTH=128 P=4 ./workloads/fio.sh > /dev/null

mkdir -p $OUT

./synthetic_systor_ofa.sh
./host_cpu_analysis.sh

echo "DONE"

echo If the experiment was successful and the results are verified, please move them to the results folder of the implementation that was used

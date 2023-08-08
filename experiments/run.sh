#!/bin/bash

echo "Welcome to the dpu-virtio-fs workload runner! :)"
echo "This script might bring up a sudo prompt after a long while, make sure you are ready to always enter your password or configure your system to not require a password!"
echo "Furthermore please make sure that any resource hogs have been removed from the system."

if [ -z $1 ]; then
	echo "You must supply an experiment runner script!"
	exit 1
fi
EXP=$(realpath $1)

if [ -z $MNT ]; then
	echo "You must set the MNT env variable to where you want to run the workloads!"
	exit 1
fi
export MNT

if [[ -z $NUMA_NODE || -z $NUMA_CORE ]]; then
	echo "You must set the NUMA_NODE and NUMA_CORE env variables to where you want to run the workloads!"
	exit 1
fi
export NUMA_NODE NUMA_CORE

if perf 2>&1 | grep "WARNING: perf not found for kernel"; then
	exit 1
fi

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
cd $SCRIPT_DIR

TIMESTAMP=$(date +"%Y-%m-%d_%T")
HOST=$(uname -n)
COMMIT=$(git rev-parse --short HEAD)
export OUT=./output/${TIMESTAMP}_${HOST}_${COMMIT}

gcc setcpulatency.c -o setcpulatency

echo "STARTING WARMUP in 10 seconds! quit the system now to reduce variability!"
echo "This run.sh will take $TIME hours. Only log back in after that amount of time!"
sleep 10

if [ -z $SKIP_WARMUP ]; then
	echo WARMUP: Running random r/w mix multicore fio workload for 70 seconds to warm up the system
	sudo -E RW=randrw BS=4k QD=128 P=4 ./workloads/fio.sh > /dev/null
fi

echo START: experiment, results will be stored in $OUT
mkdir -p $OUT

eval $EXP

echo "DONE"

echo If the experiment was successful and the results are verified, please move them to the results folder of the implementation that was used

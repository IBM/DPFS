#!/bin/bash

if [[ -z $OUT || -z $MNT ]]; then
	echo OUT and MNT must be defined!
	exit 1
fi
# NUMA defaults, based on ZRL:zac15
NUMA_NODE="${NUMA_NODE:-1}"
NUMA_CORE="${NUMA_CORE:-27}"
REPS="${REPS:-5}"

RESULTS=$OUT/filebench
mkdir -p $RESULTS

FILEBENCHES=./workloads/filebenches
sudo sh -c 'echo 0 > /proc/sys/kernel/randomize_va_space'

for f in $FILEBENCHES/*.f; do
	sed -i -e "s#set \$dir=.*#set \$dir=$MNT#g" $FILEBENCHES/$f
	echo "Running $f"
	mkdir -p $RESULTS/$f

	for i in $(seq 1 $REPS); do
		echo "i=$i"
		sudo numactl -m $NUMA_NODE ~/filebench/filebench -f $FILEBENCHES/$f > $RESULTS/$f/$i
	done
done

sudo sh -c 'echo 1 > /proc/sys/kernel/randomize_va_space'

#!/bin/bash

$FILEBENCHES=./workloads/filebenches
RESULTS=$OUT/filebench
mkdir -p $RESULTS

REPS=5

sudo sh -c 'echo 0 > /proc/sys/kernel/randomize_va_space'

for f in $FILEBENCHES/*.f; do
	sed -i -e "s#set \$dir=.*#set \$dir=$MNT#g" $FILEBENCHES/$f
	echo "Running $f"
	mkdir -p $RESULTS/$f

	for i in $(seq 1 $REPS); do
		echo "i=$i"
		sudo filebench -f $FILEBENCHES/$f > $RESULTS/$f/$i
	done
done

sudo sh -c 'echo 1 > /proc/sys/kernel/randomize_va_space'

#!/bin/bash

if [[ -z $OUT || -z $MNT ]]; then
	echo OUT and MNT must be defined!
	exit 1
fi
# NUMA defaults, based on ZRL:zac15
NUMA_NODE="${NUMA_NODE:-1}"
NUMA_CORE="${NUMA_CORE:-27}"
REPS="${REPS:-5}"

RESULTS=$OUT/redis
mkdir -p $RESULTS

PORT=3248

for i in $(seq 1 $REPS); do
	sudo numactl -m $NUMA_NODE redis-server --port $PORT --appendonly yes --appendfsync everysec --dir $MNT &
	sleep 5
	numactl -m $NUMA_NODE redis-benchmark -t set,get -d 100 -n 1000000 -p $PORT > $RESULTS/redis_$i
	sudo pkill redis-server
	sleep 5
done

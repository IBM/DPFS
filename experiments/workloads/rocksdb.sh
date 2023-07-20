#!/bin/bash

if [[ -z $OUT || -z $MNT ]]; then
	echo OUT and MNT must be defined!
	exit 1
fi
# NUMA defaults, based on ZRL:zac15
NUMA_NODE="${NUMA_NODE:-1}"
NUMA_CORE="${NUMA_CORE:-27}"
REPS="${REPS:-5}"

RESULTS=$OUT/rocksdb
mkdir -p $RESULTS

for i in $(seq 1 $REPS); do
	sudo numactl -m $NUMA_NODE ~/rocksdb/db_bench --benchmarks fillrandom,readrandom --num 1000000 --value_size 100 --threads 8 \
	                --db=$MNT --disable_wal=0 --wal_dir=$MNT > $RESULTS/rocksdb_$i
done
